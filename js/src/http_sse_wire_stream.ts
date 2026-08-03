import { type ByteMap, type ByteMapInput, randomId } from './bytes.js';
import { Deferred } from './concurrency.js';
import { WireMessage, makeHalfCloseMessage } from './data.js';
import { InProcessWireStream } from './in_process_wire_stream.js';
import { packStatus } from './status_codec.js';
import {
  dataLossError,
  failedPreconditionError,
  internalError,
  invalidArgumentError,
  isOk,
  isStatus,
  okStatus,
  outOfRangeError,
  statusFromResponse,
  statusFromUnknown,
  unavailableError,
  unimplementedError,
  type NonOkStatus,
  type Status,
  type StatusOr,
} from './status.js';
import {
  ABORT_STATUS_HEADER,
  normalizeWireStreamOptions,
  type OnWireDone,
  type OnWireMessage,
  type WireDeadline,
  type WireStream,
  type WireStreamOptions,
} from './wire_stream.js';

export const SSE_STREAM_ID_HEADER = 'x-a11-stream-id';
export const SSE_HTTP_HEADER_PREFIX = 'x-a11-http-';
export const DEFAULT_SSE_CONNECT_ENDPOINT = '/connect';
export const DEFAULT_SSE_MESSAGE_ENDPOINT = '/streams/{id}/message';

export type HttpHeaders = Readonly<Record<string, string>>;
export type FetchFunction = (
  input: string | URL | Request,
  init?: RequestInit,
) => Promise<Response>;

export interface HttpSseOptions {
  streamOptions?: WireStreamOptions;
  connectEndpoint?: string;
  messageEndpoint?: string;
  maxEventSize?: number;
  fetch?: FetchFunction;
  requestInit?: Omit<RequestInit, 'method' | 'headers' | 'body' | 'signal'>;
}

interface NormalizedHttpSseOptions {
  streamOptions: WireStreamOptions;
  connectUrl: string;
  messageUrlTemplate: string;
  maxEventSize: number;
  fetch: FetchFunction;
  requestInit: Omit<RequestInit, 'method' | 'headers' | 'body' | 'signal'>;
}

function normalizeHttpHeaders(
  headers: HttpHeaders,
): StatusOr<Readonly<Record<string, string>>> {
  const result: Record<string, string> = {};
  try {
    for (const [rawName, value] of Object.entries(headers)) {
      const name = rawName.toLowerCase();
      if (!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(name)) {
        return invalidArgumentError(`Invalid HTTP header name: ${rawName}.`);
      }
      if (typeof value !== 'string' || /[\r\n\0]/.test(value)) {
        return invalidArgumentError(`Invalid HTTP header value for ${rawName}.`);
      }
      result[name] = value;
    }
    return Object.freeze(result);
  } catch (error) {
    return statusFromUnknown(error, 'Could not normalize HTTP headers.');
  }
}

function resolveEndpoint(base: URL, endpoint: string): StatusOr<string> {
  if (typeof endpoint !== 'string' || endpoint.length === 0 || !endpoint.startsWith('/')) {
    return invalidArgumentError('HTTP SSE endpoints must be absolute paths.');
  }
  try {
    // Keep the literal {id} marker intact for the message URL template.
    return `${base.origin}${endpoint}`;
  } catch (error) {
    return invalidArgumentError('HTTP SSE endpoint is invalid.', [], error);
  }
}

function formatMessageEndpoint(template: string, streamId: string): StatusOr<string> {
  const first = template.indexOf('{id}');
  if (first < 0 || template.indexOf('{id}', first + 4) >= 0) {
    return invalidArgumentError(
      'messageEndpoint must contain exactly one {id} placeholder.',
    );
  }
  return `${template.slice(0, first)}${encodeURIComponent(streamId)}${template.slice(first + 4)}`;
}

