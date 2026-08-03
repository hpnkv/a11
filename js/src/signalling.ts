import { toBytesAsync, utf8Decode } from './bytes.js';
import { Deferred } from './concurrency.js';
import { validateName } from './data.js';
import {
  cancelledError,
  deadlineExceededError,
  failedPreconditionError,
  internalError,
  invalidArgumentError,
  isOk,
  isStatus,
  okStatus,
  permissionDeniedError,
  StatusCode,
  statusFromUnknown,
  statusToJson,
  unavailableError,
  unimplementedError,
  type NonOkStatus,
  type Status,
  type StatusOr,
} from './status.js';
import type { WebSocketFactory, WebSocketLike } from './websocket_wire_stream.js';

export enum SignallingMessageType {
  DESCRIPTION = 'description',
  CANDIDATE = 'candidate',
  ERROR = 'error',
}

export interface SignallingMessageOptions {
  type?: SignallingMessageType;
  sender?: string;
  recipient?: string;
  description?: string;
  descriptionType?: RTCSdpType | '';
  candidate?: string;
  mid?: string;
  error?: Status;
}

/** SDP, ICE, or error control message used to negotiate WebRTC. */
export class SignallingMessage {
  type: SignallingMessageType;
  sender: string;
  recipient: string;
  description: string;
  descriptionType: RTCSdpType | '';
  candidate: string;
  mid: string;
  error: Status;

  constructor(options: SignallingMessageOptions = {}) {
    this.type = options.type ?? SignallingMessageType.DESCRIPTION;
    this.sender = options.sender ?? '';
    this.recipient = options.recipient ?? '';
    this.description = options.description ?? '';
    this.descriptionType = options.descriptionType ?? '';
    this.candidate = options.candidate ?? '';
    this.mid = options.mid ?? '';
    this.error = options.error ?? okStatus();
  }

  static create(options: SignallingMessageOptions = {}): StatusOr<SignallingMessage> {
    try {
      if (typeof options !== 'object' || options === null) {
        return invalidArgumentError('Signalling options must be an object.');
      }
      const result = new SignallingMessage(options);
      const validation = result.validate();
      return isOk(validation) ? result : validation;
    } catch (error) {
      return invalidArgumentError('Signalling options could not be read.', [], error);
    }
  }

  validate(): Status {
    try {
      const sender = validateName(this.sender);
      if (!isOk(sender)) return sender;
      const recipient = validateName(this.recipient);
      if (!isOk(recipient)) return recipient;
      if (
        typeof this.description !== 'string' ||
        typeof this.descriptionType !== 'string' ||
        typeof this.candidate !== 'string' ||
        typeof this.mid !== 'string'
      ) {
        return invalidArgumentError('Signalling message fields must be strings.');
      }
      if (!isStatus(this.error)) {
        return invalidArgumentError('Signalling message error must be an A11 Status.');
      }
      if (this.type === SignallingMessageType.DESCRIPTION) {
        if (this.description.length === 0) {
          return invalidArgumentError('A signalling description must not be empty.');
        }
        if (!['offer', 'answer', 'pranswer', 'rollback'].includes(this.descriptionType)) {
          return invalidArgumentError('A signalling description has an invalid SDP type.');
        }
      } else if (this.type === SignallingMessageType.CANDIDATE) {
        if (this.candidate.length === 0) {
          return invalidArgumentError('A signalling ICE candidate must not be empty.');
        }
      } else if (this.type === SignallingMessageType.ERROR) {
        if (isOk(this.error)) {
          return invalidArgumentError(
            'A signalling error message must contain a non-OK status.',
          );
        }
      } else {
        return invalidArgumentError('Unknown signalling message type.');
      }
      return okStatus();
    } catch (error) {
      return invalidArgumentError('Signalling message could not be validated.', [], error);
    }
  }

  toJsonValue(): StatusOr<Record<string, unknown>> {
    const validation = this.validate();
    if (!isOk(validation)) return validation;
    const result: Record<string, unknown> = {
      type: this.type,
      from: this.sender,
      to: this.recipient,
    };
    if (this.type === SignallingMessageType.DESCRIPTION) {
      result.type = this.descriptionType;
      result.id = this.recipient;
      result.description = this.description;
      result.description_type = this.descriptionType;
    } else if (this.type === SignallingMessageType.CANDIDATE) {
      result.id = this.recipient;
      result.candidate = this.candidate;
      result.mid = this.mid;
    } else {
      result.status = statusToJson(this.error);
    }
    return result;
  }

