import { copyByteMap, normalizeByteMap, type ByteMap, type ByteMapInput } from './bytes.js';
import { Deferred } from './concurrency.js';
import { WireMessage, validateName } from './data.js';
import {
  deadlineExceededError,
  failedPreconditionError,
  internalError,
  invalidArgumentError,
  isOk,
  isStatus,
  okStatus,
  statusFromUnknown,
  type NonOkStatus,
  type Status,
  type StatusOr,
} from './status.js';

/** Trailer/header carrying a peer's structured non-OK terminal status. */
export const ABORT_STATUS_HEADER = 'x-a11-abort-status';
/** Hard ceiling for one reassembled A11 wire message. */
export const MAX_SINGLE_WIRE_MESSAGE_SIZE = 32 * 1024 * 1024;

/** Absolute JavaScript epoch deadline; `null` disables the deadline. */
export type WireDeadline = Date | number | null;

/** Limits applied by each endpoint of a {@link WireStream}. */
export interface WireStreamOptions {
  /** Maximum messages waiting for the application callback. */
  maxBufferedIncomingMessages?: number;
  /** Maximum encoded size of one reassembled {@link WireMessage}. */
  maxSingleMessageSize?: number;
  /** Maximum aggregate encoded bytes waiting for the application. */
  maxBufferedIncomingBytes?: number;
  /** Delivery timeout for one buffered message, or `null` for none. */
  messageTimeoutMs?: number | null;
  /** Absolute endpoint deadline, or `null` for none. */
  deadline?: WireDeadline;
  /**
   * Omit a fragment's mimetype when the fragment before it on the same node
   * carried the same one, and mark the frame so the peer puts it back.
   *
   * Off by default, and a sending decision only: a receiver always expands a
   * frame that says it was elided. Enable it when the peer is another build of
   * this library, or any A11 endpoint that understands
   * `x-a11-sticky-metadata` -- the C++ and Python runtimes do not, and would
   * deliver chunks whose mimetype had been dropped.
   */
  stickyMetadata?: boolean;
}

/** Validated, default-filled form of {@link WireStreamOptions}. */
export interface NormalizedWireStreamOptions {
  maxBufferedIncomingMessages: number;
  maxSingleMessageSize: number;
  maxBufferedIncomingBytes: number;
  messageTimeoutMs: number | null;
  stickyMetadata: boolean;
  deadline: number | null;
}

/**
 * Consumes one inbound message; `null` announces the peer's half-close.
 * The stream awaits the result before delivering more, providing backpressure.
 */
export type OnWireMessage = (
  message: WireMessage | null,
) => void | Status | Promise<void | Status>;

/** Runs once after both directions finish, or after an abort. */
export type OnWireDone = () => void | Status | Promise<void | Status>;

/**
 * Message-oriented transport shared by sessions, actions, and node mirroring.
 *
 * A wire stream is a bidirectional transport without global message ordering
 * guarantees. Sequenced {@link NodeFragment} streams provide application-level
 * ordering above this layer. The lifecycle does promise a closure
 * barrier: messages accepted before a half-close are observed before the peer
 * is told that direction has ended.
 *
 * Call {@link start} on the initiating endpoint or {@link accept} on the
 * responding endpoint exactly once. Finish normally with {@link halfClose} and
 * {@link drainOutgoingMessages}; use {@link abort} for a failed exchange.
 */
