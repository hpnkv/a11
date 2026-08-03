import { Deferred } from './concurrency.js';
import { Chunk, ChunkMetadata, NodeFragment, NodeRef, validateName } from './data.js';
import {
  alreadyExistsError,
  dataLossError,
  deadlineExceededError,
  failedPreconditionError,
  invalidArgumentError,
  isOk,
  isStatus,
  notFoundError,
  okStatus,
  resourceExhaustedError,
  statusFromUnknown,
  unimplementedError,
  type Status,
  type StatusOr,
} from './status.js';

const UINT32_MAX = 0xffff_ffff;

/** Absolute JavaScript epoch deadline. `null` waits indefinitely. */
export type Deadline = number | Date | null;

function deadlineMillis(deadline: Deadline | undefined): StatusOr<number | null> {
  try {
    if (deadline === undefined || deadline === null) return null;
    const value = deadline instanceof Date ? deadline.getTime() : deadline;
    return Number.isFinite(value)
      ? value
      : invalidArgumentError('Deadline must be a finite epoch millisecond value, a Date, or null.');
  } catch (error) {
    return invalidArgumentError('Deadline could not be read.', [], error);
  }
}

function validateUint(value: number, field: string, maximum = Number.MAX_SAFE_INTEGER): Status {
  return Number.isSafeInteger(value) && value >= 0 && value <= maximum
    ? okStatus()
    : invalidArgumentError(`${field} must be a non-negative integer no greater than ${maximum}.`);
}

function cloneMetadata(metadata: ChunkMetadata | null): ChunkMetadata | null {
  return metadata === null
    ? null
    : new ChunkMetadata({
        mimetype: metadata.mimetype,
        timestamp: metadata.timestamp,
        attributes: metadata.attributes,
      });
}

export function cloneChunk(chunk: Chunk): Chunk {
  return new Chunk({
    metadata: cloneMetadata(chunk.metadata),
    ref: chunk.ref,
    data: chunk.data,
  });
}

export function cloneFragment(fragment: NodeFragment): NodeFragment {
  const data = fragment.data instanceof Chunk
    ? cloneChunk(fragment.data)
    : new NodeRef({
        id: fragment.data.id,
        offset: fragment.data.offset,
        length: fragment.data.length,
      });
  return new NodeFragment({
    id: fragment.id,
    data,
    seq: fragment.seq,
    continued: fragment.continued,
  });
}

/** Pluggable asynchronous backing store for an A11 node stream. */
export interface ChunkStore {
  get(seq: number, deadline?: Deadline): Promise<StatusOr<NodeFragment>>;
  getByArrivalOrder(arrivalOrder: number, deadline?: Deadline): Promise<StatusOr<NodeFragment>>;
  next(deadline?: Deadline, limit?: number): Promise<StatusOr<Array<NodeFragment | null>>>;
  put(fragment: NodeFragment): Promise<StatusOr<number>>;
  putMany(fragments: readonly NodeFragment[]): Promise<StatusOr<number[]>>;
  clearData(seq: number): Promise<StatusOr<NodeFragment>>;
  getSeqForArrivalOrder(arrivalOrder: number): Promise<StatusOr<number>>;
  getFinalSeq(): Promise<StatusOr<number | null>>;
  closeWritesWithStatus(status: Status, returnStatusIfAlreadyClosed?: boolean): Promise<Status>;
  size(): Promise<StatusOr<number>>;
  getId(): StatusOr<string>;
}

export function hasChunkStoreShape(value: unknown): value is ChunkStore {
  if (typeof value !== 'object' || value === null) return false;
  try {
    const candidate = value as Record<string, unknown>;
    return [
      'get',
      'getByArrivalOrder',
      'next',
      'put',
      'putMany',
      'clearData',
      'getSeqForArrivalOrder',
      'getFinalSeq',
      'closeWritesWithStatus',
      'size',
      'getId',
    ].every((name) => typeof candidate[name] === 'function');
  } catch {
    return false;
  }
}

interface ChangeWaiter {
  deferred: Deferred<void>;
  timer: ReturnType<typeof setTimeout> | null;
}

