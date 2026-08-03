import { Deferred, storeCallbackScheduler } from './concurrency.js';
import { hasChunkStoreShape, type ChunkStore } from './chunk_store.js';
import { Chunk, ChunkMetadata, NodeFragment } from './data.js';
import {
  abortedError,
  dataLossError,
  deadlineExceededError,
  invalidArgumentError,
  isNotFoundError,
  isOk,
  isStatus,
  okStatus,
  outOfRangeError,
  statusFromUnknown,
  type Status,
  type StatusOr,
} from './status.js';

const UINT32_MAX = 0xffff_ffff;
const UINT32_RANGE = 0x1_0000_0000;

export interface ChunkStoreReaderOptions {
  ordered?: boolean;
  popChunks?: boolean;
  numChunksToBuffer?: number;
  offset?: number;
  maxChunksToRead?: number | null;
  stickyMimetype?: boolean;
}

interface NormalizedReaderOptions {
  ordered: boolean;
  popChunks: boolean;
  numChunksToBuffer: number;
  offset: number;
  maxChunksToRead: number | null;
  stickyMimetype: boolean;
}

function normalizeOptions(options: ChunkStoreReaderOptions): StatusOr<NormalizedReaderOptions> {
  try {
    if (typeof options !== 'object' || options === null) {
      return invalidArgumentError('Reader options must be an object.');
    }
    const result: NormalizedReaderOptions = {
      ordered: options.ordered ?? true,
      popChunks: options.popChunks ?? false,
      numChunksToBuffer: options.numChunksToBuffer ?? 32,
      offset: options.offset ?? 0,
      maxChunksToRead: options.maxChunksToRead ?? null,
      stickyMimetype: options.stickyMimetype ?? false,
    };
    if (
      typeof result.ordered !== 'boolean' ||
      typeof result.popChunks !== 'boolean' ||
      typeof result.stickyMimetype !== 'boolean'
    ) {
      return invalidArgumentError('Reader ordered, popChunks, and stickyMimetype options must be boolean.');
    }
    if (!Number.isSafeInteger(result.numChunksToBuffer) || result.numChunksToBuffer < 0 || result.numChunksToBuffer > UINT32_RANGE) {
      return outOfRangeError('numChunksToBuffer must be between 0 and 2^32.');
    }
    if (!Number.isSafeInteger(result.offset) || result.offset < 0 || result.offset > UINT32_MAX) {
      return outOfRangeError('offset must be a uint32 integer.');
    }
    if (result.maxChunksToRead !== null && (
      !Number.isSafeInteger(result.maxChunksToRead) ||
      result.maxChunksToRead < 0 ||
      result.maxChunksToRead > UINT32_RANGE
    )) {
      return outOfRangeError('maxChunksToRead must be between 0 and 2^32 or null.');
    }
    return result;
  } catch (error) {
    return invalidArgumentError('Reader options could not be read.', [], error);
  }
}

interface ReadRequest {
  active: boolean;
  deferred: Deferred<StatusOr<NodeFragment | null>>;
  timer: ReturnType<typeof setTimeout> | null;
}

/** Fair, stackless, prefetching cursor over a ChunkStore. */
export class ChunkStoreReader {
  readonly store: ChunkStore;
  readonly options: Readonly<NormalizedReaderOptions>;

  private position: number;
  private chunksRead = 0;
  private currentMimetype = '';
  private status: Status | null = null;
  private readonly buffer: NodeFragment[] = [];
  private readonly pendingReads: ReadRequest[] = [];
  private operation: 'none' | 'fetch' | 'clear' = 'none';
  private queued = false;
  private generation = 0;
  private readonly done = new Deferred<Status>();

  private constructor(store: ChunkStore, options: NormalizedReaderOptions) {
    this.store = store;
    this.options = Object.freeze({ ...options });
    this.position = options.offset;
  }

