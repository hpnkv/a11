import { statusToChunk } from './action_schema.js';
import { Deferred, storeCallbackScheduler } from './concurrency.js';
import { cloneChunk, hasChunkStoreShape, type ChunkStore } from './chunk_store.js';
import { Chunk, NodeFragment, WireMessage } from './data.js';
import {
  abortedError,
  dataLossError,
  failedPreconditionError,
  invalidArgumentError,
  isOk,
  isStatus,
  okStatus,
  outOfRangeError,
  resourceExhaustedError,
  statusFromUnknown,
  type NonOkStatus,
  type Status,
  type StatusOr,
} from './status.js';

const UINT32_MAX = 0xffff_ffff;
const UINT32_RANGE = 0x1_0000_0000;

/** Control sequence assignment, batching, and producer backpressure. */
export interface ChunkStoreWriterOptions {
  /** First automatically assigned sequence number. */
  offset?: number;
  /** Maximum fragments committed in one store batch. */
  maxChunksToWriteAtOnce?: number;
  /** Outstanding chunk limit; `null` admits without a queue bound. */
  numChunksToBuffer?: number | null;
  /** Omit repeated MIME types on contiguous chunks to reduce wire size. */
  stickyMimetype?: boolean;
}

interface NormalizedWriterOptions {
  offset: number;
  maxChunksToWriteAtOnce: number;
  numChunksToBuffer: number | null;
  stickyMimetype: boolean;
}

function normalizeOptions(options: ChunkStoreWriterOptions): StatusOr<NormalizedWriterOptions> {
  try {
    if (typeof options !== 'object' || options === null) {
      return invalidArgumentError('Writer options must be an object.');
    }
    const result: NormalizedWriterOptions = {
      offset: options.offset ?? 0,
      maxChunksToWriteAtOnce: options.maxChunksToWriteAtOnce ?? 8,
      numChunksToBuffer: options.numChunksToBuffer ?? null,
      stickyMimetype: options.stickyMimetype ?? false,
    };
    if (typeof result.stickyMimetype !== 'boolean') {
      return invalidArgumentError('stickyMimetype must be boolean.');
    }
    if (!Number.isSafeInteger(result.offset) || result.offset < 0 || result.offset > UINT32_MAX) {
      return outOfRangeError('offset must be a uint32 integer.');
    }
    if (!Number.isSafeInteger(result.maxChunksToWriteAtOnce) || result.maxChunksToWriteAtOnce <= 0) {
      return invalidArgumentError('maxChunksToWriteAtOnce must be positive.');
    }
    if (result.maxChunksToWriteAtOnce > UINT32_RANGE) return outOfRangeError('maxChunksToWriteAtOnce exceeds 2^32.');
    if (result.numChunksToBuffer !== null && (
      !Number.isSafeInteger(result.numChunksToBuffer) || result.numChunksToBuffer <= 0
    )) {
      return invalidArgumentError('numChunksToBuffer must be positive or null.');
    }
    if (result.numChunksToBuffer !== null && result.numChunksToBuffer > UINT32_RANGE) {
      return outOfRangeError('numChunksToBuffer exceeds 2^32.');
    }
    return result;
  } catch (error) {
    return invalidArgumentError('Writer options could not be read.', [], error);
  }
}

/** Minimal transport seam used to tee stored node fragments to peers. */
export interface WritableWireStream {
  getId(): string;
  send(message: WireMessage): Status;
}

interface WriteElement {
  chunk: Chunk;
  seq: number | null;
  continued: boolean;
  admission: Deferred<Status>;
  confirmation: Deferred<StatusOr<number>>;
}

/** Separate queue-admission backpressure from backing-store confirmation. */
export interface ChunkStoreWrite {
  /** Resolves once the bounded writer queue admits the chunk. */
  admitted: Promise<Status>;
  /** Resolves to its sequence after the backing store accepts the fragment. */
  confirmation: Promise<StatusOr<number>>;
}

type Lifecycle = 'none' | 'close' | 'abort' | 'cancel';

/**
 * Bounded, batched, stackless writer over a {@link ChunkStore}.
 *
 * Producers enqueue chunks in logical sequence order. Await `admitted` to
 * respect queue backpressure and `confirmation` to know storage accepted the
 * fragment. The writer also calls `send` on attached wire streams; that call
 * confirms local transport admission, not delivery by the remote agent.
 *
 * A final write fixes the logical end of the sequence. Lifecycle methods then
 * either drain and seal storage, propagate an error, or cancel immediately.
 */