/** In-memory ChunkStore with out-of-order writes and deadline-aware reads. */
export class LocalChunkStore implements ChunkStore {
  private readonly chunks = new Map<number, Chunk>();
  private readonly seqToArrivalOrder = new Map<number, number>();
  private readonly arrivalOrderToSeq = new Map<number, number>();
  private readonly waiters = new Set<ChangeWaiter>();
  private totalChunksPut = 0;
  private totalChunksRead = 0;
  private finalSeq: number | null = null;
  private terminalStatus: Status | null = null;

  private constructor(private readonly nodeId: string) {}

  static create(nodeId: string): StatusOr<LocalChunkStore> {
    const status = validateName(nodeId);
    return isOk(status) ? new LocalChunkStore(nodeId) : status;
  }

  getId(): StatusOr<string> {
    return this.nodeId;
  }

  private fragmentFor(seq: number): StatusOr<NodeFragment> {
    const chunk = this.chunks.get(seq);
    if (chunk === undefined) {
      if (this.terminalStatus === null) return notFoundError('Fragment is not available yet.');
      if (!isOk(this.terminalStatus)) return this.terminalStatus;
      return notFoundError(`Chunk store closed without seq ${seq}`);
    }
    return new NodeFragment({
      id: this.nodeId,
      data: cloneChunk(chunk),
      seq,
      continued: this.finalSeq === null || seq < this.finalSeq,
    });
  }

  private notifyChange(): void {
    for (const waiter of this.waiters) {
      if (waiter.timer !== null) clearTimeout(waiter.timer);
      waiter.deferred.resolve();
    }
    this.waiters.clear();
  }

  private async waitForChange(deadline: number | null, message: string): Promise<Status> {
    if (deadline !== null && deadline <= Date.now()) return deadlineExceededError(message);
    const waiter: ChangeWaiter = { deferred: new Deferred<void>(), timer: null };
    this.waiters.add(waiter);
    if (deadline !== null) {
      waiter.timer = setTimeout(() => waiter.deferred.resolve(), Math.max(0, deadline - Date.now()));
    }
    try {
      await waiter.deferred.promise;
      if (deadline !== null && Date.now() >= deadline && this.waiters.has(waiter)) {
        return deadlineExceededError(message);
      }
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Chunk store wait failed.');
    } finally {
      this.waiters.delete(waiter);
      if (waiter.timer !== null) clearTimeout(waiter.timer);
    }
  }

  async get(seq: number, deadline?: Deadline): Promise<StatusOr<NodeFragment>> {
    const seqStatus = validateUint(seq, 'seq', UINT32_MAX);
    if (!isOk(seqStatus)) return seqStatus;
    const parsedDeadline = deadlineMillis(deadline);
    if (!isOk(parsedDeadline)) return parsedDeadline;
    try {
      while (true) {
        if (this.chunks.has(seq) || this.terminalStatus !== null) return this.fragmentFor(seq);
        const wait = await this.waitForChange(
          parsedDeadline,
          'Chunk store fragment was not available before the deadline',
        );
        if (!isOk(wait)) return wait;
      }
    } catch (error) {
      return statusFromUnknown(error, 'Chunk store get failed.');
    }
  }

  async getByArrivalOrder(
    arrivalOrder: number,
    deadline?: Deadline,
  ): Promise<StatusOr<NodeFragment>> {
    const orderStatus = validateUint(arrivalOrder, 'arrivalOrder');
    if (!isOk(orderStatus)) return orderStatus;
    const parsedDeadline = deadlineMillis(deadline);
    if (!isOk(parsedDeadline)) return parsedDeadline;
    try {
      while (true) {
        const seq = this.arrivalOrderToSeq.get(arrivalOrder);
        if (seq !== undefined) {
          if (!this.chunks.has(seq)) return dataLossError('Chunk store index references a missing chunk');
          return this.fragmentFor(seq);
        }
        if (this.terminalStatus !== null) {
          return isOk(this.terminalStatus)
            ? notFoundError(`Chunk store closed without arrival order ${arrivalOrder}`)
            : this.terminalStatus;
        }
        const wait = await this.waitForChange(
          parsedDeadline,
          'Chunk store fragment was not available before the deadline',
        );
        if (!isOk(wait)) return wait;
      }
    } catch (error) {
      return statusFromUnknown(error, 'Chunk store arrival-order get failed.');
    }
  }

