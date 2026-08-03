import {
  toBytesAsync,
  type AsyncByteSource,
  type ByteMap,
  type ByteMapInput,
  randomId,
} from './bytes.js';
import { Deferred } from './concurrency.js';
import { WireMessage } from './data.js';
import {
  failedPreconditionError,
  invalidArgumentError,
  isOk,
  okStatus,
  resourceExhaustedError,
  statusCodeFromWebSocket,
  statusFromUnknown,
  unavailableError,
  unimplementedError,
  type Status,
  type StatusOr,
} from './status.js';
import {
  ChannelEndpointRole,
  ChannelWireStream,
  type BinaryChannel,
  type BinaryChannelCallbacks,
  type ChannelFramingOptions,
} from './channel_wire_stream.js';
import {
  type OnWireDone,
  type OnWireMessage,
  type WireDeadline,
  type WireStream,
  type WireStreamOptions,
} from './wire_stream.js';

interface WebSocketEventLike {
  data?: unknown;
  code?: number;
  reason?: string;
}

export interface WebSocketLike {
  readonly readyState: number;
  readonly bufferedAmount: number;
  binaryType: string;
  addEventListener(
    type: 'open' | 'message' | 'error' | 'close',
    listener: (event: WebSocketEventLike) => void,
  ): void;
  send(data: Uint8Array | string): void;
  close(code?: number, reason?: string): void;
}

export type WebSocketFactory = (
  url: string,
  protocols: string | readonly string[] | undefined,
  headers: Readonly<Record<string, string>>,
) => WebSocketLike | Promise<WebSocketLike>;

export interface WebSocketClientOptions {
  headers?: Readonly<Record<string, string>>;
  protocols?: string | readonly string[];
  framing?: ChannelFramingOptions;
  maxBufferedAmount?: number;
  bufferedAmountLowThreshold?: number;
  webSocketFactory?: WebSocketFactory;
}

interface NormalizedWebSocketClientOptions {
  headers: Readonly<Record<string, string>>;
  protocols: string | readonly string[] | undefined;
  framing: ChannelFramingOptions;
  maxBufferedAmount: number;
  bufferedAmountLowThreshold: number;
  webSocketFactory: WebSocketFactory | undefined;
}

function normalizeWebSocketOptions(
  options: WebSocketClientOptions = {},
): StatusOr<NormalizedWebSocketClientOptions> {
  try {
    if (typeof options !== 'object' || options === null) {
      return invalidArgumentError('WebSocket options must be an object.');
    }
    const headers: Record<string, string> = {};
    for (const [rawName, value] of Object.entries(options.headers ?? {})) {
      const name = rawName.toLowerCase();
      if (!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(name)) {
        return invalidArgumentError(`Invalid WebSocket header name: ${rawName}.`);
      }
      if (typeof value !== 'string' || /[\r\n\0]/.test(value)) {
        return invalidArgumentError(
          `WebSocket header ${rawName} must be a safe string.`,
        );
      }
      headers[name] = value;
    }
    const protocols = options.protocols;
    if (
      protocols !== undefined &&
      typeof protocols !== 'string' &&
      (!Array.isArray(protocols) ||
        protocols.some((protocol) => typeof protocol !== 'string'))
    ) {
      return invalidArgumentError('protocols must be a string or string array.');
    }
    const maxBufferedAmount = options.maxBufferedAmount ?? 16 * 1024 * 1024;
    const bufferedAmountLowThreshold =
      options.bufferedAmountLowThreshold ?? 64 * 1024;
    if (!Number.isSafeInteger(maxBufferedAmount) || maxBufferedAmount <= 0) {
      return invalidArgumentError('maxBufferedAmount must be a positive integer.');
    }
    if (
      !Number.isSafeInteger(bufferedAmountLowThreshold) ||
      bufferedAmountLowThreshold < 0 ||
      bufferedAmountLowThreshold > maxBufferedAmount
    ) {
      return invalidArgumentError(
        'bufferedAmountLowThreshold must be in [0, maxBufferedAmount].',
      );
    }
    if (
      options.webSocketFactory !== undefined &&
      typeof options.webSocketFactory !== 'function'
    ) {
      return invalidArgumentError('webSocketFactory must be callable.');
    }
    return {
      headers: Object.freeze(headers),
      protocols,
      framing: { ...(options.framing ?? {}) },
      maxBufferedAmount,
      bufferedAmountLowThreshold,
      webSocketFactory: options.webSocketFactory,
    };
  } catch (error) {
    return invalidArgumentError('Could not normalize WebSocket options.', [], error);
  }
}