export class ChunkStoreWriter {
  readonly store: ChunkStore;
  readonly options: Readonly<NormalizedWriterOptions>;

  private nextOffsetSeq: number;
  private nextStickySeq: number;
  private currentMimetype = '';
  private readonly queue: WriteElement[] = [];
  private readonly pendingQueue: WriteElement[] = [];
  private outstanding = 0;
  private status: Status | null = null;
  private closing = false;
  private stopStatus: NonOkStatus | null = null;
  private lifecycle: Lifecycle = 'none';
  private lifecycleDone: Deferred<Status> | null = null;
  private readonly drainWaiters: Array<Deferred<Status>> = [];
  private readonly streams: WritableWireStream[] = [];
  private operation: 'none' | 'write' | 'close' = 'none';
  private queued = false;
  private generation = 0;
  private activeBatch: WriteElement[] = [];

  private constructor(store: ChunkStore, options: NormalizedWriterOptions) {
    this.store = store;
    this.options = Object.freeze({ ...options });
    this.nextOffsetSeq = options.offset;
    this.nextStickySeq = options.offset;
  }

  /** Validate options and create a writer over the supplied store. */
  static create(
    store: ChunkStore,
    options: ChunkStoreWriterOptions = {},
  ): StatusOr<ChunkStoreWriter> {
    try {
      if (!hasChunkStoreShape(store)) {
        return invalidArgumentError('store must implement ChunkStore.');
      }
      const normalized = normalizeOptions(options);
      return isOk(normalized) ? new ChunkStoreWriter(store, normalized) : normalized;
    } catch (error) {
      return statusFromUnknown(error, 'ChunkStoreWriter could not be created.');
    }
  }

  /** Number of admitted and backpressured chunks still awaiting completion. */
  get queueSize(): number { return this.queue.length + this.pendingQueue.length; }
  /** Terminal status, or `null` while the writer remains open. */
  getStatus(): Status | null { return this.status; }
  /** Requested abort/cancel status, or `null` for a graceful lifecycle. */
  getAbortStatus(): Status | null { return this.stopStatus; }
  /** Whether ordinary writes can still be accepted. */
  isWritable(): boolean { return this.status === null && !this.closing; }

  /** Schedule the flush pump before the first write; safe to call repeatedly. */
  ensureStarted(): Status { return this.wake(); }

  /** Enqueue a chunk and expose admission and persistence as separate promises. */
  enqueueChunk(
    chunk: Chunk,
    seq: number | null = null,
    final = false,
    ensureStarted = true,
  ): ChunkStoreWrite {
    const admission = new Deferred<Status>();
    const confirmation = new Deferred<StatusOr<number>>();
    const failed = (status: NonOkStatus): ChunkStoreWrite => {
      admission.resolve(status);
      confirmation.resolve(status);
      return { admitted: admission.promise, confirmation: confirmation.promise };
    };
    if (!(chunk instanceof Chunk)) return failed(invalidArgumentError('chunk must be a Chunk.'));
    const validation = chunk.validate();
    if (!isOk(validation)) return failed(validation);
    if (seq !== null && (!Number.isSafeInteger(seq) || seq < 0 || seq > UINT32_MAX)) {
      return failed(invalidArgumentError('seq must be a uint32 integer or null.'));
    }
    if (typeof final !== 'boolean') return failed(invalidArgumentError('final must be boolean.'));
    if (this.status !== null) {
      return failed(isOk(this.status) ? failedPreconditionError('ChunkStoreWriter is closed') : this.status);
    }
    if (this.closing) return failed(this.stopStatus ?? failedPreconditionError('ChunkStoreWriter is closing'));
    const requestedSeq = seq;
    if (seq === null && this.options.offset !== 0) {
      if (this.nextOffsetSeq > UINT32_MAX) return failed(resourceExhaustedError('Maximum writer sequence number exceeded'));
      seq = this.nextOffsetSeq++;
    }
    if (this.options.stickyMimetype) {
      chunk = cloneChunk(chunk);
      const explicitSequenceGap = requestedSeq !== null && requestedSeq !== this.nextStickySeq;
      const mimetype = chunk.mimetype;
      if (explicitSequenceGap || mimetype !== this.currentMimetype) {
        this.currentMimetype = mimetype;
      } else if (chunk.metadata !== null) {
        chunk.metadata.mimetype = '';
        if (chunk.metadata.timestamp === null && chunk.metadata.attributes.size === 0) {
          chunk.metadata = null;
        }
      }
      this.nextStickySeq = requestedSeq === null ? this.nextStickySeq + 1 : requestedSeq + 1;
    }
    const element: WriteElement = {
      chunk,
      seq,
      continued: !final,
      admission,
      confirmation,
    };
    if (this.options.numChunksToBuffer === null || this.outstanding < this.options.numChunksToBuffer) {
      this.queue.push(element);
      ++this.outstanding;
      admission.resolve(okStatus());
      if (ensureStarted) this.wake();
    } else {
      this.pendingQueue.push(element);
    }
    return { admitted: admission.promise, confirmation: confirmation.promise };
  }

