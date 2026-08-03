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

export interface ChunkStoreWriterOptions {
  offset?: number;
  maxChunksToWriteAtOnce?: number;
  numChunksToBuffer?: number | null;
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

export interface ChunkStoreWrite {
  admitted: Promise<Status>;
  confirmation: Promise<StatusOr<number>>;
}

type Lifecycle = 'none' | 'close' | 'abort' | 'cancel';

/** Bounded, batched, stackless writer over a ChunkStore. */
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

  get queueSize(): number { return this.queue.length + this.pendingQueue.length; }
  getStatus(): Status | null { return this.status; }
  getAbortStatus(): Status | null { return this.stopStatus; }
  isWritable(): boolean { return this.status === null && !this.closing; }

  ensureStarted(): Status { return this.wake(); }

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

  put(chunk: Chunk, seq: number | null = null, final = false): Promise<StatusOr<number>> {
    return this.putChunk(chunk, seq, final);
  }

  waitForBufferToDrain(): Promise<Status> {
    if (this.status !== null && !isOk(this.status)) return Promise.resolve(this.status);
    if (this.outstanding === 0 && this.pendingQueue.length === 0) return Promise.resolve(okStatus());
    const waiter = new Deferred<Status>();
    this.drainWaiters.push(waiter);
    this.wake();
    return waiter.promise;
  }

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

  private startClose(requested: Status): Status {
    if (this.operation === 'close') return okStatus();
    this.operation = 'close';
    const generation = ++this.generation;
    let pending: Promise<Status>;
    try { pending = this.store.closeWritesWithStatus(requested); }
    catch (error) {
      this.closeDone(generation, requested, statusFromUnknown(error, 'ChunkStore close raised an exception'));
      return okStatus();
    }
    Promise.resolve(pending)
      .then((result) => this.closeDone(generation, requested, result))
      .catch((error: unknown) => this.closeDone(generation, requested, statusFromUnknown(error, 'ChunkStore close rejected')));
    return okStatus();
  }

  private closeDone(generation: number, requested: Status, returned: unknown): void {
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