function normalizeOptions(
  baseUrl: string,
  options: HttpSseOptions,
): StatusOr<NormalizedHttpSseOptions> {
  try {
    if (typeof options !== 'object' || options === null) {
      return invalidArgumentError('HTTP SSE options must be an object.');
    }
    const parsed = new URL(baseUrl);
    if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
      return invalidArgumentError('SSE service URL must use http:// or https://.');
    }
    const streamOptions = { ...(options.streamOptions ?? {}) };
    const normalizedStream = normalizeWireStreamOptions(streamOptions);
    if (!isOk(normalizedStream)) return normalizedStream;
    const connectUrl = resolveEndpoint(
      parsed,
      options.connectEndpoint ?? DEFAULT_SSE_CONNECT_ENDPOINT,
    );
    if (!isOk(connectUrl)) return connectUrl;
    const messageUrlTemplate = resolveEndpoint(
      parsed,
      options.messageEndpoint ?? DEFAULT_SSE_MESSAGE_ENDPOINT,
    );
    if (!isOk(messageUrlTemplate)) return messageUrlTemplate;
    const formatted = formatMessageEndpoint(messageUrlTemplate, 'validation');
    if (!isOk(formatted)) return formatted;
    const maxEventSize = options.maxEventSize ??
      normalizedStream.maxSingleMessageSize * 2 + 64 * 1024;
    if (!Number.isSafeInteger(maxEventSize) || maxEventSize <= 0) {
      return invalidArgumentError('maxEventSize must be a positive integer.');
    }
    const fetchFunction = options.fetch ?? globalThis.fetch?.bind(globalThis);
    if (typeof fetchFunction !== 'function') {
      return unimplementedError('Fetch is unavailable in this JavaScript runtime.');
    }
    return {
      streamOptions,
      connectUrl,
      messageUrlTemplate,
      maxEventSize,
      fetch: fetchFunction,
      requestInit: { ...(options.requestInit ?? {}) },
    };
  } catch (error) {
    return invalidArgumentError('Could not normalize HTTP SSE options.', [], error);
  }
}

function hasResponseShape(value: unknown): value is Response {
  if (typeof value !== 'object' || value === null) return false;
  try {
    const candidate = value as Record<string, unknown>;
    const headers = candidate.headers as Record<string, unknown> | undefined;
    return typeof candidate.ok === 'boolean' &&
      Number.isInteger(candidate.status) &&
      typeof headers === 'object' &&
      headers !== null &&
      typeof headers.get === 'function';
  } catch {
    return false;
  }
}

/** Fetch-based client for A11's HTTP/2 Server-Sent Events transport. */
export class HttpSseClientWireStream implements WireStream {
  private id = randomId('sse-');
  private started = false;
  private transportStatus: NonOkStatus | null = null;
  private requestHeaders: Readonly<Record<string, string>>;
  private responseHeaders: Readonly<Record<string, string>> | null = null;
  private readonly headersReady = new Deferred<Status>();
  private response: Response | null = null;
  private abortController: AbortController | null = null;
  private localTerminalTransmitted = false;
  private remoteTerminalReceived = false;
  private suppressOutboundTerminal = false;
  private transportFinished = false;

  private constructor(
    private readonly application: InProcessWireStream,
    private readonly bridge: InProcessWireStream,
    private readonly options: NormalizedHttpSseOptions,
    requestHeaders: Readonly<Record<string, string>>,
  ) {
    this.requestHeaders = requestHeaders;
  }

  static create(
    url: string,
    options: HttpSseOptions = {},
    requestHeaders: HttpHeaders = {},
  ): StatusOr<HttpSseClientWireStream> {
    try {
      const normalized = normalizeOptions(url, options);
      if (!isOk(normalized)) return normalized;
      const headers = normalizeHttpHeaders(requestHeaders);
      if (!isOk(headers)) return headers;
      const pair = InProcessWireStream.createPair(normalized.streamOptions);
      if (!isOk(pair)) return pair;
      return new HttpSseClientWireStream(pair[0], pair[1], normalized, headers);
    } catch (error) {
      return statusFromUnknown(error, 'Creating HTTP SSE stream raised an exception.');
    }
  }