  /** Apply backpressure, persist a chunk, and return its confirmed sequence. */
  async putChunk(
    chunk: Chunk,
    seq: number | null = null,
    final = false,
  ): Promise<StatusOr<number>> {
    const write = this.enqueueChunk(chunk, seq, final, true);
    const admitted = await write.admitted;
    if (!isOk(admitted)) return admitted;
    return write.confirmation;
  }

  /** Alias for {@link putChunk}. */
  put(chunk: Chunk, seq: number | null = null, final = false): Promise<StatusOr<number>> {
    return this.putChunk(chunk, seq, final);
  }

  /** Await all currently outstanding writes without closing the writer. */
  waitForBufferToDrain(): Promise<Status> {
    if (this.status !== null && !isOk(this.status)) return Promise.resolve(this.status);
    if (this.outstanding === 0 && this.pendingQueue.length === 0) return Promise.resolve(okStatus());
    const waiter = new Deferred<Status>();
    this.drainWaiters.push(waiter);
    this.wake();
    return waiter.promise;
  }

  /**
   * Write `chunk` as the final fragment and, unless told not to, close.
   *
   * The chunk is enqueued before closure is asked for, and that order is the
   * whole of the synchronisation: the pump only starts its close once nothing
   * is outstanding, and admits whatever the bounded buffer held back before
   * that count reaches zero. So a close requested here cannot overtake the
   * chunk. With `wait` false the returned promise resolves immediately and both
   * are left to the pump; a failure is reported rather than dropped, and stays
   * visible through {@link getStatus}. {@link AsyncNode.finalize} is the entry
   * point application code wants -- it serializes values for you.
   */
  finalize(chunk: Chunk, seq: number | null = null, wait = false, close = true): Promise<Status> {
    const write = this.enqueueChunk(chunk, seq, true, true);
    const closed = close ? this.drainAndClose() : Promise.resolve(okStatus());
    if (wait) {
      // A close cannot complete before the final chunk is confirmed, and fails
      // if that write fails, so it is the only promise this needs.
      return close
        ? closed
        : write.confirmation.then((stored) => (isOk(stored) ? okStatus() : stored));
    }
    const report = (what: string) => (result: Status | StatusOr<number>) => {
      if (!isOk(result)) console.warn(`a11: AsyncNode ${what} failed: ${String(result.message)}`);
    };
    void write.confirmation.then(report('final write'));
    if (close) void closed.then(report('close'));
    return Promise.resolve(okStatus());
  }

  /**
   * Flush queued chunks and close the backing store to further writes.
   *
   * This does not append a final fragment. Mark the last write `final` (or call
   * {@link finalize}) when readers need a final sequence number to identify the
   * logical end of the stream.
   */
  drainAndClose(): Promise<Status> {
    if (this.lifecycle !== 'none') {
      return this.lifecycle === 'close' && this.lifecycleDone !== null
        ? this.lifecycleDone.promise
        : Promise.resolve(failedPreconditionError('ChunkStoreWriter is already being aborted'));
    }
    if (this.status !== null) {
      return Promise.resolve(isOk(this.status)
        ? failedPreconditionError('ChunkStoreWriter has already stopped')
        : this.status);
    }
    this.closing = true;
    this.lifecycle = 'close';
    this.lifecycleDone = new Deferred<Status>();
    this.wake();
    return this.lifecycleDone.promise;
  }