function hasWebSocketShape(value: unknown): value is WebSocketLike {
  if (typeof value !== 'object' || value === null) return false;
  try {
    const candidate = value as Record<string, unknown>;
    return typeof candidate.addEventListener === 'function' &&
      typeof candidate.send === 'function' &&
      typeof candidate.close === 'function';
  } catch {
    return false;
  }
}

function validateWebSocketUrl(url: string): Status {
  if (typeof url !== 'string') {
    return invalidArgumentError('WebSocket URL must be a string.');
  }
  try {
    const parsed = new URL(url);
    if (parsed.protocol !== 'ws:' && parsed.protocol !== 'wss:') {
      return invalidArgumentError('WebSocket URL must start with ws:// or wss://.');
    }
    if (!parsed.hostname) {
      return invalidArgumentError('WebSocket URL host must not be empty.');
    }
    return okStatus();
  } catch (error) {
    return invalidArgumentError('WebSocket URL is invalid.', [], error);
  }
}

function hasBrowserWebSocket(): boolean {
  return typeof globalThis.WebSocket === 'function';
}

class WebSocketBinaryChannel implements BinaryChannel {
  private callbacks: BinaryChannelCallbacks | null = null;
  private socket: WebSocketLike | null = null;
  private opening: Deferred<Status> | null = null;
  private messageChain: Promise<void> = Promise.resolve();
  private closed = false;
  private readonly lowWaiters: Array<Deferred<Status>> = [];
  private pollTimer: ReturnType<typeof setTimeout> | null = null;

  constructor(
    private readonly url: string,
    private readonly options: NormalizedWebSocketClientOptions,
  ) {}

  setCallbacks(callbacks: BinaryChannelCallbacks): Status {
    this.callbacks = callbacks;
    return okStatus();
  }

  resetCallbacks(): Status {
    this.callbacks = null;
    return okStatus();
  }

  async open(): Promise<Status> {
    try {
      if (this.isOpen()) return okStatus();
      if (this.opening !== null) return this.opening.promise;
      if (this.closed) {
        return failedPreconditionError('WebSocket channel is closed.');
      }
      this.opening = new Deferred<Status>();
      const socket = await this.makeSocket();
      if (!isOk(socket)) {
        this.opening.resolve(socket);
        return socket;
      }
      if (!hasWebSocketShape(socket)) {
        const status = invalidArgumentError(
          'WebSocket factory returned an invalid WebSocket object.',
        );
        this.opening.resolve(status);
        return status;
      }
      this.socket = socket;
      socket.binaryType = 'arraybuffer';
      socket.addEventListener('open', () => this.handleOpen());
      socket.addEventListener('message', (event) => {
        try { this.handleMessage(event.data); }
        catch (error) {
          this.signalCallbackError(error, 'Reading WebSocket message event failed.');
        }
      });
      socket.addEventListener('error', () => this.handleError());
      socket.addEventListener('close', (event) => {
        try { this.handleClose(event.code ?? 1006, event.reason ?? ''); }
        catch (error) {
          this.signalCallbackError(error, 'Reading WebSocket close event failed.');
        }
      });
      if (socket.readyState === 1) this.handleOpen();
    } catch (error) {
      const status = statusFromUnknown(
        error,
        'Creating WebSocket raised an exception.',
      );
      this.opening?.resolve(status);
    }
    return this.opening?.promise ?? Promise.resolve(
      unavailableError('WebSocket startup did not initialize.'),
    );
  }

  isOpen(): boolean {
    try { return this.socket?.readyState === 1 && !this.closed; }
    catch { return false; }
  }