  toJson(): StatusOr<string> {
    const value = this.toJsonValue();
    if (!isOk(value)) return value;
    try {
      return JSON.stringify(value);
    } catch (error) {
      return internalError('Failed to encode signalling JSON.', [], error);
    }
  }

  static fromJson(encoded: string): StatusOr<SignallingMessage> {
    if (typeof encoded !== 'string') {
      return invalidArgumentError('Signalling JSON must be a string.');
    }
    try {
      return SignallingMessage.fromJsonValue(JSON.parse(encoded));
    } catch (error) {
      return invalidArgumentError('Failed to parse signalling JSON.', [], error);
    }
  }

  static fromJsonValue(value: unknown): StatusOr<SignallingMessage> {
    try {
      return SignallingMessage.fromJsonValueUnchecked(value);
    } catch (error) {
      return invalidArgumentError('Signalling JSON object could not be read.', [], error);
    }
  }

  private static fromJsonValueUnchecked(
    value: unknown,
  ): StatusOr<SignallingMessage> {
    if (typeof value !== 'object' || value === null || Array.isArray(value)) {
      return invalidArgumentError('A signalling message must be a JSON object.');
    }
    const object = value as Record<string, unknown>;
    if (typeof object.type !== 'string') {
      return invalidArgumentError('A signalling message requires a string type.');
    }
    let type: SignallingMessageType;
    if (
      ['description', 'offer', 'answer', 'pranswer', 'rollback'].includes(
        object.type,
      )
    ) {
      type = SignallingMessageType.DESCRIPTION;
    } else if (object.type === 'candidate') {
      type = SignallingMessageType.CANDIDATE;
    } else if (object.type === 'error') {
      type = SignallingMessageType.ERROR;
    } else {
      return invalidArgumentError(`Unknown signalling message type: ${object.type}.`);
    }
    const sender = typeof object.from === 'string' ? object.from : '';
    const recipient = typeof object.to === 'string'
      ? object.to
      : typeof object.id === 'string'
        ? object.id
        : '';
    const result = new SignallingMessage({ type, sender, recipient });
    if (type === SignallingMessageType.DESCRIPTION) {
      result.description = typeof object.description === 'string'
        ? object.description
        : '';
      result.descriptionType = (
        typeof object.description_type === 'string'
          ? object.description_type
          : object.type
      ) as RTCSdpType;
    } else if (type === SignallingMessageType.CANDIDATE) {
      result.candidate = typeof object.candidate === 'string' ? object.candidate : '';
      result.mid = typeof object.mid === 'string' ? object.mid : '';
    } else {
      const parsed = decodeStatusJson(object.status);
      if (!isOk(parsed)) return parsed;
      result.error = parsed.status;
    }
    const validation = result.validate();
    return isOk(validation) ? result : validation;
  }
}

interface DecodedJsonStatus { status: Status }

function decodeStatusJson(value: unknown): StatusOr<DecodedJsonStatus> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    return invalidArgumentError('Signalling error requires a Status object.');
  }
  const object = value as Record<string, unknown>;
  if (
    !Number.isInteger(object.code) ||
    (object.code as number) < StatusCode.OK ||
    (object.code as number) > StatusCode.UNAUTHENTICATED ||
    typeof object.message !== 'string'
  ) {
    return invalidArgumentError('Signalling error contains an invalid Status.');
  }
  const details = object.details ?? [];
  if (
    !Array.isArray(details) ||
    details.some((detail) =>
      typeof detail !== 'object' || detail === null || Array.isArray(detail))
  ) {
    return invalidArgumentError('Signalling Status details must be objects.');
  }
  return {
    status: {
      code: object.code as StatusCode,
      message: object.message,
      details: details as object[],
    } as Status,
  };
}

export type OnSignallingMessage = (
  message: SignallingMessage,
) => void | Status | Promise<void | Status>;

