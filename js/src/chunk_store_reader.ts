/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import { Deferred, storeCallbackScheduler } from './concurrency.js';
import { hasChunkStoreShape, type ChunkStore } from './chunk_store.js';
import { Chunk, ChunkMetadata, NodeFragment, NodeRef } from './data.js';
import {
  abortedError,
  dataLossError,
  deadlineExceededError,
  failedPreconditionError,
  invalidArgumentError,
  isDeadlineExceededError,
  isNotFoundError,
  isOk,
  isStatus,
  okStatus,
  outOfRangeError,
  resourceExhaustedError,
  statusFromUnknown,
  unimplementedError,
  type Status,
  type StatusOr,
} from './status.js';

const UINT32_MAX = 0xffff_ffff;
const UINT32_RANGE = 0x1_0000_0000;

/** Select how a reader follows and retains a {@link ChunkStore} sequence. */
export interface ChunkStoreReaderOptions {
  /** Read logical sequence order; false follows ingestion/arrival order. */
  ordered?: boolean;
  /** Clear each payload after reading while retaining its store tombstone. */
  popChunks?: boolean;
  /** Number of fragments to prefetch beyond active callers. */
  numChunksToBuffer?: number;
  /** Sequence or arrival position at which this cursor begins. */
  offset?: number;
  /** Stop after this many fragments, or continue to final when `null`. */
  maxChunksToRead?: number | null;
  /** Expand omitted MIME types from the preceding contiguous chunk. */
  stickyMimetype?: boolean;
  followNodeRefs?: boolean;
  maxRefDepth?: number;
  maxRefBreadth?: number;
}

interface NormalizedReaderOptions {
  ordered: boolean;
  popChunks: boolean;
  numChunksToBuffer: number;
  offset: number;
  maxChunksToRead: number | null;
  stickyMimetype: boolean;
  followNodeRefs: boolean;
  maxRefDepth: number;
  maxRefBreadth: number;
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
      followNodeRefs: options.followNodeRefs ?? true,
      maxRefDepth: options.maxRefDepth ?? 8,
      maxRefBreadth: options.maxRefBreadth ?? 2,
    };
    if (
      typeof result.ordered !== 'boolean' ||
      typeof result.popChunks !== 'boolean' ||
      typeof result.stickyMimetype !== 'boolean' ||
      typeof result.followNodeRefs !== 'boolean'
    ) {
      return invalidArgumentError('Reader ordered, popChunks, and stickyMimetype options must be boolean.');
    }
    if (result.followNodeRefs && (!result.ordered || result.popChunks))
      return invalidArgumentError('Following references requires ordered, non-destructive reads.');
    if (![result.maxRefDepth, result.maxRefBreadth].every(value => Number.isInteger(value) && value >= 1 && value <= 64))
      return outOfRangeError('Reference depth and breadth must be 1..64.');
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

export interface ReferenceTarget {
  store: ChunkStore;
  resolver: ReferenceResolver;
}

export type ReferenceResolver = (reference: NodeRef) => Promise<StatusOr<ReferenceTarget>>;

interface LogicalFrame {
  store: ChunkStore;
  reader: ChunkStoreReader;
  resolver: ReferenceResolver;
  remaining: number | null;
}

interface PendingReference {
  frame: LogicalFrame;
  reference: NodeRef;
  target: Promise<StatusOr<ReferenceTarget>>;
}

/**
 * Fair, stackless, prefetching cursor over a {@link ChunkStore}.
 *
 * A reader turns the store's waitable log operations into a simple `next()` or
 * async-iterator interface. Ordered mode stops after the final fragment, a
 * clean store closure, or its configured read limit; arrival mode is useful
 * when inspecting interleaved network ingestion. The bounded prefetch buffer
 * lets consumers overlap storage latency without allocating a task or fiber
 * per node.
 *
 * **One fetch is in flight at a time, unlike native.** `ChunkStoreReader` in
 * C++ keeps 16 behind a reorder buffer, which overlaps the latency of a store
 * that answers slowly. Every store in this library is
 * {@link LocalChunkStore} -- in memory, resolving on the microtask queue -- so
 * there is no latency to overlap, and a reorder buffer would add ordering and
 * end-of-sequence cases for a gain that cannot appear. Revisit this if a store
 * with real per-read latency arrives, such as one backed by a fetch.
 */