  send(packet: Uint8Array): Status {
    try {
      if (!(packet instanceof Uint8Array)) {
        return invalidArgumentError('WebSocket packet must be a Uint8Array.');
      }
      const socket = this.socket;
      if (socket === null || socket.readyState !== 1 || this.closed) {
        return failedPreconditionError('WebSocket is not open.');
      }
      const bufferedAmount = socket.bufferedAmount;
      if (!Number.isFinite(bufferedAmount) || bufferedAmount < 0) {
        return unavailableError('WebSocket reported an invalid bufferedAmount.');
      }
      if (
        bufferedAmount + packet.byteLength >
        this.options.maxBufferedAmount
      ) {
        return resourceExhaustedError(
          'WebSocket buffered amount would exceed maxBufferedAmount.',
        );
      }
      socket.send(packet);
      this.scheduleLowPoll();
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'WebSocket send raised an exception.');
    }
  }

  bufferedAmount(): StatusOr<number> {
    const socket = this.socket;
    if (socket === null) return failedPreconditionError('WebSocket is not created.');
    try {
      const amount = socket.bufferedAmount;
      return Number.isFinite(amount) && amount >= 0
        ? amount
        : unavailableError('WebSocket reported an invalid bufferedAmount.');
    } catch (error) {
      return statusFromUnknown(
        error,
        'Reading WebSocket bufferedAmount raised an exception.',
      );
    }
  }

  waitForBufferedAmountLow(): Promise<Status> {
    const amount = this.bufferedAmount();
    if (!isOk(amount)) return Promise.resolve(amount);
    if (amount <= this.options.bufferedAmountLowThreshold) {
      return Promise.resolve(okStatus());
    }
    const waiter = new Deferred<Status>();
    this.lowWaiters.push(waiter);
    this.scheduleLowPoll();
    return waiter.promise;
  }

  close(): Status {
    try {
      if (this.closed) return okStatus();
      this.closed = true;
      if (this.pollTimer !== null) clearTimeout(this.pollTimer);
      this.pollTimer = null;
      for (const waiter of this.lowWaiters.splice(0)) {
        waiter.resolve(failedPreconditionError('WebSocket was closed.'));
      }
      const socket = this.socket;
      if (socket === null || socket.readyState === 3) return okStatus();
      socket.close(1000, 'A11 stream complete');
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'WebSocket close raised an exception.');
    }
  }

  getImpl(): unknown | null {
    return this.socket;
  }

  private async makeSocket(): Promise<StatusOr<WebSocketLike>> {
    if (this.options.webSocketFactory !== undefined) {
      try {
        return await this.options.webSocketFactory(
          this.url,
          this.options.protocols,
          this.options.headers,
        );
      } catch (error) {
        return statusFromUnknown(
          error,
          'Custom WebSocket factory raised an exception.',
        );
      }
    }

    const hasHeaders = Object.keys(this.options.headers).length > 0;
    const isNode =
      typeof process !== 'undefined' &&
      process.versions?.node !== undefined;
    if (hasBrowserWebSocket() && !(isNode && hasHeaders)) {
      if (hasHeaders && !isNode) {
        return unimplementedError(
          'Browser WebSocket does not support custom HTTP headers; use a query parameter or subprotocol.',
        );
      }
      try {
        const protocols = this.options.protocols;
        const socket = protocols === undefined
          ? new globalThis.WebSocket(this.url)
          : new globalThis.WebSocket(this.url, protocols as string | string[]);
        return socket as unknown as WebSocketLike;
      } catch (error) {
        return statusFromUnknown(error, 'Browser WebSocket construction failed.');
      }
    }

    try {
      const module = await import('ws');
      const protocols = this.options.protocols;
      const socket = protocols === undefined
        ? new module.WebSocket(this.url, { headers: this.options.headers })
        : new module.WebSocket(
            this.url,
            protocols as string | string[],
            { headers: this.options.headers },
          );
      return socket as unknown as WebSocketLike;
    } catch (error) {
      return statusFromUnknown(error, 'Node.js WebSocket construction failed.');
    }
  }

  private handleOpen(): void {
    if (this.closed) return;
    this.opening?.resolve(okStatus());
    try {
      this.callbacks?.onOpen();
    } catch (error) {
      this.signalCallbackError(error, 'WebSocket open callback raised an exception.');
    }
  }

  private handleMessage(data: unknown): void {
    this.messageChain = this.messageChain.then(async () => {
      const source = this.asByteSource(data);
      if (!isOk(source)) {
        this.callbacks?.onError(source);
        return;
      }
      const bytes = await toBytesAsync(source);
      if (!isOk(bytes)) {
        this.callbacks?.onError(bytes);
        return;
      }
      try {
        this.callbacks?.onMessage(bytes);
      } catch (error) {
        this.signalCallbackError(
          error,
          'WebSocket message callback raised an exception.',
        );
      }
    }).catch((error: unknown) => {
      this.signalCallbackError(error, 'WebSocket message processing failed.');
    });
  }

  private asByteSource(data: unknown): StatusOr<AsyncByteSource> {
    if (
      data instanceof ArrayBuffer ||
      ArrayBuffer.isView(data) ||
      (typeof Blob !== 'undefined' && data instanceof Blob)
    ) {
      return data as AsyncByteSource;
    }
    return invalidArgumentError('A11 WebSocket messages must be binary.');
  }

  private handleError(): void {
    if (this.closed) return;
    const status = unavailableError('WebSocket transport reported an error.');
    this.opening?.resolve(status);
    try {
      this.callbacks?.onError(status);
    } catch {
      // There is no further callback boundary after a transport error.
    }
  }

  private handleClose(code: number, reason: string): void {
    if (!Number.isInteger(code)) code = 1006;
    if (typeof reason !== 'string') reason = '';
    const wasOpening = this.opening !== null && !this.opening.settled;
    this.closed = true;
    if (wasOpening) {
      this.opening?.resolve({
        code: statusCodeFromWebSocket(code),
        message: reason || `WebSocket closed during startup (${code}).`,
      } as Status);
    }
    for (const waiter of this.lowWaiters.splice(0)) {
      waiter.resolve(unavailableError('WebSocket closed before its buffer drained.'));
    }
    try {
      this.callbacks?.onClosed();
    } catch {
      // ChannelWireStream will observe the close independently.
    }
  }

  private signalCallbackError(error: unknown, message: string): void {
    const status = statusFromUnknown(error, message);
    try {
      this.callbacks?.onError(status);
    } catch {
      // Avoid an exception escaping an event listener.
    }
  }

  private scheduleLowPoll(): void {
    if (this.pollTimer !== null || this.lowWaiters.length === 0) return;
    this.pollTimer = setTimeout(() => {
      this.pollTimer = null;
      const amount = this.bufferedAmount();
      if (!isOk(amount)) {
        for (const waiter of this.lowWaiters.splice(0)) waiter.resolve(amount);
        return;
      }
      if (amount <= this.options.bufferedAmountLowThreshold) {
        for (const waiter of this.lowWaiters.splice(0)) waiter.resolve(okStatus());
        try {
          this.callbacks?.onBufferedAmountLow();
        } catch {
          // The sender can still directly observe bufferedAmount.
        }
        return;
      }
      this.scheduleLowPoll();
    }, 4);
  }
}