export interface SignallingTransport {
  send(message: SignallingMessage): Status;
  setOnMessage(onMessage: OnSignallingMessage): Status;
  close(): Status;
  getIdentity(): string;
  isConnected(): boolean;
  getStatus(): Status;
  getImpl?(): unknown | null;
}

export interface WebSocketSignallingClientOptions {
  deadline?: Date | number | null;
  maxMessageSize?: number;
  headers?: Readonly<Record<string, string>>;
  protocols?: string | readonly string[];
  webSocketFactory?: WebSocketFactory;
}

/** Network signalling client used by the browser WebRTC transport. */
export class WebSocketSignallingClient implements SignallingTransport {
  private socket: WebSocketLike;
  private callback: OnSignallingMessage;
  private connectedInternal = true;
  private status: Status = okStatus();
  private messageChain: Promise<void> = Promise.resolve();

  private constructor(
    socket: WebSocketLike,
    private readonly identityValue: string,
    callback: OnSignallingMessage,
    private readonly maxMessageSize: number,
  ) {
    this.socket = socket;
    this.callback = callback;
  }

  static async connect(
    url: string,
    identity: string,
    onMessage: OnSignallingMessage = () => undefined,
    options: WebSocketSignallingClientOptions = {},
  ): Promise<StatusOr<WebSocketSignallingClient>> {
    try {
      return await WebSocketSignallingClient.connectUnchecked(
        url,
        identity,
        onMessage,
        options,
      );
    } catch (error) {
      return statusFromUnknown(
        error,
        'Connecting signalling WebSocket raised an exception.',
      );
    }
  }

  private static async connectUnchecked(
    url: string,
    identity: string,
    onMessage: OnSignallingMessage,
    options: WebSocketSignallingClientOptions,
  ): Promise<StatusOr<WebSocketSignallingClient>> {
    const validIdentity = validateName(identity);
    if (!isOk(validIdentity)) return validIdentity;
    if (typeof onMessage !== 'function') {
      return invalidArgumentError('onMessage must be callable.');
    }
    const maxMessageSize = options.maxMessageSize ?? 1024 * 1024;
    if (!Number.isSafeInteger(maxMessageSize) || maxMessageSize <= 0) {
      return invalidArgumentError('maxMessageSize must be a positive integer.');
    }
    const deadline = options.deadline === undefined || options.deadline === null
      ? null
      : options.deadline instanceof Date
        ? options.deadline.getTime()
        : options.deadline;
    if (deadline !== null && !Number.isFinite(deadline)) {
      return invalidArgumentError('deadline must be a finite Date, number, or null.');
    }
    if (deadline !== null && deadline <= Date.now()) {
      return deadlineExceededError(
        'WebSocket signalling connect deadline exceeded.',
      );
    }
    const identityUrl = makeIdentityUrl(url, identity);
    if (!isOk(identityUrl)) return identityUrl;
    const headers = normalizeStringHeaders(options.headers ?? {});
    if (!isOk(headers)) return headers;
    const socket = await createSignallingSocket(
      identityUrl,
      options.protocols,
      headers,
      options.webSocketFactory,
    );
    if (!isOk(socket)) return socket;
    if (!hasSignallingSocketShape(socket)) {
      return invalidArgumentError(
        'WebSocket factory returned an invalid signalling socket.',
      );
    }
    const opened = new Deferred<Status>();
    let timer: ReturnType<typeof setTimeout> | null = null;
    let client: WebSocketSignallingClient | null = null;
    const pendingMessages: unknown[] = [];
    try {
      socket.addEventListener('open', () => opened.resolve(okStatus()));
      socket.addEventListener('message', (event) => {
        try {
          const data = event.data;
          if (client === null) pendingMessages.push(data);
          else client.handleRaw(data);
        } catch (error) {
          const status = statusFromUnknown(
            error,
            'Reading signalling WebSocket message raised an exception.',
          );
          if (client === null) opened.resolve(status);
          else client.fail(status);
        }
      });
      socket.addEventListener('error', () => {
        const status = unavailableError(
          'Signalling WebSocket reported an error.',
        );
        if (client === null) opened.resolve(status);
        else client.fail(status);
      });
      socket.addEventListener('close', (event) => {
        try {
          if (client === null) {
            opened.resolve(
              unavailableError(
                event.reason || 'Signalling WebSocket closed during startup.',
              ),
            );
          } else {
            client.fail(unavailableError('Signalling WebSocket was closed.'));
          }
        } catch (error) {
          const status = statusFromUnknown(
            error,
            'Handling signalling WebSocket close raised an exception.',
          );
          if (client === null) opened.resolve(status);
          else client.fail(status);
        }
      });
      if (deadline !== null) {
        timer = setTimeout(
          () => opened.resolve(
            deadlineExceededError('WebSocket signalling connect deadline exceeded.'),
          ),
          Math.max(0, deadline - Date.now()),
        );
      }
      if (socket.readyState === 1) opened.resolve(okStatus());
      const status = await opened.promise;
      if (timer !== null) clearTimeout(timer);
      if (!isOk(status)) {
        try { socket.close(); } catch { /* preserve connect status */ }
        return status;
      }
      client = new WebSocketSignallingClient(
        socket,
        identity,
        onMessage,
        maxMessageSize,
      );
      for (const data of pendingMessages) client.handleRaw(data);
      return client;
    } catch (error) {
      if (timer !== null) clearTimeout(timer);
      try { socket.close(); } catch { /* preserve primary status */ }
      return statusFromUnknown(
        error,
        'Configuring signalling WebSocket raised an exception.',
      );
    }
  }