  static create(
    store: ChunkStore,
    options: ChunkStoreReaderOptions = {},
  ): StatusOr<ChunkStoreReader> {
    try {
      if (!hasChunkStoreShape(store)) {
        return invalidArgumentError('store must implement ChunkStore.');
      }
      const normalized = normalizeOptions(options);
      if (!isOk(normalized)) return normalized;
      const reader = new ChunkStoreReader(store, normalized);
      const started = reader.ensureStarted();
      return isOk(started) ? reader : started;
    } catch (error) {
      return statusFromUnknown(error, 'ChunkStoreReader could not be created.');
    }
  }

  get bufferSize(): number { return this.buffer.length; }

  getStatus(): Status { return this.status ?? okStatus(); }

  ensureStarted(): Status { return this.wake(); }

  cancel(): Status {
    if (this.status === null) this.status = abortedError('ChunkStoreReader was cancelled');
    this.collectAvailable();
    this.completeDone();
    return okStatus();
  }

  wait(): Promise<Status> { return this.done.promise; }

  next(timeoutMs?: number): Promise<StatusOr<NodeFragment | null>> {
    if (timeoutMs !== undefined && (!Number.isFinite(timeoutMs) || timeoutMs < 0)) {
      return Promise.resolve(invalidArgumentError('timeoutMs must be non-negative or omitted.'));
    }
    const request: ReadRequest = {
      active: true,
      deferred: new Deferred<StatusOr<NodeFragment | null>>(),
      timer: null,
    };
    if (timeoutMs !== undefined) {
      request.timer = setTimeout(() => {
        if (!request.active) return;
        request.active = false;
        request.deferred.resolve(deadlineExceededError('ChunkStoreReader next timed out before a fragment was available'));
        this.wake();
      }, timeoutMs);
    }
    this.pendingReads.push(request);
    this.wake();
    return request.deferred.promise;
  }

  async *values(timeoutMs?: number): AsyncGenerator<StatusOr<NodeFragment>, void, void> {
    while (true) {
      const result = await this.next(timeoutMs);
      if (!isOk(result)) {
        yield result;
        return;
      }
      if (result === null) return;
      yield result;
    }
  }

  [Symbol.asyncIterator](): AsyncGenerator<StatusOr<NodeFragment>, void, void> {
    return this.values();
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
    this.collectAvailable();
    if (this.status !== null || this.operation !== 'none') {
      this.completeDone();
      return okStatus();
    }
    if (this.options.maxChunksToRead === 0) {
      this.status = okStatus();
      this.collectAvailable();
      this.completeDone();
      return okStatus();
    }
    const pendingCount = this.pendingReads.filter((request) => request.active).length;
    if (this.buffer.length >= this.options.numChunksToBuffer + pendingCount) return okStatus();
    if (this.position > UINT32_MAX) {
      this.fail(outOfRangeError('ChunkStoreReader position exceeds the sequence range'));
      return okStatus();
    }
    this.operation = 'fetch';
    const generation = ++this.generation;
    let pending: Promise<StatusOr<NodeFragment>>;
    try {
      pending = this.options.ordered
        ? this.store.get(this.position)
        : this.store.getByArrivalOrder(this.position);
    } catch (error) {
      this.fetchDone(generation, statusFromUnknown(error, 'ChunkStore reader fetch raised an exception'));
      return okStatus();
    }
    Promise.resolve(pending)
      .then((result) => this.fetchDone(generation, result))
      .catch((error: unknown) => this.fetchDone(generation, statusFromUnknown(error, 'ChunkStore reader fetch rejected')));
    return okStatus();
  }