/** Client-side A11 WireStream over an isomorphic WebSocket. */
export class WebSocketWireStream implements WireStream {
  private constructor(private readonly stream: ChannelWireStream) {}

  static createClient(
    url: string,
    options: WireStreamOptions = {},
    websocketOptions: WebSocketClientOptions = {},
  ): StatusOr<WebSocketWireStream> {
    try {
      const validUrl = validateWebSocketUrl(url);
      if (!isOk(validUrl)) return validUrl;
      const normalized = normalizeWebSocketOptions(websocketOptions);
      if (!isOk(normalized)) return normalized;
      const channel = new WebSocketBinaryChannel(url, normalized);
      const stream = ChannelWireStream.create(
        channel,
        randomId('ws-'),
        ChannelEndpointRole.CLIENT,
        options,
        normalized.framing,
      );
      return isOk(stream) ? new WebSocketWireStream(stream) : stream;
    } catch (error) {
      return statusFromUnknown(error, 'Creating WebSocketWireStream raised an exception.');
    }
  }

  static connect(
    url: string,
    options: WireStreamOptions = {},
    websocketOptions: WebSocketClientOptions = {},
  ): StatusOr<WebSocketWireStream> {
    return WebSocketWireStream.createClient(url, options, websocketOptions);
  }

  send(message: WireMessage): Status { return this.stream.send(message); }
  start(onMessage?: OnWireMessage, onDone?: OnWireDone): Promise<Status> {
    return this.stream.start(onMessage, onDone);
  }
  accept(onMessage?: OnWireMessage, onDone?: OnWireDone): Promise<Status> {
    return this.stream.accept(onMessage, onDone);
  }
  halfClose(trailers?: ByteMapInput): Status { return this.stream.halfClose(trailers); }
  drainOutgoingMessages(): Promise<Status> { return this.stream.drainOutgoingMessages(); }
  abort(status: Status): Status { return this.stream.abort(status); }
  setDeadline(deadline?: WireDeadline): Status { return this.stream.setDeadline(deadline); }
  getDeadline(): number | null { return this.stream.getDeadline(); }
  getStatus(): Status { return this.stream.getStatus(); }
  getTrailers(): ByteMap | null { return this.stream.getTrailers(); }
  getId(): string { return this.stream.getId(); }
  getImpl(): unknown | null { return this.stream.getImpl(); }
  wait(): Promise<Status> { return this.stream.wait(); }
}