  send(message: SignallingMessage): Status {
    try {
      if (!this.connectedInternal || this.socket.readyState !== 1) {
        return isOk(this.status)
          ? failedPreconditionError('Signalling WebSocket is not connected.')
          : this.status;
      }
      if (!(message instanceof SignallingMessage)) {
        return invalidArgumentError('message must be a SignallingMessage.');
      }
      if (message.sender !== '' && message.sender !== this.identityValue) {
        return permissionDeniedError(
          'A signalling client cannot impersonate another identity.',
        );
      }
      const outgoing = new SignallingMessage({
        ...message,
        sender: this.identityValue,
      });
      const encoded = outgoing.toJson();
      if (!isOk(encoded)) return encoded;
      if (new TextEncoder().encode(encoded).byteLength > this.maxMessageSize) {
        return invalidArgumentError('Signalling message exceeds maxMessageSize.');
      }
      this.socket.send(encoded);
      return okStatus();
    } catch (error) {
      const status = statusFromUnknown(
        error,
        'Sending signalling WebSocket message raised an exception.',
      );
      this.fail(status);
      return status;
    }
  }

  setOnMessage(onMessage: OnSignallingMessage): Status {
    if (typeof onMessage !== 'function') {
      return invalidArgumentError('onMessage must be callable.');
    }
    if (!this.connectedInternal) return this.status;
    this.callback = onMessage;
    return okStatus();
  }

  close(): Status {
    if (!this.connectedInternal) return okStatus();
    this.connectedInternal = false;
    this.status = cancelledError('Signalling WebSocket closed.');
    try {
      this.socket.close(1000, 'A11 signalling complete');
      return okStatus();
    } catch (error) {
      return statusFromUnknown(
        error,
        'Closing signalling WebSocket raised an exception.',
      );
    }
  }

  getIdentity(): string { return this.identityValue; }
  isConnected(): boolean {
    try { return this.connectedInternal && this.socket.readyState === 1; }
    catch (error) {
      this.fail(statusFromUnknown(
        error,
        'Reading signalling WebSocket state raised an exception.',
      ));
      return false;
    }
  }
  getStatus(): Status { return this.status; }
  getImpl(): unknown | null { return this.socket; }

