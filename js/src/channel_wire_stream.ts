import {
  ByteReassembler,
  MINIMUM_BYTE_PACKET_SIZE,
  splitBytesIntoPackets,
} from './byte_chunking.js';
import { copyByteMap, type ByteMap, type ByteMapInput } from './bytes.js';
import { Deferred, sleep } from './concurrency.js';
import { WireMessage, makeHalfCloseMessage, validateName } from './data.js';
import { decodeStatus, packStatus } from './status_codec.js';
import {
  abortedError,
  deadlineExceededError,
  failedPreconditionError,
  internalError,
  invalidArgumentError,
  isOk,
  isStatus,
  okStatus,
  outOfRangeError,
  resourceExhaustedError,
  statusFromUnknown,
  unavailableError,
  unimplementedError,
  type NonOkStatus,
  type Status,
  type StatusOr,
} from './status.js';
import {
  ABORT_STATUS_HEADER,
  invokeWireCallback,
  normalizeWireHeaders,
  normalizeWireStreamOptions,
  wireDeadlineMillis,
  type NormalizedWireStreamOptions,
  type OnWireDone,
  type OnWireMessage,
  type WireDeadline,
  type WireStream,
  type WireStreamOptions,
} from './wire_stream.js';

/** Which side of a binary channel a {@link ChannelWireStream} may drive. */
export enum ChannelEndpointRole {
  /** Initiate the transport with {@link WireStream.start}. */
  CLIENT = 'client',
  /** Receive the transport with {@link WireStream.accept}. */
  SERVER = 'server',
  /** Allow either startup method; useful for custom channel adapters. */
  EITHER = 'either',
}

/** Packetization and reassembly bounds for a binary channel transport. */
export interface ChannelFramingOptions {
  /** Maximum bytes in one transport packet, including framing metadata. */
  splitSize?: number;
  /** Incomplete interleaved messages retained during reassembly. */
  maxPendingMessages?: number;
  /** Aggregate bytes retained for incomplete messages. */
  maxPendingBytes?: number;
}

/** Validated, default-filled form of {@link ChannelFramingOptions}. */
export interface NormalizedChannelFramingOptions {
  /** Maximum bytes in one framed packet. */
  splitSize: number;
  /** Maximum number of incomplete messages. */
  maxPendingMessages: number;
  /** Maximum aggregate bytes held by the reassembler. */
  maxPendingBytes: number;
}

/** Events a {@link BinaryChannel} reports to its framing state machine. */
export interface BinaryChannelCallbacks {
  /** Report that packets may now be sent. */
  onOpen: () => void;
  /** Deliver one complete framing packet, not an application message. */
  onMessage: (packet: Uint8Array) => void;
  /** End the stream because the underlying transport failed. */
  onError: (status: NonOkStatus) => void;
  /** Report that the underlying transport closed. */
  onClosed: () => void;
  /** Wake a sender waiting for transport backpressure to ease. */
  onBufferedAmountLow: () => void;
}

/**
 * Transport-adapter seam shared by WebSocket, WebRTC, and in-process streams.
 *
 * Implement this interface when bringing another message-capable binary
 * transport to A11. {@link ChannelWireStream} supplies byte packetization,
 * bounded out-of-order reassembly, A11 half-close/abort messages, deadlines,
 * and callback ordering. The adapter only owns opening, packet I/O,
 * backpressure observation, and physical closure.
 */
export interface BinaryChannel {
  /** Install the callbacks used for one stream lifecycle. */
  setCallbacks(callbacks: BinaryChannelCallbacks): Status;
  /** Release callback references after full stream completion. */
  resetCallbacks(): Status;
  /** Open the underlying channel and resolve when packet I/O is available. */
  open(): Promise<Status>;
  /** Return whether packet sends are currently possible. */
  isOpen(): boolean;
  /** Queue one framing packet without waiting for physical delivery. */
  send(packet: Uint8Array): Status;
  /** Return bytes still buffered by the underlying transport. */
  bufferedAmount(): StatusOr<number>;
  /** Await a low-water notification; callers recheck {@link bufferedAmount}. */
  waitForBufferedAmountLow(): Promise<Status>;
  /** Close the physical channel and release transport resources. */
  close(): Status;
  /** Expose the transport-specific object for advanced integration. */
  getImpl(): unknown | null;
}