  async next(
    deadline?: Deadline,
    limit = 1,
  ): Promise<StatusOr<Array<NodeFragment | null>>> {
    const limitStatus = validateUint(limit, 'limit', 0x1_0000_0000);
    if (!isOk(limitStatus)) return limitStatus;
    if (limit === 0) return invalidArgumentError('limit must be positive');
    const parsedDeadline = deadlineMillis(deadline);
    if (!isOk(parsedDeadline)) return parsedDeadline;
    const result: Array<NodeFragment | null> = [];
    try {
      while (true) {
        if (this.totalChunksRead > UINT32_MAX) return [...result, null];
        if (this.finalSeq !== null && this.totalChunksRead > this.finalSeq) {
          if (this.terminalStatus !== null && !isOk(this.terminalStatus) && result.length === 0) {
            return this.terminalStatus;
          }
          return [...result, null];
        }
        const expected = this.totalChunksRead;
        const chunk = this.chunks.get(expected);
        if (chunk !== undefined && result.length < limit) {
          ++this.totalChunksRead;
          result.push(new NodeFragment({
            id: this.nodeId,
            data: cloneChunk(chunk),
            seq: expected,
            continued: this.finalSeq === null || expected < this.finalSeq,
          }));
          continue;
        }
        if (result.length === limit) return result;
        if (this.terminalStatus !== null) {
          if (!isOk(this.terminalStatus) && result.length === 0) return this.terminalStatus;
          return [...result, null];
        }
        const wait = await this.waitForChange(
          parsedDeadline,
          'Expected seq was not available before the deadline',
        );
        if (!isOk(wait)) return result.length > 0 ? result : wait;
      }
    } catch (error) {
      return statusFromUnknown(error, 'Chunk store next failed.');
    }
  }

  async put(fragment: NodeFragment): Promise<StatusOr<number>> {
    const result = await this.putMany([fragment]);
    if (!isOk(result)) return result;
    return result.length === 1
      ? result[0]!
      : dataLossError('PutMany did not return exactly one sequence');
  }