  private handleRaw(data: unknown): void {
    this.messageChain = this.messageChain.then(async () => {
      let encoded: string;
      if (typeof data === 'string') {
        encoded = data;
      } else if (
        data instanceof ArrayBuffer ||
        ArrayBuffer.isView(data) ||
        (typeof Blob !== 'undefined' && data instanceof Blob)
      ) {
        const bytes = await toBytesAsync(data);
        if (!isOk(bytes)) { this.fail(bytes); return; }
        if (bytes.byteLength > this.maxMessageSize) {
          this.fail(invalidArgumentError('Signalling message exceeds maxMessageSize.'));
          return;
        }
        const text = utf8Decode(bytes);
        if (!isOk(text)) { this.fail(text); return; }
        encoded = text;
      } else {
        this.fail(invalidArgumentError('Signalling WebSocket message must be text.'));
        return;
      }
      if (new TextEncoder().encode(encoded).byteLength > this.maxMessageSize) {
        this.fail(invalidArgumentError('Signalling message exceeds maxMessageSize.'));
        return;
      }
      const message = SignallingMessage.fromJson(encoded);
      if (!isOk(message)) { this.fail(message); return; }
      if (message.recipient !== '' && message.recipient !== this.identityValue) {
        this.fail(permissionDeniedError('Signalling message has the wrong recipient.'));
        return;
      }
      let status: Status;
      try {
        const result = await this.callback(message);
        status = result === undefined
          ? okStatus()
          : isStatus(result)
            ? result
            : internalError(
                'WebSocket signalling callback returned a non-Status value.',
              );
      } catch (error) {
        status = statusFromUnknown(
          error,
          'WebSocket signalling callback raised an exception.',
        );
      }
      if (!isOk(status)) this.fail(status);
    }).catch((error: unknown) => {
      this.fail(statusFromUnknown(error, 'Signalling message pump failed.'));
    });
  }

  private fail(status: NonOkStatus): void {
    if (!this.connectedInternal) return;
    this.connectedInternal = false;
    this.status = status;
    try { this.socket.close(); } catch { /* preserve primary status */ }
  }
}

function makeIdentityUrl(url: string, identity: string): StatusOr<string> {
  if (typeof url !== 'string' || (!url.startsWith('ws://') && !url.startsWith('wss://'))) {
    return invalidArgumentError(
      'WebSocket signalling URL must start with ws:// or wss://.',
    );
  }
  try {
    const result = url.includes('{id}')
      ? url.replace('{id}', encodeURIComponent(identity))
      : `${url.endsWith('/') ? url : `${url}/`}${encodeURIComponent(identity)}`;
    new URL(result);
    return result;
  } catch (error) {
    return invalidArgumentError('WebSocket signalling URL is invalid.', [], error);
  }
}

function normalizeStringHeaders(
  values: Readonly<Record<string, string>>,
): StatusOr<Readonly<Record<string, string>>> {
  const result: Record<string, string> = {};
  try {
    for (const [rawName, value] of Object.entries(values)) {
      const name = rawName.toLowerCase();
      if (!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(name) || typeof value !== 'string' || /[\r\n\0]/.test(value)) {
        return invalidArgumentError(`Invalid WebSocket header: ${rawName}.`);
      }
      result[name] = value;
    }
    return Object.freeze(result);
  } catch (error) {
    return statusFromUnknown(error, 'Could not normalize WebSocket headers.');
  }
}

async function createSignallingSocket(
  url: string,
  protocols: string | readonly string[] | undefined,
  headers: Readonly<Record<string, string>>,
  factory?: WebSocketFactory,
): Promise<StatusOr<WebSocketLike>> {
  if (factory !== undefined) {
    try { return await factory(url, protocols, headers); }
    catch (error) { return statusFromUnknown(error, 'WebSocket factory raised an exception.'); }
  }
  const hasHeaders = Object.keys(headers).length > 0;
  const isNode = typeof process !== 'undefined' && process.versions?.node !== undefined;
  if (typeof globalThis.WebSocket === 'function' && !(isNode && hasHeaders)) {
    if (hasHeaders && !isNode) {
      return unimplementedError('Browser WebSocket does not support custom HTTP headers.');
    }
    try {
      return (protocols === undefined
        ? new globalThis.WebSocket(url)
        : new globalThis.WebSocket(url, protocols as string | string[])) as unknown as WebSocketLike;
    } catch (error) {
      return statusFromUnknown(error, 'Browser WebSocket construction failed.');
    }
  }
  try {
    const module = await import('ws');
    return (protocols === undefined
      ? new module.WebSocket(url, { headers })
      : new module.WebSocket(url, protocols as string | string[], { headers })) as unknown as WebSocketLike;
  } catch (error) {
    return statusFromUnknown(error, 'Node.js WebSocket construction failed.');
  }
}

function hasSignallingSocketShape(value: unknown): value is WebSocketLike {
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