  private fetchDone(generation: number, result: unknown): void {
    try {
      if (this.operation !== 'fetch' || generation !== this.generation) return;
      this.operation = 'none';
      if (this.status !== null) {
        this.wake();
        return;
      }
      if (isStatus(result) && !isOk(result)) {
        this.status = isNotFoundError(result) && this.options.maxChunksToRead === null
          ? okStatus()
          : result;
        this.wake();
        return;
      }
      if (!(result instanceof NodeFragment)) {
        this.fail(dataLossError('ChunkStore returned a value that is not a NodeFragment'));
        return;
      }
      const validation = result.validate();
      if (!isStatus(validation) || !isOk(validation)) {
        this.fail(
          isStatus(validation) && !isOk(validation)
            ? validation
            : dataLossError('NodeFragment validation returned an invalid status'),
        );
        return;
      }
      if (result.seq === null) {
        this.fail(dataLossError('ChunkStore returned a fragment without a sequence number'));
        return;
      }
      if (!this.options.popChunks) {
        this.finishFragment(result);
        this.wake();
        return;
      }
      this.operation = 'clear';
      const clearGeneration = ++this.generation;
      let pending: Promise<StatusOr<NodeFragment>>;
      try {
        pending = this.store.clearData(result.seq);
      } catch (error) {
        this.clearDone(clearGeneration, statusFromUnknown(error, 'ChunkStore clear raised an exception'));
        return;
      }
      Promise.resolve(pending)
        .then((cleared) => this.clearDone(clearGeneration, cleared))
        .catch((error: unknown) => this.clearDone(clearGeneration, statusFromUnknown(error, 'ChunkStore clear rejected')));
    } catch (error) {
      this.operation = 'none';
      this.fail(statusFromUnknown(error, 'Processing ChunkStore fetch result raised an exception'));
    }
  }

  private clearDone(generation: number, result: unknown): void {
    try {
      if (this.operation !== 'clear' || generation !== this.generation) return;
      this.operation = 'none';
      if (this.status === null) {
        if (isStatus(result) && !isOk(result)) this.status = result;
        else if (!(result instanceof NodeFragment)) {
          this.status = dataLossError('ChunkStore clearData returned a value that is not a NodeFragment');
        } else {
          const validation = result.validate();
          if (!isStatus(validation) || !isOk(validation)) {
            this.status = isStatus(validation) && !isOk(validation)
              ? validation
              : dataLossError('NodeFragment validation returned an invalid status');
          } else {
            this.finishFragment(result);
          }
        }
      }
      this.wake();
    } catch (error) {
      this.operation = 'none';
      this.fail(statusFromUnknown(error, 'Processing ChunkStore clear result raised an exception'));
    }
  }

  private finishFragment(fragment: NodeFragment): void {
    if (this.options.ordered && this.options.stickyMimetype && fragment.data instanceof Chunk) {
      const mimetype = fragment.data.mimetype;
      if (mimetype !== '') {
        if (mimetype !== this.currentMimetype) this.currentMimetype = mimetype;
      } else if (this.currentMimetype !== '') {
        if (fragment.data.metadata === null) fragment.data.metadata = new ChunkMetadata();
        fragment.data.metadata.mimetype = this.currentMimetype;
      }
    }
    this.buffer.push(fragment);
    ++this.chunksRead;
    ++this.position;
    if (this.options.maxChunksToRead !== null && this.chunksRead === this.options.maxChunksToRead) {
      this.status = okStatus();
    } else if (this.options.ordered && !fragment.continued) {
      this.status = this.options.maxChunksToRead === null
        ? okStatus()
        : outOfRangeError(
            `The final fragment arrived after ${this.chunksRead} chunks, before maxChunksToRead=${this.options.maxChunksToRead}`,
          );
    }
    this.collectAvailable();
  }

  private collectAvailable(): void {
    while (this.buffer.length > 0) {
      const request = this.popRequest();
      if (request === null) break;
      const fragment = this.buffer.shift()!;
      this.resolveRequest(request, fragment);
    }
    if (this.status === null || this.buffer.length > 0) return;
    let request: ReadRequest | null;
    while ((request = this.popRequest()) !== null) {
      this.resolveRequest(request, isOk(this.status) ? null : this.status);
    }
  }

  private popRequest(): ReadRequest | null {
    while (this.pendingReads.length > 0) {
      const request = this.pendingReads.shift()!;
      if (!request.active) continue;
      request.active = false;
      return request;
    }
    return null;
  }

  private resolveRequest(request: ReadRequest, value: StatusOr<NodeFragment | null>): void {
    if (request.timer !== null) clearTimeout(request.timer);
    request.deferred.resolve(value);
  }

  private fail(status: Status): void {
    if (this.status === null) this.status = status;
    this.collectAvailable();
    this.completeDone();
  }

  private completeDone(): void {
    if (this.status !== null && !this.done.settled) this.done.resolve(this.status);
  }
}