  /** Reject queued writes and seal the store with a non-OK producer status. */
  abortWithStatus(status: Status): Promise<Status> {
    if (!isStatus(status)) {
      return Promise.resolve(invalidArgumentError('Abort status must be an A11 Status'));
    }
    if (isOk(status)) return Promise.resolve(invalidArgumentError('Abort status must be non-OK'));
    if (this.lifecycle !== 'none') {
      return this.lifecycle === 'abort' && this.lifecycleDone !== null
        ? this.lifecycleDone.promise
        : Promise.resolve(failedPreconditionError('ChunkStoreWriter is already being closed'));
    }
    if (this.status !== null) return Promise.resolve(isOk(this.status) ? failedPreconditionError('ChunkStoreWriter has already stopped') : this.status);
    this.closing = true;
    this.stopStatus = status;
    this.lifecycle = 'abort';
    this.lifecycleDone = new Deferred<Status>();
    this.wake();
    return this.lifecycleDone.promise;
  }

  /** Abandon queued work immediately without persisting a store error status. */
  cancel(): Promise<Status> {
    if (this.lifecycle !== 'none') {
      return this.lifecycle === 'cancel' && this.lifecycleDone !== null
        ? this.lifecycleDone.promise
        : Promise.resolve(failedPreconditionError('ChunkStoreWriter is already stopping'));
    }
    if (this.status !== null) return Promise.resolve(failedPreconditionError('ChunkStoreWriter has already stopped'));
    this.closing = true;
    this.stopStatus = abortedError('ChunkStoreWriter was cancelled');
    this.lifecycle = 'cancel';
    this.lifecycleDone = new Deferred<Status>();
    this.wake();
    return this.lifecycleDone.promise;
  }

  /** Tee stored fragments to a stream; a send failure stops later writes. */
  attachStream(stream: WritableWireStream): Status {
    try {
      if (
        stream === null ||
        typeof stream !== 'object' ||
        typeof stream.send !== 'function' ||
        typeof stream.getId !== 'function'
      ) {
        return invalidArgumentError('stream must implement WritableWireStream.');
      }
      const id = stream.getId();
      if (typeof id !== 'string' || id.length === 0) {
        return invalidArgumentError('WritableWireStream.getId() must return a non-empty string.');
      }
    } catch (error) {
      return statusFromUnknown(error, 'WireStream getId raised an exception');
    }
    if (!this.streams.includes(stream)) this.streams.push(stream);
    return okStatus();
  }

  /** Stop teeing future stored fragments to a previously attached stream. */
  detachStream(stream: WritableWireStream): Status {
    const index = this.streams.indexOf(stream);
    if (index >= 0) this.streams.splice(index, 1);
    return okStatus();
  }

  private wake(): Status {
    if (this.queued || this.operation !== 'none') return okStatus();
    this.queued = true;
    return storeCallbackScheduler.schedule(
      () => this.drive(),
      (status) => this.fail(status),
    );
  }

  private drive(): Status {
    this.queued = false;
    if (this.operation !== 'none') return okStatus();

    if (this.stopStatus !== null && this.status === null) {
      this.rejectElements(this.queue.splice(0), this.stopStatus);
      this.rejectElements(this.pendingQueue.splice(0), this.stopStatus);
      this.outstanding = 0;
      this.status = this.stopStatus;
      this.resolveDrainWaiters(this.stopStatus);
    }
    if (this.lifecycle === 'cancel' && this.status !== null) {
      this.finishLifecycle(okStatus());
      return okStatus();
    }
    if (this.lifecycle === 'abort' && this.status !== null) {
      return this.startClose(this.stopStatus!);
    }
    if (this.status !== null && !isOk(this.status)) {
      this.finishLifecycle(this.status);
      return okStatus();
    }
    if (this.queue.length > 0) return this.startWrite();
    if (this.lifecycle === 'close' && this.outstanding === 0 && this.pendingQueue.length === 0) {
      return this.startClose(okStatus());
    }
    if (this.outstanding === 0 && this.pendingQueue.length === 0) this.resolveDrainWaiters(okStatus());
    return okStatus();
  }