/** Validate framing limits and choose defaults for a transport endpoint. */
export function normalizeChannelFramingOptions(
  options: ChannelFramingOptions = {},
  maxMessageSize = 32 * 1024 * 1024,
): StatusOr<NormalizedChannelFramingOptions> {
  try {
    return normalizeChannelFramingOptionsUnchecked(options, maxMessageSize);
  } catch (error) {
    return invalidArgumentError(
      'Channel framing options could not be read.',
      [],
      error,
    );
  }
}

function normalizeChannelFramingOptionsUnchecked(
  options: ChannelFramingOptions,
  maxMessageSize: number,
): StatusOr<NormalizedChannelFramingOptions> {
  const result: NormalizedChannelFramingOptions = {
    splitSize: options.splitSize ?? 64 * 1024,
    maxPendingMessages: options.maxPendingMessages ?? 64,
    maxPendingBytes: options.maxPendingBytes ?? 64 * 1024 * 1024,
  };
  if (
    !Number.isSafeInteger(result.splitSize) ||
    result.splitSize < MINIMUM_BYTE_PACKET_SIZE ||
    result.splitSize > 1024 * 1024
  ) {
    return invalidArgumentError(
      `splitSize must be an integer in [${MINIMUM_BYTE_PACKET_SIZE}, 1048576].`,
    );
  }
  if (
    !Number.isSafeInteger(result.maxPendingMessages) ||
    result.maxPendingMessages <= 0 ||
    !Number.isSafeInteger(result.maxPendingBytes) ||
    result.maxPendingBytes <= 0
  ) {
    return invalidArgumentError(
      'Channel pending reassembly limits must be positive integers.',
    );
  }
  if (result.splitSize > maxMessageSize + 9) {
    return invalidArgumentError(
      'splitSize must not exceed maxSingleMessageSize plus metadata.',
    );
  }
  return result;
}

type End = 'none' | 'half-close' | 'abort';

interface Outbound {
  bytes: Uint8Array;
  end: End;
  messageId: bigint;
}

interface Incoming {
  bytes: Uint8Array;
  receivedAt: number;
}

function hasBinaryChannelShape(value: unknown): value is BinaryChannel {
  if (typeof value !== 'object' || value === null) return false;
  try {
    const candidate = value as Record<string, unknown>;
    return [
      'setCallbacks',
      'resetCallbacks',
      'open',
      'isOpen',
      'send',
      'bufferedAmount',
      'waitForBufferedAmountLow',
      'close',
      'getImpl',
    ].every((name) => typeof candidate[name] === 'function');
  } catch {
    return false;
  }
}

/**
 * Adds the A11 WireStream lifecycle to a packet-oriented binary channel.
 *
 * Each {@link WireMessage} is split into bounded packets and may be reassembled
 * out of order or interleaved with other messages. Incoming queues and partial
 * messages are bounded separately so an agent cannot accumulate unbounded
 * state while an application callback is slow.
 *
 * Most applications construct {@link WebSocketWireStream},
 * {@link WebRtcWireStream}, or {@link InProcessWireStream} instead. Use this
 * class directly when adapting a new channel. After exactly one of
 * {@link start} or {@link accept}, finish normally with {@link halfClose} then
 * {@link drainOutgoingMessages}; peer input continues until its half-close.
 * An abort, transport failure, or deadline skips that normal drain and ends
 * both directions with a structured status.
 */
export class ChannelWireStream implements WireStream {
  /** Normalized application-message and timing limits. */
  readonly options: Readonly<NormalizedWireStreamOptions>;
  /** Normalized packet reassembly limits. */
  readonly framing: Readonly<NormalizedChannelFramingOptions>;

  private readonly reassembler: ByteReassembler;
  private started = false;
  private opened = false;
  private finished = false;
  private doneCalled = false;
  private onMessage: OnWireMessage | undefined;
  private onDone: OnWireDone | undefined;
  private status: Status = okStatus();
  private trailers: ByteMap | null = null;
  private localEnd: End = 'none';
  private localEndSent: End = 'none';
  private remoteHalfClosed = false;
  private remoteAborted = false;
  private nextMessageId = 0n;
  private readonly outgoing: Outbound[] = [];
  private outgoingPumpRunning = false;
  private readonly incoming: Incoming[] = [];
  private incomingBytes = 0;
  private incomingPumpRunning = false;
  private readonly drainDone = new Deferred<Status>();
  private readonly finishedDone = new Deferred<Status>();
  private deadlineTimer: ReturnType<typeof setTimeout> | null = null;
  private activityTimer: ReturnType<typeof setTimeout> | null = null;
  private lastActivity = Date.now();