  async putMany(fragments: readonly NodeFragment[]): Promise<StatusOr<number[]>> {
    try {
      if (!Array.isArray(fragments)) return invalidArgumentError('fragments must be an array.');
      let anyExplicit = false;
      let allExplicit = true;
      const explicit = new Set<number>();
      for (const fragment of fragments) {
        if (!(fragment instanceof NodeFragment)) return invalidArgumentError('fragments must contain NodeFragment values.');
        const validation = fragment.validate();
        if (!isOk(validation)) return validation;
        anyExplicit ||= fragment.seq !== null;
        allExplicit &&= fragment.seq !== null;
        if (fragment.seq !== null) {
          if (explicit.has(fragment.seq)) return invalidArgumentError(`Explicit seq ${fragment.seq} occurs more than once`);
          explicit.add(fragment.seq);
        }
        if (!(fragment.data instanceof Chunk)) return unimplementedError('LocalChunkStore supports Chunk payloads, not NodeRef');
      }
      if (anyExplicit !== allExplicit) return invalidArgumentError('Sequence numbers must be set on every fragment or none');
      if (this.terminalStatus !== null) return failedPreconditionError(`Chunk store ${this.nodeId} is closed for writes`);
      if (fragments.length === 0) return [];

      const assigned: number[] = [];
      if (allExplicit) {
        for (const fragment of fragments) assigned.push(fragment.seq!);
      } else {
        let candidate = this.totalChunksPut;
        for (let index = 0; index < fragments.length; ++index) {
          while (candidate <= UINT32_MAX && this.chunks.has(candidate)) ++candidate;
          if (candidate > UINT32_MAX) return resourceExhaustedError('Maximum implicit sequence number exceeded');
          assigned.push(candidate++);
        }
      }
      for (const seq of assigned) {
        if (this.chunks.has(seq)) return alreadyExistsError(`A fragment with seq ${seq} already exists`);
      }
      let batchFinal: number | null = null;
      let sawFinal = false;
      for (let index = 0; index < fragments.length; ++index) {
        if (fragments[index]!.continued) {
          if (sawFinal && !allExplicit) return invalidArgumentError('The final implicit fragment must be last');
          continue;
        }
        if (sawFinal) return invalidArgumentError('More than one fragment in the batch is marked final');
        sawFinal = true;
        batchFinal = assigned[index]!;
      }
      if (batchFinal !== null && this.finalSeq !== null && batchFinal !== this.finalSeq) {
        return failedPreconditionError('The chunk store already has a different final sequence');
      }
      const pendingFinal = batchFinal ?? this.finalSeq;
      if (pendingFinal !== null) {
        if (assigned.some((seq) => seq > pendingFinal)) return invalidArgumentError('A fragment sequence exceeds the final sequence');
        if ([...this.chunks.keys()].some((seq) => seq > pendingFinal)) {
          return invalidArgumentError('An existing fragment exceeds the proposed final sequence');
        }
      }
      for (let index = 0; index < fragments.length; ++index) {
        const seq = assigned[index]!;
        const arrival = this.totalChunksPut + index;
        this.chunks.set(seq, cloneChunk(fragments[index]!.data as Chunk));
        this.arrivalOrderToSeq.set(arrival, seq);
        this.seqToArrivalOrder.set(seq, arrival);
      }
      this.totalChunksPut += fragments.length;
      this.finalSeq = pendingFinal;
      this.notifyChange();
      return assigned;
    } catch (error) {
      return statusFromUnknown(error, 'Chunk store put failed.');
    }
  }

  async clearData(seq: number): Promise<StatusOr<NodeFragment>> {
    const seqStatus = validateUint(seq, 'seq', UINT32_MAX);
    if (!isOk(seqStatus)) return seqStatus;
    try {
      const found = this.chunks.get(seq);
      if (found === undefined) return notFoundError(`No fragment with seq ${seq} exists`);
      const original = cloneChunk(found);
      this.chunks.set(seq, new Chunk({ metadata: cloneMetadata(found.metadata), ref: '__tombstone__' }));
      return new NodeFragment({
        id: this.nodeId,
        data: original,
        seq,
        continued: this.finalSeq === null || seq < this.finalSeq,
      });
    } catch (error) {
      return statusFromUnknown(error, 'Chunk store clear failed.');
    }
  }

  async getSeqForArrivalOrder(arrivalOrder: number): Promise<StatusOr<number>> {
    const status = validateUint(arrivalOrder, 'arrivalOrder');
    if (!isOk(status)) return status;
    const seq = this.arrivalOrderToSeq.get(arrivalOrder);
    return seq === undefined
      ? notFoundError(`No fragment has arrival order ${arrivalOrder}`)
      : seq;
  }

  async getFinalSeq(): Promise<StatusOr<number | null>> {
    return this.finalSeq;
  }

  async closeWritesWithStatus(
    status: Status,
    returnStatusIfAlreadyClosed = false,
  ): Promise<Status> {
    try {
      if (!isStatus(status)) {
        return invalidArgumentError('status must be an A11 Status.');
      }
      if (typeof returnStatusIfAlreadyClosed !== 'boolean') {
        return invalidArgumentError(
          'returnStatusIfAlreadyClosed must be boolean.',
        );
      }
      if (this.terminalStatus !== null) {
        return returnStatusIfAlreadyClosed
          ? this.terminalStatus
          : failedPreconditionError('Chunk store is already closed for writes');
      }
      this.terminalStatus = { ...status, details: status.details ? [...status.details] : undefined };
      this.notifyChange();
      return this.terminalStatus;
    } catch (error) {
      return statusFromUnknown(error, 'Chunk store close failed.');
    }
  }

  async size(): Promise<StatusOr<number>> {
    return this.chunks.size;
  }
}