export interface WireStream {
  /** Queue a message for asynchronous transport; this does not await delivery. */
  send(message: WireMessage): Status;
  /** Drive the initiating side and install inbound lifecycle callbacks. */
  start(onMessage?: OnWireMessage, onDone?: OnWireDone): Promise<Status>;
  /** Drive the responding side and install inbound lifecycle callbacks. */
  accept(onMessage?: OnWireMessage, onDone?: OnWireDone): Promise<Status>;
  /** Stop sending after queued messages, while continuing to receive. */
  halfClose(trailers?: ByteMapInput): Status;
  /** Await queued outbound delivery after requesting a half-close. */
  drainOutgoingMessages(): Promise<Status>;
  /** End both directions with a structured non-OK status. */
  abort(status: Status): Status;
  /** Change the absolute deadline; omit it to disable the deadline. */
  setDeadline(deadline?: WireDeadline): Status;
  /** Return the normalized epoch deadline, or `null` when disabled. */
  getDeadline(): number | null;
  /** Return the current terminal error, or OK while healthy/cleanly done. */
  getStatus(): Status;
  /** Return peer half-close trailers once received. */
  getTrailers(): ByteMap | null;
  /** Return the transport-assigned stream identifier. */
  getId(): string;
  /** Return an advanced transport handle, when the implementation has one. */
  getImpl(): unknown | null;
  /** Await full stream completion rather than only the startup handshake. */
  wait(): Promise<Status>;
}

/** Convert a Date/epoch value into the normalized millisecond deadline. */
export function wireDeadlineMillis(
  value: WireDeadline | undefined,
): StatusOr<number | null> {
  try {
    if (value === undefined || value === null) return null;
    const result = value instanceof Date ? value.getTime() : value;
    if (!Number.isFinite(result)) {
      return invalidArgumentError('deadline must be a finite Date, number, or null.');
    }
    return result;
  } catch (error) {
    return invalidArgumentError(
      'deadline must be a readable Date, number, or null.',
      [],
      error,
    );
  }
}

/** Validate endpoint limits and apply defaults before opening a transport. */
export function normalizeWireStreamOptions(
  options: WireStreamOptions = {},
): StatusOr<NormalizedWireStreamOptions> {
  try {
    return normalizeWireStreamOptionsUnchecked(options);
  } catch (error) {
    return invalidArgumentError(
      'WireStream options could not be read.',
      [],
      error,
    );
  }
}

function normalizeWireStreamOptionsUnchecked(
  options: WireStreamOptions,
): StatusOr<NormalizedWireStreamOptions> {
  const empty = new WireMessage().toMsgpack();
  if (!isOk(empty)) return empty;
  const deadline = wireDeadlineMillis(options.deadline);
  if (!isOk(deadline)) return deadline;
  const result: NormalizedWireStreamOptions = {
    maxBufferedIncomingMessages: options.maxBufferedIncomingMessages ?? 100,
    maxSingleMessageSize:
      options.maxSingleMessageSize ?? MAX_SINGLE_WIRE_MESSAGE_SIZE,
    maxBufferedIncomingBytes:
      options.maxBufferedIncomingBytes ?? 32 * 1024 * 1024,
    messageTimeoutMs: options.messageTimeoutMs ?? null,
    stickyMetadata: options.stickyMetadata ?? false,
    deadline,
  };
  if (typeof result.stickyMetadata !== 'boolean') {
    return invalidArgumentError('stickyMetadata must be boolean.');
  }
  if (
    !Number.isSafeInteger(result.maxBufferedIncomingMessages) ||
    result.maxBufferedIncomingMessages < 1 ||
    result.maxBufferedIncomingMessages > 1024
  ) {
    return invalidArgumentError(
      'maxBufferedIncomingMessages must be an integer in [1, 1024].',
    );
  }
  if (
    !Number.isSafeInteger(result.maxSingleMessageSize) ||
    result.maxSingleMessageSize < empty.byteLength ||
    result.maxSingleMessageSize > MAX_SINGLE_WIRE_MESSAGE_SIZE
  ) {
    return invalidArgumentError(
      'maxSingleMessageSize is outside the supported range.',
    );
  }
  if (
    !Number.isSafeInteger(result.maxBufferedIncomingBytes) ||
    result.maxBufferedIncomingBytes < empty.byteLength
  ) {
    return invalidArgumentError(
      'maxBufferedIncomingBytes is smaller than an empty message.',
    );
  }
  if (
    result.messageTimeoutMs !== null &&
    (!Number.isFinite(result.messageTimeoutMs) || result.messageTimeoutMs < 0)
  ) {
    return invalidArgumentError(
      'messageTimeoutMs must be a non-negative finite number or null.',
    );
  }
  return result;
}