export class ChunkStoreReader {
  /** Ordered storage followed by this cursor. */
  readonly store: ChunkStore;
  /** Validated immutable cursor options. */
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
  private logicalFrames: LogicalFrame[] | null = null;
  private logicalStatus: Status = okStatus();
  private logicalEnded = false;
  private logicalEmitted = 0;
  private logicalMimetype = '';
  private logicalRootFinal = false;
  private logicalGate: Promise<void> = Promise.resolve();
  private pendingReference: PendingReference | null = null;
  private readonly logicalCancelled = new Deferred<void>();

  private constructor(store: ChunkStore, options: NormalizedReaderOptions) {
    this.store = store;
    this.options = Object.freeze({ ...options });
    this.position = options.offset;
  }

  /** Validate options, create the cursor, and start its lazy fetch pump. */
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

  /** Number of prefetched fragments currently ready for callers. */
  get bufferSize(): number { return this.buffer.length; }

  /** Reader terminal status, or OK while it is healthy and still running. */
  getStatus(): Status { return isOk(this.logicalStatus) ? this.status ?? okStatus() : this.logicalStatus; }

  logicalSawFinal(): boolean { return this.logicalRootFinal; }

  /** Schedule fetching before the first `next()`; safe to call repeatedly. */
  ensureStarted(): Status { return this.wake(); }

  /** Stop fetching and release pending readers with cancellation. */
  cancel(): Status {
    for (const frame of this.logicalFrames ?? []) frame.reader.cancel();
    this.logicalFrames = null;
    this.pendingReference = null;
    this.logicalCancelled.resolve();
    this.logicalStatus = abortedError('ChunkStoreReader was cancelled');
    if (this.status === null) this.status = abortedError('ChunkStoreReader was cancelled');
    this.collectAvailable();
    this.completeDone();
    return okStatus();
  }

  /** Await end-of-sequence, cancellation, or a store error. */
  wait(): Promise<Status> { return this.done.promise; }

  private async awaitReference(
    target: Promise<StatusOr<ReferenceTarget>>,
    deadline: number | null,
  ): Promise<StatusOr<ReferenceTarget>> {
    let timer: ReturnType<typeof setTimeout> | null = null;
    const cancelled = this.logicalCancelled.promise.then(
      () => abortedError('ChunkStoreReader was cancelled'),
    );
    const candidates: Promise<StatusOr<ReferenceTarget>>[] = [target, cancelled];
    if (deadline !== null) {
      candidates.push(new Promise(resolve => {
        timer = setTimeout(
          () => resolve(deadlineExceededError('Reference resolution timed out')),
          Math.max(0, deadline - performance.now()),
        );
      }));
    }
    try {
      return await Promise.race(candidates);
    } finally {
      if (timer !== null) clearTimeout(timer);
    }
  }