  private constructor(
    private readonly channel: BinaryChannel,
    private readonly id: string,
    private readonly role: ChannelEndpointRole,
    options: NormalizedWireStreamOptions,
    framing: NormalizedChannelFramingOptions,
    reassembler: ByteReassembler,
  ) {
    this.options = { ...options };
    this.framing = Object.freeze({ ...framing });
    this.reassembler = reassembler;
    this.armTiming();
  }

  /**
   * Wrap a binary channel with A11 framing and lifecycle semantics.
   *
   * Construction validates the stream id and all limits without opening the
   * channel. Opening happens later in {@link start} or {@link accept}.
   */
  static create(
    channel: BinaryChannel,
    id: string,
    role: ChannelEndpointRole = ChannelEndpointRole.EITHER,
    options: WireStreamOptions = {},
    framingOptions: ChannelFramingOptions = {},
  ): StatusOr<ChannelWireStream> {
    try {
      return ChannelWireStream.createUnchecked(
        channel,
        id,
        role,
        options,
        framingOptions,
      );
    } catch (error) {
      return statusFromUnknown(
        error,
        'Creating ChannelWireStream raised an exception.',
      );
    }
  }

  private static createUnchecked(
    channel: BinaryChannel,
    id: string,
    role: ChannelEndpointRole,
    options: WireStreamOptions,
    framingOptions: ChannelFramingOptions,
  ): StatusOr<ChannelWireStream> {
    if (!hasBinaryChannelShape(channel)) {
      return invalidArgumentError('channel must implement BinaryChannel.');
    }
    const validId = validateName(id);
    if (!isOk(validId)) return validId;
    if (!Object.values(ChannelEndpointRole).includes(role)) {
      return invalidArgumentError('role is not a valid ChannelEndpointRole.');
    }
    const normalized = normalizeWireStreamOptions(options);
    if (!isOk(normalized)) return normalized;
    const framing = normalizeChannelFramingOptions(
      framingOptions,
      normalized.maxSingleMessageSize,
    );
    if (!isOk(framing)) return framing;
    const reassembler = ByteReassembler.create({
      packetSize: framing.splitSize,
      maxMessageSize: normalized.maxSingleMessageSize,
      maxPendingMessages: framing.maxPendingMessages,
      maxPendingBytes: framing.maxPendingBytes,
    });
    if (!isOk(reassembler)) return reassembler;
    return new ChannelWireStream(
      channel,
      id,
      role,
      normalized,
      framing,
      reassembler,
    );
  }