/** Validate A11 header names, fold them to lowercase, and copy their bytes. */
export function normalizeWireHeaders(
  headers: ByteMapInput | undefined = undefined,
): StatusOr<ByteMap> {
  try {
    const values = normalizeByteMap(headers);
    if (!isOk(values)) return values;
    const result: ByteMap = new Map();
    for (const [key, value] of values) {
      if (typeof key !== 'string') {
        return invalidArgumentError('Wire header names must be strings.');
      }
      const folded = key.toLowerCase();
      const valid = validateName(folded);
      if (!isOk(valid)) return valid;
      result.set(folded, value);
    }
    return result;
  } catch (error) {
    return statusFromUnknown(error, 'Normalizing WireStream headers raised.');
  }
}

function returnedStatus(value: unknown, operation: string): Status {
  return isStatus(value)
    ? value
    : internalError(`${operation} returned a non-Status value.`);
}

/** Invoke a user callback and convert throws or invalid returns into Status. */
export async function invokeWireCallback(
  callback: OnWireMessage | OnWireDone | undefined,
  message?: WireMessage | null,
): Promise<Status> {
  if (callback === undefined) return okStatus();
  if (typeof callback !== 'function') {
    return invalidArgumentError('WireStream callback must be callable.');
  }
  try {
    const result = message === undefined
      ? await (callback as OnWireDone)()
      : await (callback as OnWireMessage)(message);
    return result === undefined
      ? okStatus()
      : returnedStatus(result, 'WireStream callback');
  } catch (error) {
    return statusFromUnknown(error, 'WireStream callback raised an exception.');
  }
}

function hasWireStreamShape(value: unknown): value is WireStream {
  if (typeof value !== 'object' || value === null) return false;
  try {
    const candidate = value as Record<string, unknown>;
    return [
      'send',
      'start',
      'accept',
      'halfClose',
      'drainOutgoingMessages',
      'abort',
      'setDeadline',
      'getDeadline',
      'getStatus',
      'getTrailers',
      'getId',
      'getImpl',
      'wait',
    ].every((name) => typeof candidate[name] === 'function');
  } catch {
    return false;
  }
}

interface ReceiveWaiter {
  deferred: Deferred<StatusOr<WireMessage | null>>;
  timer: ReturnType<typeof setTimeout> | null;
}

/**
 * Pull-oriented, one-slot adapter for a callback-driven {@link WireStream}.
 *
 * Use this in an agent loop that wants to `await receive()` rather than own a
 * callback. The one-slot handoff retains transport backpressure. A remote
 * abort takes priority over buffered data and remains visible to all future
 * receivers.
 */
export class WireStreamWithRecv implements WireStream {
  private readonly id: string;
  private slot: WireMessage | null | undefined;
  private readonly receivers: ReceiveWaiter[] = [];
  private readonly spaceWaiters: Array<Deferred<Status>> = [];
  private error: NonOkStatus | null = null;
  private done = false;
  private remoteHalfClosed = false;
  private eofDelivered = false;

  private constructor(private readonly wrapped: WireStream, id: string) {
    this.id = id;
  }

  /** Wrap a stream while preserving its id, status, trailers, and transport. */
  static create(stream: unknown): StatusOr<WireStreamWithRecv> {
    if (!hasWireStreamShape(stream)) {
      return invalidArgumentError('stream must implement WireStream.');
    }
    try {
      const id = stream.getId();
      const validId = validateName(id);
      if (!isOk(validId)) return validId;
      return new WireStreamWithRecv(stream, id);
    } catch (error) {
      return statusFromUnknown(error, 'WireStream.getId() raised an exception.');
    }
  }

  get wrappedStream(): WireStream {
    return this.wrapped;
  }

  send(message: WireMessage): Status {
    try {
      return returnedStatus(this.wrapped.send(message), 'WireStream.send()');
    } catch (error) {
      return statusFromUnknown(error, 'WireStream.send() raised an exception.');
    }
  }

