import { type ByteMap, type ByteMapInput, randomId } from './bytes.js';
import { Deferred } from './concurrency.js';
import { WireMessage } from './data.js';
import {
  failedPreconditionError,
  invalidArgumentError,
  isOk,
  okStatus,
  statusFromUnknown,
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

interface PendingPacket {
  source: MemoryBinaryChannel;
  bytes: Uint8Array;
}

class MemoryBinaryChannel implements BinaryChannel {
  peer: MemoryBinaryChannel | null = null;
  private callbacks: BinaryChannelCallbacks | null = null;
  private opened = false;
  private closed = false;
  private flushScheduled = false;
  private readonly incoming: PendingPacket[] = [];
  private outgoingBytes = 0;
  private readonly drainWaiters: Array<Deferred<Status>> = [];

  setCallbacks(callbacks: BinaryChannelCallbacks): Status {
    if (typeof callbacks !== 'object' || callbacks === null) {
      return invalidArgumentError('callbacks must implement BinaryChannelCallbacks.');
    }
    this.callbacks = callbacks;
    return okStatus();
  }

  resetCallbacks(): Status {
    this.callbacks = null;
    return okStatus();
  }

  async open(): Promise<Status> {
    if (this.closed) {
      return failedPreconditionError('In-process channel is closed.');
    }
    this.opened = true;
    try {
      this.callbacks?.onOpen();
    } catch (error) {
      return statusFromUnknown(error, 'In-process open callback raised an exception.');
    }
    this.scheduleFlush();
    return okStatus();
  }

  isOpen(): boolean {
    return this.opened && !this.closed;
  }

  send(packet: Uint8Array): Status {
    if (!this.isOpen()) {
      return failedPreconditionError('In-process channel is not open.');
    }
    const peer = this.peer;
    if (peer === null || peer.closed) {
      return failedPreconditionError('In-process peer is closed.');
    }
    try {
      const bytes = new Uint8Array(packet);
      this.outgoingBytes += bytes.byteLength;
      peer.incoming.push({ source: this, bytes });
      peer.scheduleFlush();
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Could not enqueue in-process packet.');
    }
  }

  bufferedAmount(): StatusOr<number> {
    return this.outgoingBytes;
  }

  waitForBufferedAmountLow(): Promise<Status> {
    if (this.outgoingBytes === 0) return Promise.resolve(okStatus());
    const waiter = new Deferred<Status>();
    this.drainWaiters.push(waiter);
    return waiter.promise;
  }

  close(): Status {
    if (this.closed) return okStatus();
    this.closeOne();
    const peer = this.peer;
    if (peer !== null) {
      // Model a transport close notification: packets admitted before close
      // are observable first, just as WebSocket/DataChannel message events are.
      setTimeout(() => peer.closeOne(), 0);
    }
    return okStatus();
  }

  getImpl(): unknown | null {
    return this;
  }

  private closeOne(): void {
    if (this.closed) return;
    this.closed = true;
    this.opened = false;
    for (const packet of this.incoming.splice(0)) {
      packet.source.packetDelivered(packet.bytes.byteLength);
    }
    queueMicrotask(() => {
      try {
        this.callbacks?.onClosed();
      } catch {
        // ChannelWireStream owns conversion of transport failures to statuses.
      }
    });
  }

  private scheduleFlush(): void {
    if (
      this.flushScheduled ||
      !this.opened ||
      this.closed ||
      this.callbacks === null ||
      this.incoming.length === 0
    ) {
      return;
    }
    this.flushScheduled = true;
    queueMicrotask(() => this.flush());
  }

  private flush(): void {
    this.flushScheduled = false;
    if (!this.opened || this.closed || this.callbacks === null) return;
    const packet = this.incoming.shift();
    if (packet === undefined) return;
    try {
      this.callbacks.onMessage(packet.bytes);
    } catch (error) {
      const status = statusFromUnknown(
        error,
        'In-process message callback raised an exception.',
      );
      try {
        this.callbacks.onError(status);
      } catch {
        // No further callback boundary exists here.
      }
    } finally {
      packet.source.packetDelivered(packet.bytes.byteLength);
    }
    if (this.incoming.length > 0) this.scheduleFlush();
  }

  private packetDelivered(size: number): void {
    this.outgoingBytes = Math.max(0, this.outgoingBytes - size);
    if (this.outgoingBytes !== 0) return;
    for (const waiter of this.drainWaiters.splice(0)) waiter.resolve(okStatus());
    try {
      this.callbacks?.onBufferedAmountLow();
    } catch {
      // The amount is still observable as zero by the sender pump.
    }
  }
}

function makeMemoryChannelPair(): [MemoryBinaryChannel, MemoryBinaryChannel] {
  const first = new MemoryBinaryChannel();
  const second = new MemoryBinaryChannel();
  first.peer = second;
  second.peer = first;
  return [first, second];
}

/**
 * A paired WireStream transport with no network dependency.
 *
 * Use this for tests, local agent composition, and bridges that need ordinary
 * WireStream lifecycle/backpressure semantics without a socket. Packets cross
 * an asynchronous in-memory boundary, so callbacks, half-close, drain, and
 * completion behave like the network transports rather than like direct
 * function calls.
 */
export class InProcessWireStream implements WireStream {
  private constructor(private readonly stream: ChannelWireStream) {}

  /**
   * Create client/server endpoints sharing one stream id.
   *
   * Call {@link WireStream.start} on the first endpoint and
   * {@link WireStream.accept} on the second. `options` applies to both sides;
   * `firstOptions` and `secondOptions` provide endpoint-specific overrides.
   */
  static createPair(
    options: WireStreamOptions = {},
    firstOptions: WireStreamOptions = {},
    secondOptions: WireStreamOptions = {},
    preassignedId = '',
    framing: ChannelFramingOptions = {},
  ): StatusOr<[InProcessWireStream, InProcessWireStream]> {
    if (typeof preassignedId !== 'string') {
      return invalidArgumentError('preassignedId must be a string.');
    }
    const id = preassignedId || randomId('inproc-');
    const [firstChannel, secondChannel] = makeMemoryChannelPair();
    const first = ChannelWireStream.create(
      firstChannel,
      id,
      ChannelEndpointRole.CLIENT,
      { ...options, ...firstOptions },
      framing,
    );
    if (!isOk(first)) return first;
    const second = ChannelWireStream.create(
      secondChannel,
      id,
      ChannelEndpointRole.SERVER,
      { ...options, ...secondOptions },
      framing,
    );
    if (!isOk(second)) return second;
    return [new InProcessWireStream(first), new InProcessWireStream(second)];
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