  private startWrite(): Status {
    const implicit = this.queue[0]!.seq === null;
    this.activeBatch = [];
    while (
      this.queue.length > 0 &&
      this.activeBatch.length < this.options.maxChunksToWriteAtOnce &&
      (this.queue[0]!.seq === null) === implicit
    ) {
      this.activeBatch.push(this.queue.shift()!);
    }
    this.operation = 'write';
    const generation = ++this.generation;
    let id: StatusOr<string>;
    try { id = this.store.getId(); }
    catch (error) {
      this.writeDone(generation, statusFromUnknown(error, 'ChunkStore getId raised an exception'));
      return okStatus();
    }
    if (!isOk(id)) {
      this.writeDone(generation, id);
      return okStatus();
    }
    if (typeof id !== 'string' || id.length === 0) {
      this.writeDone(generation, dataLossError('ChunkStore getId returned an invalid node id'));
      return okStatus();
    }
    const fragments = this.activeBatch.map((element) => new NodeFragment({
      id,
      data: element.chunk,
      seq: element.seq,
      continued: element.continued,
    }));
    let pending: Promise<StatusOr<number[]>>;
    try { pending = this.store.putMany(fragments); }
    catch (error) {
      this.writeDone(generation, statusFromUnknown(error, 'ChunkStore putMany raised an exception'));
      return okStatus();
    }
    Promise.resolve(pending)
      .then((result) => this.writeDone(generation, result))
      .catch((error: unknown) => this.writeDone(generation, statusFromUnknown(error, 'ChunkStore putMany rejected')));
    return okStatus();
  }

  private writeDone(generation: number, result: unknown): void {
    try {
      if (this.operation !== 'write' || generation !== this.generation) return;
      this.operation = 'none';
      let operationStatus: Status = okStatus();
      if (isStatus(result) && !isOk(result)) operationStatus = result;
      else if (!Array.isArray(result)) operationStatus = dataLossError('ChunkStore putMany returned a value that is not an array');
      else if (result.some((seq) => !Number.isSafeInteger(seq) || seq < 0 || seq > UINT32_MAX)) {
        operationStatus = dataLossError('ChunkStore putMany returned an invalid sequence number');
      } else if (result.length !== this.activeBatch.length) operationStatus = dataLossError('ChunkStore putMany returned the wrong number of sequences');
      else {
        for (let index = 0; index < result.length; ++index) {
          const explicit = this.activeBatch[index]!.seq;
          if (explicit !== null && explicit !== result[index]) {
            operationStatus = dataLossError('ChunkStore changed an explicit sequence number');
            break;
          }
        }
      }
      let teeStatus: Status = okStatus();
      if (isOk(operationStatus) && this.streams.length > 0 && Array.isArray(result)) {
        let id: StatusOr<string>;
        try { id = this.store.getId(); }
        catch (error) { id = statusFromUnknown(error, 'ChunkStore getId raised an exception'); }
        if (!isOk(id)) teeStatus = id;
        else if (typeof id !== 'string' || id.length === 0) {
          teeStatus = dataLossError('ChunkStore getId returned an invalid node id');
        }
        else {
          const message = new WireMessage({
            nodeFragments: this.activeBatch.map((element, index) => new NodeFragment({
              id,
              data: element.chunk,
              seq: result[index]!,
              continued: element.continued,
            })),
          });
          for (const stream of this.streams) {
            try {
              const returned = stream.send(message);
              teeStatus = isStatus(returned)
                ? returned
                : dataLossError('WireStream send returned an invalid status');
            }
            catch (error) { teeStatus = statusFromUnknown(error, 'WireStream send raised an exception'); }
            if (!isOk(teeStatus)) break;
          }
        }
      }
      const completed = this.activeBatch.splice(0);
      this.outstanding -= completed.length;
      if (isOk(operationStatus) && Array.isArray(result)) {
        completed.forEach((element, index) => element.confirmation.resolve(result[index]!));
      } else {
        this.rejectElements(
          completed,
          operationStatus as NonOkStatus,
        );
      }
      if (this.stopStatus !== null) this.status = this.stopStatus;
      else if (!isOk(operationStatus)) this.status = operationStatus;
      else if (!isOk(teeStatus)) this.status = teeStatus;
      if (this.status !== null && !isOk(this.status)) {
        this.rejectElements(this.queue.splice(0), this.status);
        this.rejectElements(this.pendingQueue.splice(0), this.status);
        this.outstanding = 0;
        this.resolveDrainWaiters(this.status);
      } else {
        this.admitPending();
        if (this.outstanding === 0) this.resolveDrainWaiters(okStatus());
      }
      this.wake();
    } catch (error) {
      this.operation = 'none';
      this.fail(statusFromUnknown(error, 'Processing ChunkStore putMany result raised an exception'));
    }
  }