  start(onMessage?: OnWireMessage, onDone?: OnWireDone): Promise<Status> {
    return this.startImpl(false, onMessage, onDone);
  }

  accept(onMessage?: OnWireMessage, onDone?: OnWireDone): Promise<Status> {
    return this.startImpl(true, onMessage, onDone);
  }

  private async startImpl(
    accept: boolean,
    observer?: OnWireMessage,
    doneObserver?: OnWireDone,
  ): Promise<Status> {
    try {
      const start = accept ? this.wrapped.accept.bind(this.wrapped) : this.wrapped.start.bind(this.wrapped);
      const status = await start(
        async (message) => this.handleMessage(message, observer),
        async () => this.handleDone(doneObserver),
      );
      const returned = returnedStatus(status, 'WireStream startup');
      if (!isOk(returned)) this.signalError(returned);
      return returned;
    } catch (error) {
      const status = statusFromUnknown(
        error,
        'WireStream startup raised an exception.',
      );
      this.signalError(status);
      return status;
    }
  }

  halfClose(trailers?: ByteMapInput): Status {
    try {
      return returnedStatus(
        this.wrapped.halfClose(trailers),
        'WireStream.halfClose()',
      );
    } catch (error) {
      return statusFromUnknown(error, 'WireStream.halfClose() raised an exception.');
    }
  }

  async drainOutgoingMessages(): Promise<Status> {
    try {
      return returnedStatus(
        await this.wrapped.drainOutgoingMessages(),
        'WireStream.drainOutgoingMessages()',
      );
    } catch (error) {
      return statusFromUnknown(
        error,
        'WireStream.drainOutgoingMessages() raised an exception.',
      );
    }
  }

  abort(status: Status): Status {
    if (!isStatus(status) || isOk(status)) {
      return invalidArgumentError('WireStream abort status must be non-OK.');
    }
    try {
      const result = returnedStatus(
        this.wrapped.abort(status),
        'WireStream.abort()',
      );
      this.recordCurrentStatus();
      return result;
    } catch (error) {
      const result = statusFromUnknown(error, 'WireStream.abort() raised an exception.');
      this.signalError(result);
      return result;
    }
  }

  setDeadline(deadline?: WireDeadline): Status {
    try {
      const result = returnedStatus(
        this.wrapped.setDeadline(deadline),
        'WireStream.setDeadline()',
      );
      this.recordCurrentStatus();
      return result;
    } catch (error) {
      const result = statusFromUnknown(
        error,
        'WireStream.setDeadline() raised an exception.',
      );
      this.signalError(result);
      return result;
    }
  }

  getDeadline(): number | null {
    try {
      const deadline = this.wrapped.getDeadline();
      return deadline === null || Number.isFinite(deadline) ? deadline : null;
    } catch {
      return null;
    }
  }

  getStatus(): Status {
    try {
      return returnedStatus(this.wrapped.getStatus(), 'WireStream.getStatus()');
    } catch (error) {
      return statusFromUnknown(error, 'WireStream.getStatus() raised an exception.');
    }
  }

  getTrailers(): ByteMap | null {
    try {
      const trailers = this.wrapped.getTrailers();
      return trailers === null ? null : copyByteMap(trailers);
    } catch {
      return null;
    }
  }

  getId(): string {
    try {
      const current = this.wrapped.getId();
      return typeof current === 'string' && current.length > 0 ? current : this.id;
    } catch {
      return this.id;
    }
  }

  getImpl(): unknown | null {
    try {
      return this.wrapped.getImpl();
    } catch {
      return null;
    }
  }

  async wait(): Promise<Status> {
    try {
      return returnedStatus(
        await this.wrapped.wait(),
        'WireStream.wait()',
      );
    } catch (error) {
      return statusFromUnknown(error, 'WireStream.wait() raised an exception.');
    }
  }