  /** Read the next leaf fragment, expanding typed references in source order. */
  async nextLogical(
    resolver: ReferenceResolver,
    timeoutMs?: number,
  ): Promise<StatusOr<NodeFragment | null>> {
    if (timeoutMs !== undefined && (!Number.isFinite(timeoutMs) || timeoutMs < 0))
      return invalidArgumentError('timeoutMs must be non-negative or omitted.');
    let release!: () => void;
    const previous = this.logicalGate;
    this.logicalGate = new Promise<void>(resolve => { release = resolve; });
    await previous;
    try {
      if (!isOk(this.logicalStatus)) return this.logicalStatus;
      if (this.logicalEnded) return null;
      if (this.logicalFrames === null) {
        const raw = ChunkStoreReader.create(this.store, {
          ...this.options, maxChunksToRead: null, stickyMimetype: false,
        });
        if (!isOk(raw)) return raw;
        this.logicalFrames = [{store: this.store, reader: raw, resolver, remaining: null}];
      }
      if (this.options.maxChunksToRead === 0) { this.logicalEnded = true; return null; }
      const deadline = timeoutMs === undefined ? null : performance.now() + timeoutMs;
      let steps = 0;
      while (this.logicalFrames.length) {
        if (++steps % 128 === 0) await Promise.resolve();
        const frame = this.logicalFrames.at(-1)!;
        if (frame.remaining === 0) { frame.reader.cancel(); this.logicalFrames.pop(); continue; }
        if (this.pendingReference?.frame === frame) {
          const pending = this.pendingReference;
          const target = await this.awaitReference(pending.target, deadline);
          if (!isOk(target)) {
            if (!isDeadlineExceededError(target)) this.logicalStatus = target;
            return target;
          }
          if (this.logicalFrames === null) return abortedError('ChunkStoreReader was cancelled');
          this.pendingReference = null;
          if (this.logicalFrames.some(item => item.store === target.store)) {
            this.logicalStatus = failedPreconditionError('Reference cycle detected.');
            return this.logicalStatus;
          }
          const child = ChunkStoreReader.create(target.store, {
            ...this.options, offset: pending.reference.offset, maxChunksToRead: null,
            stickyMimetype: false,
            numChunksToBuffer: 0,
          });
          if (!isOk(child)) { this.logicalStatus = child; return child; }
          this.logicalFrames.push({
            store: target.store, reader: child, resolver: target.resolver,
            remaining: pending.reference.length,
          });
          continue;
        }
        const remainingMs = deadline === null ? undefined : Math.max(0, deadline - performance.now());
        const fragment = await frame.reader.next(remainingMs);
        if (!isOk(fragment)) {
          if (!isDeadlineExceededError(fragment)) this.logicalStatus = fragment;
          return fragment;
        }
        if (fragment === null) {
          if (frame.remaining !== null) {
            this.logicalStatus = dataLossError('Referenced node ended before its fragment window.');
            return this.logicalStatus;
          }
          frame.reader.cancel(); this.logicalFrames.pop(); continue;
        }
        if (frame.remaining !== null) --frame.remaining;
        if (this.logicalFrames.length === 1 && !fragment.continued) this.logicalRootFinal = true;
        if (fragment.data instanceof NodeRef) {
          if (!this.options.followNodeRefs) {
            this.logicalStatus = unimplementedError('Reference following is disabled.');
            return this.logicalStatus;
          }
          if (fragment.data.length === 0) continue;
          if (this.logicalFrames.length > this.options.maxRefDepth) {
            this.logicalStatus = resourceExhaustedError('Reference depth exceeded.');
            return this.logicalStatus;
          }
          const reference = fragment.data;
          this.pendingReference = {
            frame,
            reference,
            target: Promise.resolve().then(() => frame.resolver(reference)).catch(
              error => statusFromUnknown(error, 'Reference resolution failed.'),
            ),
          };
          continue;
        }
        const chunk = fragment.data;
        if (chunk.isNull) {
          if (!fragment.continued && this.logicalFrames.length === 1) {
            this.logicalEnded = true;
            return null;
          }
          continue;
        }
        if (this.options.stickyMimetype) {
          if (chunk.mimetype) this.logicalMimetype = chunk.mimetype;
          else if (this.logicalMimetype) {
            chunk.metadata ??= new ChunkMetadata({mimetype: this.logicalMimetype});
            chunk.metadata.mimetype = this.logicalMimetype;
          }
        }
        if (++this.logicalEmitted >= (this.options.maxChunksToRead ?? Infinity)) this.logicalEnded = true;
        return fragment;
      }
      this.logicalEnded = true;
      return null;
    } finally { release(); }
  }

  /** Await the next fragment; `null` is a clean logical end of the sequence. */
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
    // Wake the pump instead of running it on the caller's turn. In JavaScript
    // the scheduler hop is one microtask, and this avoids re-entering
    // `drive()`. See bench/FINDINGS.md, "Reader prefetch".
    this.wake();
    return request.deferred.promise;
  }

  /**
   * Iterate to the clean reader end, yielding one terminal error when needed.
   */
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

  /** Iterate fragments until finality, closure, or failure. */
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