  send(message: WireMessage): Status { return this.application.send(message); }

  start(onMessage?: OnWireMessage, onDone?: OnWireDone): Promise<Status> {
    return this.startEndpoint(false, onMessage, onDone);
  }

  accept(onMessage?: OnWireMessage, onDone?: OnWireDone): Promise<Status> {
    return this.startEndpoint(true, onMessage, onDone);
  }

  private async startEndpoint(
    accept: boolean,
    onMessage?: OnWireMessage,
    onDone?: OnWireDone,
  ): Promise<Status> {
    try {
      if (accept) return unimplementedError('An SSE client stream cannot accept.');
      if (this.started) return failedPreconditionError('WireStream is already started.');
      if (onMessage !== undefined && typeof onMessage !== 'function') {
        return invalidArgumentError('onMessage must be callable.');
      }
      if (onDone !== undefined && typeof onDone !== 'function') {
        return invalidArgumentError('onDone must be callable.');
      }
      this.started = true;
      const opened = await this.openTransport();
      if (!isStatus(opened)) {
        const invalid = internalError('SSE transport startup returned an invalid Status.');
        this.failTransport(invalid);
        return invalid;
      }
      if (!isOk(opened)) {
        this.failTransport(opened);
        return opened;
      }
      const bridgeStarted = await this.bridge.accept(
        (message) => this.handleBridgeMessage(message),
        () => this.handleBridgeDone(),
      );
      if (!isStatus(bridgeStarted)) {
        const invalid = internalError('SSE bridge startup returned an invalid Status.');
        this.failTransport(invalid);
        return invalid;
      }
      if (!isOk(bridgeStarted)) {
        this.failTransport(bridgeStarted);
        return bridgeStarted;
      }
      const applicationStarted = await this.application.start(onMessage, onDone);
      if (!isStatus(applicationStarted)) {
        const invalid = internalError('SSE application stream returned an invalid Status.');
        this.failTransport(invalid);
        return invalid;
      }
      if (!isOk(applicationStarted)) this.failTransport(applicationStarted);
      return applicationStarted;
    } catch (error) {
      const status = statusFromUnknown(error, 'Starting HTTP SSE stream raised an exception.');
      this.failTransport(status);
      return status;
    }
  }

  halfClose(trailers?: ByteMapInput): Status { return this.application.halfClose(trailers); }
  drainOutgoingMessages(): Promise<Status> { return this.application.drainOutgoingMessages(); }
  abort(status: Status): Status { return this.application.abort(status); }

  setDeadline(deadline?: WireDeadline): Status {
    const first = this.application.setDeadline(deadline);
    if (!isOk(first)) return first;
    return this.bridge.setDeadline(deadline);
  }

  getDeadline(): number | null { return this.application.getDeadline(); }
  getStatus(): Status { return this.transportStatus ?? this.application.getStatus(); }
  getTrailers(): ByteMap | null { return this.application.getTrailers(); }
  getId(): string { return this.id; }
  getImpl(): unknown | null { return this.response; }
  wait(): Promise<Status> { return this.application.wait(); }

  getHttpRequestHeaders(): Readonly<Record<string, string>> {
    return { ...this.requestHeaders };
  }

  getHttpResponseHeaders(): Readonly<Record<string, string>> | null {
    return this.responseHeaders === null ? null : { ...this.responseHeaders };
  }

  setHttpRequestHeaders(headers: HttpHeaders): Status {
    if (this.started) {
      return failedPreconditionError(
        'HTTP request headers cannot change after start().',
      );
    }
    const normalized = normalizeHttpHeaders(headers);
    if (!isOk(normalized)) return normalized;
    this.requestHeaders = normalized;
    return okStatus();
  }

  waitForHttpHeaders(): Promise<Status> {
    return this.headersReady.promise;
  }