  send(message: WireMessage): Status {
    if (!(message instanceof WireMessage)) {
      return invalidArgumentError('message must be a WireMessage.');
    }
    const validation = message.validate();
    if (!isOk(validation)) return validation;
    let end: End = 'none';
    if (message.isHalfClose) {
      const normalized = normalizeWireHeaders(message.headers);
      if (!isOk(normalized)) return normalized;
      message = new WireMessage({ headers: normalized });
      end = normalized.has(ABORT_STATUS_HEADER) ? 'abort' : 'half-close';
    }
    const bytes = message.toMsgpack();
    if (!isOk(bytes)) return bytes;
    if (bytes.byteLength > this.options.maxSingleMessageSize) {
      return outOfRangeError(
        'Outgoing WireMessage exceeds maxSingleMessageSize.',
      );
    }
    if (this.remoteAborted) {
      return failedPreconditionError('The peer aborted the stream.');
    }
    if (this.localEnd !== 'none' || this.finished) {
      return failedPreconditionError('This endpoint has already terminated.');
    }
    if (this.deadlineExpired()) {
      this.forceAbort(
        deadlineExceededError('WireStream deadline exceeded.'),
        false,
      );
      return failedPreconditionError('WireStream deadline exceeded.');
    }
    this.localEnd = end;
    if (end === 'abort') {
      this.status = abortedError('The stream was aborted by this endpoint.');
    }
    this.outgoing.push({
      bytes,
      end,
      messageId: this.nextMessageId,
    });
    this.nextMessageId = (this.nextMessageId + 1n) & 0xffff_ffff_ffff_ffffn;
    this.markActivity();
    this.scheduleOutgoingPump();
    return okStatus();
  }

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
    if (onMessage !== undefined && typeof onMessage !== 'function') {
      return invalidArgumentError('onMessage must be callable.');
    }
    if (onDone !== undefined && typeof onDone !== 'function') {
      return invalidArgumentError('onDone must be callable.');
    }
    if (this.started) {
      return failedPreconditionError('WireStream is already started.');
    }
    if (
      (accept && this.role === ChannelEndpointRole.CLIENT) ||
      (!accept && this.role === ChannelEndpointRole.SERVER)
    ) {
      return unimplementedError(
        accept
          ? 'This WireStream cannot accept.'
          : 'This WireStream cannot start as a client.',
      );
    }
    this.started = true;
    this.onMessage = onMessage;
    this.onDone = onDone;
    this.lastActivity = Date.now();
    const configured = this.configureChannel();
    if (!isOk(configured)) {
      this.finish(configured);
      return configured;
    }
    if (this.deadlineExpired()) {
      const status = deadlineExceededError('WireStream deadline exceeded.');
      this.forceAbort(status, false);
      return status;
    }
    try {
      const opened = await this.channel.open();
      if (!isStatus(opened)) {
        const status = internalError(
          'BinaryChannel.open() returned a non-Status value.',
        );
        this.finish(status);
        return status;
      }
      if (!isOk(opened)) {
        this.finish(opened);
        return opened;
      }
      const reportedOpen = this.channel.isOpen();
      if (typeof reportedOpen !== 'boolean') {
        const status = internalError(
          'BinaryChannel.isOpen() returned a non-boolean value.',
        );
        this.finish(status);
        return status;
      }
      this.opened = reportedOpen;
      if (!this.opened) {
        const status = unavailableError(
          'Binary channel did not report open after startup.',
        );
        this.finish(status);
        return status;
      }
      this.markActivity();
      this.scheduleOutgoingPump();
      this.scheduleIncomingPump();
      return okStatus();
    } catch (error) {
      const status = statusFromUnknown(
        error,
        'Binary channel startup raised an exception.',
      );
      this.finish(status);
      return status;
    }
  }

  halfClose(trailers?: ByteMapInput): Status {
    if (this.localEnd !== 'none' || this.finished) return okStatus();
    const normalized = normalizeWireHeaders(trailers);
    if (!isOk(normalized)) return normalized;
    return this.send(makeHalfCloseMessage(normalized));
  }

  drainOutgoingMessages(): Promise<Status> {
    if (this.localEnd !== 'half-close') {
      return Promise.resolve(
        failedPreconditionError(
          'drainOutgoingMessages() requires halfClose() first.',
        ),
      );
    }
    if (this.localEndSent === 'half-close') return Promise.resolve(okStatus());
    return this.drainDone.promise;
  }

  abort(status: Status): Status {
    if (!isStatus(status) || isOk(status)) {
      return invalidArgumentError('Abort status must be non-OK.');
    }
    if (this.localEnd !== 'none' || this.finished) return okStatus();
    const packed = packStatus(status);
    if (!isOk(packed)) return packed;
    return this.send(
      makeHalfCloseMessage(new Map([[ABORT_STATUS_HEADER, packed]])),
    );
  }

  setDeadline(deadline?: WireDeadline): Status {
    const parsed = wireDeadlineMillis(deadline);
    if (!isOk(parsed)) return parsed;
    (this.options as { deadline: number | null }).deadline = parsed;
    this.armTiming();
    if (this.deadlineExpired() && !this.finished) {
      this.forceAbort(
        deadlineExceededError('WireStream deadline exceeded.'),
        true,
      );
    }
    return okStatus();
  }

  getDeadline(): number | null {
    return this.options.deadline;
  }

  getStatus(): Status {
    if (this.deadlineExpired() && !this.finished) {
      this.forceAbort(
        deadlineExceededError('WireStream deadline exceeded.'),
        true,
      );
    }
    return this.status;
  }

  getTrailers(): ByteMap | null {
    return this.trailers === null ? null : copyByteMap(this.trailers);
  }

  getId(): string {
    return this.id;
  }

  getImpl(): unknown | null {
    try {
      return this.channel.getImpl();
    } catch {
      return null;
    }
  }

  /** Await terminal stream completion, including the done callback. */
  wait(): Promise<Status> {
    return this.finishedDone.promise;
  }

  private configureChannel(): Status {
    try {
      const status = this.channel.setCallbacks({
        onOpen: () => {
          this.opened = true;
          this.markActivity();
          this.scheduleOutgoingPump();
        },
        onMessage: (packet) => this.handlePacket(packet),
        onError: (status) => this.finish(status),
        onClosed: () => this.handleChannelClosed(),
        onBufferedAmountLow: () => undefined,
      });
      return isStatus(status)
        ? status
        : internalError(
            'BinaryChannel.setCallbacks() returned a non-Status value.',
          );
    } catch (error) {
      return statusFromUnknown(
        error,
        'Configuring binary channel callbacks raised an exception.',
      );
    }
  }

  private handlePacket(packet: Uint8Array): void {
    if (this.finished) return;
    try {
      const complete = this.reassembler.feed(packet);
      if (!isOk(complete)) {
        this.forceAbort(complete, true);
        return;
      }
      if (complete === null) return;
      if (complete.byteLength > this.options.maxSingleMessageSize) {
        this.forceAbort(
          outOfRangeError(
            'Incoming WireMessage exceeds maxSingleMessageSize.',
          ),
          true,
        );
        return;
      }
      if (
        this.incoming.length >= this.options.maxBufferedIncomingMessages ||
        (this.incoming.length > 0 &&
          this.incomingBytes + complete.byteLength >
            this.options.maxBufferedIncomingBytes)
      ) {
        this.forceAbort(
          resourceExhaustedError(
            'Incoming WireMessage buffer capacity was exceeded.',
          ),
          true,
        );
        return;
      }
      this.incoming.push({ bytes: complete, receivedAt: Date.now() });
      this.incomingBytes += complete.byteLength;
      this.markActivity();
      this.scheduleIncomingPump();
    } catch (error) {
      this.forceAbort(
        statusFromUnknown(error, 'Receiving channel data raised an exception.'),
        true,
      );
    }
  }

  private scheduleOutgoingPump(): void {
    if (
      this.outgoingPumpRunning ||
      !this.started ||
      !this.opened ||
      this.finished
    ) {
      return;
    }
    this.outgoingPumpRunning = true;
    queueMicrotask(() => void this.pumpOutgoing());
  }

  private async pumpOutgoing(): Promise<void> {
    try {
      while (!this.finished && this.started && this.opened) {
        const outbound = this.outgoing.shift();
        if (outbound === undefined) break;
        const packets = splitBytesIntoPackets(
          outbound.bytes,
          outbound.messageId,
          this.framing.splitSize,
        );
        if (!isOk(packets)) {
          this.finish(packets);
          return;
        }
        for (const packet of packets) {
          let sent: Status;
          try {
            sent = this.channel.send(packet);
          } catch (error) {
            sent = statusFromUnknown(
              error,
              'Binary channel send raised an exception.',
            );
          }
          if (!isStatus(sent)) {
            sent = internalError(
              'BinaryChannel.send() returned a non-Status value.',
            );
          }
          if (!isOk(sent)) {
            this.finish(sent);
            return;
          }
        }
        this.markActivity();
        if (outbound.end !== 'none') {
          const drained = await this.waitForChannelDrain();
          if (!isOk(drained)) {
            this.finish(drained);
            return;
          }
          this.localEndSent = outbound.end;
          if (outbound.end === 'half-close') {
            this.drainDone.resolve(okStatus());
          }
          this.maybeFinish();
          return;
        }
      }
    } catch (error) {
      this.finish(
        statusFromUnknown(error, 'WireStream sender raised an exception.'),
      );
    } finally {
      this.outgoingPumpRunning = false;
      if (this.outgoing.length > 0) this.scheduleOutgoingPump();
    }
  }

  private async waitForChannelDrain(): Promise<Status> {
    while (!this.finished) {
      let amount: StatusOr<number>;
      try {
        amount = this.channel.bufferedAmount();
      } catch (error) {
        return statusFromUnknown(
          error,
          'Reading binary channel buffered amount raised an exception.',
        );
      }
      if (!isOk(amount)) return amount;
      if (!Number.isFinite(amount) || amount < 0) {
        return internalError(
          'BinaryChannel.bufferedAmount() returned an invalid amount.',
        );
      }
      if (amount === 0) return okStatus();
      let waited: Status;
      try {
        waited = await this.channel.waitForBufferedAmountLow();
      } catch (error) {
        waited = statusFromUnknown(
          error,
          'Waiting for binary channel drain raised an exception.',
        );
      }
      if (!isStatus(waited)) {
        waited = internalError(
          'BinaryChannel.waitForBufferedAmountLow() returned a non-Status value.',
        );
      }
      if (!isOk(waited)) return waited;
      // A low-water notification need not mean fully drained. Yield to the
      // host event loop so browser/Node transports can advance their socket
      // buffers before observing bufferedAmount again.
      const yielded = await sleep(0);
      if (!isOk(yielded)) return yielded;
    }
    return isOk(this.status)
      ? failedPreconditionError('WireStream finished before transport drain.')
      : this.status;
  }

  private scheduleIncomingPump(): void {
    if (
      this.incomingPumpRunning ||
      !this.started ||
      this.finished ||
      this.incoming.length === 0
    ) {
      return;
    }
    this.incomingPumpRunning = true;
    queueMicrotask(() => void this.pumpIncoming());
  }

  private async pumpIncoming(): Promise<void> {
    try {
      while (!this.finished) {
        const incoming = this.incoming.shift();
        if (incoming === undefined) break;
        this.incomingBytes -= incoming.bytes.byteLength;
        const message = WireMessage.fromMsgpack(incoming.bytes);
        if (!isOk(message)) {
          this.forceAbort(message, true);
          return;
        }
        this.markActivity();
        if (!message.isHalfClose) {
          if (this.remoteHalfClosed || this.remoteAborted) {
            this.forceAbort(
              failedPreconditionError(
                'Peer sent data after a terminal message.',
              ),
              true,
            );
            return;
          }
          const callbackStatus = await this.invokeMessageWithTimeout(message);
          if (!isOk(callbackStatus)) {
            this.forceAbort(callbackStatus, true);
            return;
          }
          continue;
        }

        const headers = normalizeWireHeaders(message.headers);
        if (!isOk(headers)) {
          this.forceAbort(headers, true);
          return;
        }
        const abortBytes = headers.get(ABORT_STATUS_HEADER);
        if (abortBytes !== undefined) {
          const decoded = decodeStatus(abortBytes);
          let remoteStatus: NonOkStatus;
          if (!isOk(decoded)) {
            remoteStatus = decoded;
          } else if (isOk(decoded.status)) {
            remoteStatus = abortedError(decoded.status.message);
          } else {
            remoteStatus = decoded.status;
          }
          this.remoteAborted = true;
          this.trailers = null;
          if (isOk(this.status)) this.status = remoteStatus;
          this.finish();
          return;
        }
        this.remoteHalfClosed = true;
        this.trailers = headers;
        const callbackStatus = await this.invokeMessageWithTimeout(null);
        if (!isOk(callbackStatus)) {
          this.forceAbort(callbackStatus, true);
          return;
        }
        this.maybeFinish();
        return;
      }
    } catch (error) {
      this.forceAbort(
        statusFromUnknown(error, 'WireStream receiver raised an exception.'),
        true,
      );
    } finally {
      this.incomingPumpRunning = false;
      if (this.incoming.length > 0) this.scheduleIncomingPump();
    }
  }

  private async invokeMessageWithTimeout(
    message: WireMessage | null,
  ): Promise<Status> {
    const callback = invokeWireCallback(this.onMessage, message);
    const timeoutMs = this.options.messageTimeoutMs;
    if (timeoutMs === null) return callback;
    const timeout = new Deferred<Status>();
    const timer = setTimeout(
      () =>
        timeout.resolve(
          deadlineExceededError('Timed out delivering a WireStream message.'),
        ),
      timeoutMs,
    );
    try {
      return await Promise.race([callback, timeout.promise]);
    } finally {
      clearTimeout(timer);
    }
  }

  private forceAbort(status: NonOkStatus, canCommunicate: boolean): void {
    if (this.finished || this.remoteAborted || this.localEnd === 'abort') return;
    this.status = status;
    this.trailers = null;
    this.localEnd = 'abort';
    this.outgoing.splice(0);
    if (canCommunicate && this.opened) {
      const packed = packStatus(status);
      if (isOk(packed)) {
        const message = makeHalfCloseMessage(
          new Map([[ABORT_STATUS_HEADER, packed]]),
        );
        const bytes = message.toMsgpack();
        if (isOk(bytes)) {
          this.outgoing.push({
            bytes,
            end: 'abort',
            messageId: this.nextMessageId++,
          });
          this.scheduleOutgoingPump();
          return;
        }
      }
    }
    this.finish();
  }

  private maybeFinish(): void {
    if (
      this.remoteAborted ||
      this.localEndSent === 'abort' ||
      (this.localEndSent === 'half-close' && this.remoteHalfClosed)
    ) {
      this.finish();
    }
  }

  private finish(terminalError?: NonOkStatus): void {
    if (this.finished) return;
    this.finished = true;
    if (terminalError !== undefined && isOk(this.status)) {
      this.status = terminalError;
    }
    this.clearTiming();
    this.incoming.splice(0);
    this.incomingBytes = 0;
    this.reassembler.clear();
    if (this.localEnd === 'half-close' && this.localEndSent !== 'half-close') {
      this.drainDone.resolve(
        isOk(this.status)
          ? failedPreconditionError(
              'WireStream finished before its half-close was drained.',
            )
          : this.status,
      );
    }
    if (!isOk(this.status)) {
      try {
        this.channel.close();
      } catch {
        // The primary terminal status remains authoritative.
      }
    }
    if (!this.doneCalled) {
      this.doneCalled = true;
      queueMicrotask(() => void this.invokeDone());
    }
  }

  private async invokeDone(): Promise<void> {
    const callbackStatus = await invokeWireCallback(this.onDone);
    if (!isOk(callbackStatus) && isOk(this.status)) this.status = callbackStatus;
    let cleanupStatus: Status = okStatus();
    try {
      const closed = this.channel.close();
      cleanupStatus = isStatus(closed)
        ? closed
        : internalError('BinaryChannel.close() returned a non-Status value.');
    } catch (error) {
      cleanupStatus = statusFromUnknown(
        error,
        'BinaryChannel.close() raised an exception.',
      );
    }
    try {
      const reset = this.channel.resetCallbacks();
      if (!isStatus(reset)) {
        if (isOk(cleanupStatus)) {
          cleanupStatus = internalError(
            'BinaryChannel.resetCallbacks() returned a non-Status value.',
          );
        }
      } else if (!isOk(reset) && isOk(cleanupStatus)) {
        cleanupStatus = reset;
      }
    } catch (error) {
      if (isOk(cleanupStatus)) {
        cleanupStatus = statusFromUnknown(
          error,
          'BinaryChannel.resetCallbacks() raised an exception.',
        );
      }
    }
    if (!isOk(cleanupStatus) && isOk(this.status)) this.status = cleanupStatus;
    this.finishedDone.resolve(this.status);
  }

  private handleChannelClosed(): void {
    this.opened = false;
    if (this.finished) return;
    const expected =
      this.remoteAborted ||
      this.localEndSent === 'abort' ||
      (this.localEndSent === 'half-close' && this.remoteHalfClosed);
    if (expected) this.maybeFinish();
    else this.finish(unavailableError('Channel closed before A11 termination.'));
  }

  private markActivity(): void {
    this.lastActivity = Date.now();
    this.armTiming();
  }

  private deadlineExpired(): boolean {
    return this.options.deadline !== null && this.options.deadline <= Date.now();
  }

  private armTiming(): void {
    this.clearTiming();
    if (this.finished) return;
    const now = Date.now();
    if (this.options.deadline !== null) {
      this.deadlineTimer = setTimeout(() => {
        this.forceAbort(
          deadlineExceededError('WireStream deadline exceeded.'),
          true,
        );
      }, Math.max(0, this.options.deadline - now));
    }
    if (this.options.messageTimeoutMs !== null && this.started) {
      this.activityTimer = setTimeout(() => {
        this.forceAbort(
          deadlineExceededError('Timed out waiting for WireStream activity.'),
          true,
        );
      }, Math.max(0, this.lastActivity + this.options.messageTimeoutMs - now));
    }
  }

  private clearTiming(): void {
    if (this.deadlineTimer !== null) clearTimeout(this.deadlineTimer);
    if (this.activityTimer !== null) clearTimeout(this.activityTimer);
    this.deadlineTimer = null;
    this.activityTimer = null;
  }
}