  /**
   * Tell attached streams that this writer closed.
   *
   * A peer ends a node on a not-continued fragment and closing writes none, so
   * the graceful path sends one closure marker after the last teed batch —
   * draining is already synchronised with the tee, since the close only starts
   * once every batch has gone out. Aborts send nothing here; the action layer
   * already fans failures out.
   */
  private teeClose(closeStatus: Status): Status {
    if (this.lifecycle !== 'close' || this.streams.length === 0) return okStatus();
    let id: StatusOr<string>;
    try { id = this.store.getId(); }
    catch (error) { id = statusFromUnknown(error, 'ChunkStore getId raised an exception'); }
    if (!isOk(id)) return id;
    if (typeof id !== 'string' || id.length === 0) {
      return dataLossError('ChunkStore getId returned an invalid node id');
    }
    const marker = statusToChunk(closeStatus, true);
    if (!isOk(marker)) return marker;
    const message = new WireMessage({
      nodeFragments: [new NodeFragment({ id, data: marker, seq: 0, continued: false })],
    });
    for (const stream of this.streams) {
      try {
        const returned = stream.send(message);
        if (isStatus(returned) && !isOk(returned)) return returned;
      } catch (error) {
        return statusFromUnknown(error, 'WireStream send raised an exception');
      }
    }
    return okStatus();
  }

  private startClose(requested: Status): Status {
    if (this.operation === 'close') return okStatus();
    this.operation = 'close';
    const generation = ++this.generation;
    const teeStatus = this.teeClose(requested);
    let pending: Promise<Status>;
    try { pending = this.store.closeWritesWithStatus(requested); }
    catch (error) {
      this.closeDone(generation, requested, teeStatus, statusFromUnknown(error, 'ChunkStore close raised an exception'));
      return okStatus();
    }
    Promise.resolve(pending)
      .then((result) => this.closeDone(generation, requested, teeStatus, result))
      .catch((error: unknown) => this.closeDone(generation, requested, teeStatus, statusFromUnknown(error, 'ChunkStore close rejected')));
    return okStatus();
  }

  private closeDone(generation: number, requested: Status, teeStatus: Status, returned: unknown): void {
    try {
      if (this.operation !== 'close' || generation !== this.generation) return;
      this.operation = 'none';
      if (!isStatus(returned)) {
        this.status = dataLossError('ChunkStore closeWritesWithStatus returned an invalid status');
        this.finishLifecycle(this.status);
        return;
      }
      if (returned.code !== requested.code) {
        this.status = dataLossError('ChunkStore closed with a different status than requested');
        this.finishLifecycle(this.status);
        return;
      }
      // A failed closure marker cannot un-close the store, exactly as a failed
      // data tee cannot revoke store confirmations: the send error becomes the
      // writer's terminal status so the producer learns its peer was not told.
      if (!isOk(teeStatus)) {
        this.status = teeStatus;
        this.closing = false;
        this.finishLifecycle(teeStatus);
        return;
      }
      this.status = requested;
      this.closing = false;
      this.finishLifecycle(okStatus());
    } catch (error) {
      this.operation = 'none';
      this.fail(statusFromUnknown(error, 'Processing ChunkStore close result raised an exception'));
    }
  }

  private admitPending(): void {
    while (
      this.pendingQueue.length > 0 &&
      (this.options.numChunksToBuffer === null || this.outstanding < this.options.numChunksToBuffer)
    ) {
      const element = this.pendingQueue.shift()!;
      this.queue.push(element);
      ++this.outstanding;
      element.admission.resolve(okStatus());
    }
  }

  private rejectElements(elements: WriteElement[], status: NonOkStatus): void {
    for (const element of elements) {
      element.admission.resolve(status);
      element.confirmation.resolve(status);
    }
  }

  private resolveDrainWaiters(status: Status): void {
    for (const waiter of this.drainWaiters.splice(0)) waiter.resolve(status);
  }

  private finishLifecycle(status: Status): void {
    if (this.lifecycleDone !== null && !this.lifecycleDone.settled) this.lifecycleDone.resolve(status);
  }

  private fail(status: Status): void {
    if (isOk(status)) return;
    this.status = status;
    this.rejectElements(this.activeBatch.splice(0), status);
    this.rejectElements(this.queue.splice(0), status);
    this.rejectElements(this.pendingQueue.splice(0), status);
    this.outstanding = 0;
    this.resolveDrainWaiters(status);
    this.finishLifecycle(status);
  }
}