  private async openTransport(): Promise<Status> {
    try {
      this.abortController = new AbortController();
      const headers = {
        ...this.requestHeaders,
        accept: 'text/event-stream',
      };
      let result: unknown;
      try {
        result = await this.options.fetch(this.options.connectUrl, {
          ...this.options.requestInit,
          method: 'POST',
          headers,
          signal: this.abortController.signal,
        });
      } catch (error) {
        return statusFromUnknown(error, 'SSE connect request failed.');
      }
      if (isStatus(result) && !isOk(result)) return result;
      if (!hasResponseShape(result)) {
        return dataLossError('SSE connect did not return a valid Response.');
      }
      if (!result.ok) return statusFromResponse(result, 'SSE connect');
      const streamId = result.headers.get(SSE_STREAM_ID_HEADER);
      if (streamId === null || streamId.length === 0) {
        return dataLossError('SSE response did not include x-a11-stream-id.');
      }
      const contentType = result.headers.get('content-type');
      if (contentType === null || !contentType.toLowerCase().startsWith('text/event-stream')) {
        return dataLossError('SSE response did not use text/event-stream.');
      }
      if (result.body === null) return dataLossError('SSE response has no body stream.');
      this.id = streamId;
      this.response = result;
      this.responseHeaders = Object.freeze(Object.fromEntries(result.headers.entries()));
      this.headersReady.resolve(okStatus());
      queueMicrotask(() => void this.receiveSseLoop(result));
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Opening SSE transport raised an exception.');
    }
  }

  private async handleBridgeMessage(message: WireMessage | null): Promise<Status> {
    const outgoing = message ?? makeHalfCloseMessage(this.bridge.getTrailers() ?? new Map());
    const terminal = outgoing.isHalfClose;
    const status = await this.transmit(outgoing);
    if (isOk(status) && terminal) this.localTerminalTransmitted = true;
    if (!isOk(status)) this.failTransport(status);
    return status;
  }

  private async handleBridgeDone(): Promise<Status> {
    if (this.transportFinished) return okStatus();
    const status = this.bridge.getStatus();
    if (
      !this.suppressOutboundTerminal &&
      !this.localTerminalTransmitted &&
      !this.remoteTerminalReceived &&
      !isOk(status)
    ) {
      const packed = packStatus(status);
      if (isOk(packed)) {
        const sent = await this.transmit(
          makeHalfCloseMessage(new Map([[ABORT_STATUS_HEADER, packed]])),
        );
        if (!isOk(sent) && this.transportStatus === null) this.transportStatus = sent;
      }
    }
    this.transportFinished = true;
    try { this.abortController?.abort(); } catch { /* bridge status is authoritative */ }
    return okStatus();
  }

  private async transmit(message: WireMessage): Promise<Status> {
    const payload = message.toJson();
    if (!isOk(payload)) return payload;
    const endpoint = formatMessageEndpoint(this.options.messageUrlTemplate, this.id);
    if (!isOk(endpoint)) return endpoint;
    let response: unknown;
    try {
      response = await this.options.fetch(endpoint, {
        ...this.options.requestInit,
        method: 'POST',
        headers: {
          ...this.requestHeaders,
          'content-type': 'application/json',
        },
        body: payload,
      });
    } catch (error) {
      return statusFromUnknown(error, 'SSE message request failed.');
    }
    if (isStatus(response) && !isOk(response)) return response;
    if (!hasResponseShape(response)) {
      return dataLossError('SSE message request did not return a valid Response.');
    }
    return response.ok
      ? okStatus()
      : statusFromResponse(response, 'SSE message request');
  }