  /** Await one message, or `null` once for the peer's clean half-close. */
  receive(timeoutMs?: number): Promise<StatusOr<WireMessage | null>> {
    this.recordCurrentStatus();
    if (
      timeoutMs !== undefined &&
      (!Number.isFinite(timeoutMs) || timeoutMs < 0)
    ) {
      return Promise.resolve(
        invalidArgumentError('timeoutMs must be a non-negative finite number.'),
      );
    }
    if (this.error !== null) return Promise.resolve(this.error);
    if (this.eofDelivered) {
      return Promise.resolve(
        failedPreconditionError(
          'The remote WireStream half-close was already received.',
        ),
      );
    }
    if (this.slot !== undefined) {
      const result = this.slot;
      this.slot = undefined;
      if (result === null) this.deliverEofToRemainingReceivers();
      this.notifySpace();
      return Promise.resolve(result);
    }
    if (this.done) {
      return Promise.resolve(
        internalError('WireStream finished without a remote half-close.'),
      );
    }
    const waiter: ReceiveWaiter = {
      deferred: new Deferred<StatusOr<WireMessage | null>>(),
      timer: null,
    };
    if (timeoutMs !== undefined) {
      waiter.timer = setTimeout(() => {
        const index = this.receivers.indexOf(waiter);
        if (index < 0) return;
        this.receivers.splice(index, 1);
        waiter.deferred.resolve(
          deadlineExceededError(
            'WireStream receive timed out before a message was available.',
          ),
        );
      }, timeoutMs);
    }
    this.receivers.push(waiter);
    return waiter.deferred.promise;
  }

  private async handleMessage(
    message: WireMessage | null,
    observer?: OnWireMessage,
  ): Promise<Status> {
    if (this.error !== null) return okStatus();
    if (message !== null && !(message instanceof WireMessage)) {
      return internalError('WireStream delivered a non-WireMessage value.');
    }
    if (this.remoteHalfClosed) {
      return internalError('WireStream delivered data after remote half-close.');
    }
    while (
      this.slot !== undefined &&
      this.receivers.length === 0 &&
      this.error === null
    ) {
      const changed = new Deferred<Status>();
      this.spaceWaiters.push(changed);
      const status = await changed.promise;
      if (!isOk(status)) return status;
    }
    if (this.error !== null) return okStatus();
    if (message === null) this.remoteHalfClosed = true;
    const receiver = this.receivers.shift();
    if (receiver !== undefined) {
      if (receiver.timer !== null) clearTimeout(receiver.timer);
      receiver.deferred.resolve(message);
      if (message === null) this.deliverEofToRemainingReceivers();
    } else {
      this.slot = message;
    }
    return invokeWireCallback(observer, message);
  }

  private async handleDone(observer?: OnWireDone): Promise<Status> {
    this.done = true;
    this.recordCurrentStatus();
    if (this.error === null && !this.remoteHalfClosed && this.slot === undefined) {
      this.signalError(
        internalError('WireStream finished without a remote half-close.'),
      );
    }
    const status = await invokeWireCallback(observer);
    return status;
  }

  private recordCurrentStatus(): void {
    const status = this.getStatus();
    if (!isOk(status)) this.signalError(status);
  }

  private signalError(status: NonOkStatus): void {
    if (this.error !== null) return;
    this.error = status;
    this.slot = undefined;
    for (const receiver of this.receivers.splice(0)) {
      if (receiver.timer !== null) clearTimeout(receiver.timer);
      receiver.deferred.resolve(status);
    }
    for (const waiter of this.spaceWaiters.splice(0)) waiter.resolve(status);
  }

  private deliverEofToRemainingReceivers(): void {
    this.eofDelivered = true;
    const status = failedPreconditionError(
      'The remote WireStream half-close was already received.',
    );
    for (const receiver of this.receivers.splice(0)) {
      if (receiver.timer !== null) clearTimeout(receiver.timer);
      receiver.deferred.resolve(status);
    }
  }

  private notifySpace(): void {
    const waiter = this.spaceWaiters.shift();
    waiter?.resolve(okStatus());
  }
}