  private async receiveSseLoop(response: Response): Promise<void> {
    let reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
    let decoder: TextDecoder;
    try {
      const body = response.body;
      if (body === null) {
        this.failTransport(dataLossError('SSE response body disappeared.'));
        return;
      }
      reader = body.getReader();
      decoder = new TextDecoder('utf-8', { fatal: true });
    } catch (error) {
      this.failTransport(statusFromUnknown(error, 'Opening SSE response body failed.'));
      return;
    }
    let lineBuffer = '';
    let dataLines: string[] = [];
    let sawTerminal = false;
    const processEvent = async (): Promise<Status> => {
      if (dataLines.length === 0) return okStatus();
      const payload = dataLines.join('\n');
      dataLines = [];
      if (new TextEncoder().encode(payload).byteLength > this.options.maxEventSize) {
        return outOfRangeError('SSE event exceeds maxEventSize.');
      }
      const message = WireMessage.fromJson(payload);
      if (!isOk(message)) return message;
      if (sawTerminal) {
        return failedPreconditionError(
          'SSE peer sent an event after a terminal WireMessage.',
        );
      }
      sawTerminal = message.isHalfClose;
      if (sawTerminal) this.remoteTerminalReceived = true;
      const sent = this.bridge.send(message);
      if (!isOk(sent)) return sent;
      if (message.isHalfClose && !message.headers.has(ABORT_STATUS_HEADER)) {
        return this.bridge.drainOutgoingMessages();
      }
      return okStatus();
    };

    try {
      while (true) {
        const rawRead: unknown = await reader.read();
        if (typeof rawRead !== 'object' || rawRead === null) {
          this.failTransport(
            dataLossError('SSE response reader returned an invalid result.'),
          );
          return;
        }
        const read = rawRead as { done?: unknown; value?: unknown };
        if (typeof read.done !== 'boolean') {
          this.failTransport(
            dataLossError('SSE response reader returned an invalid done flag.'),
          );
          return;
        }
        if (read.done) break;
        if (!(read.value instanceof Uint8Array)) {
          this.failTransport(
            dataLossError('SSE response reader returned a non-byte chunk.'),
          );
          return;
        }
        lineBuffer += decoder.decode(read.value, { stream: true });
        if (new TextEncoder().encode(lineBuffer).byteLength > this.options.maxEventSize) {
          this.failTransport(outOfRangeError('SSE event exceeds maxEventSize.'));
          return;
        }
        while (true) {
          const newline = lineBuffer.indexOf('\n');
          if (newline < 0) break;
          let line = lineBuffer.slice(0, newline);
          lineBuffer = lineBuffer.slice(newline + 1);
          if (line.endsWith('\r')) line = line.slice(0, -1);
          if (line === '') {
            const status = await processEvent();
            if (!isOk(status)) { this.failTransport(status); return; }
            continue;
          }
          if (line.startsWith(':')) continue;
          const colon = line.indexOf(':');
          const field = colon < 0 ? line : line.slice(0, colon);
          let value = colon < 0 ? '' : line.slice(colon + 1);
          if (value.startsWith(' ')) value = value.slice(1);
          if (field === 'data') dataLines.push(value);
        }
      }
      lineBuffer += decoder.decode();
      if (lineBuffer.endsWith('\r')) lineBuffer = lineBuffer.slice(0, -1);
      if (lineBuffer.startsWith('data:')) {
        let value = lineBuffer.slice(5);
        if (value.startsWith(' ')) value = value.slice(1);
        dataLines.push(value);
      }
      if (dataLines.length > 0) {
        const status = await processEvent();
        if (!isOk(status)) { this.failTransport(status); return; }
      }
      if (!sawTerminal && !this.transportFinished) {
        this.failTransport(
          unavailableError('SSE response ended before a terminal WireMessage.'),
        );
      }
    } catch (error) {
      if (this.transportFinished || this.abortController?.signal.aborted) return;
      this.failTransport(statusFromUnknown(error, 'Reading SSE response failed.'));
    } finally {
      try { reader?.releaseLock(); } catch { /* no externally visible effect */ }
    }
  }

  private failTransport(status: NonOkStatus): void {
    if (this.transportStatus !== null) return;
    this.transportStatus = status;
    this.suppressOutboundTerminal = true;
    this.headersReady.resolve(status);
    this.bridge.abort(status);
  }
}
