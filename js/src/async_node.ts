import { hasChunkStoreShape, LocalChunkStore, type ChunkStore } from './chunk_store.js';
import {
  ChunkStoreReader,
  type ChunkStoreReaderOptions,
} from './chunk_store_reader.js';
import {
  ChunkStoreWriter,
  type ChunkStoreWriterOptions,
  type WritableWireStream,
} from './chunk_store_writer.js';
import { Chunk, NodeFragment, makeNullChunk, validateName } from './data.js';
import {
  SerializationRegistry,
  getGlobalSerializationRegistry,
} from './serialization.js';
import {
  failedPreconditionError,
  internalError,
  invalidArgumentError,
  isOk,
  isStatus,
  okStatus,
  statusFromUnknown,
  unimplementedError,
  type Status,
  type StatusOr,
} from './status.js';

/** Creates the backing log when a {@link NodeMap} first sees a node id. */
export type ChunkStoreFactory = (
  nodeId: string,
) => StatusOr<ChunkStore> | Promise<StatusOr<ChunkStore>>;

/** Collaborators and cursor policy for an {@link AsyncNode}. */
export interface AsyncNodeOptions {
  /** Registry used to turn application objects into typed chunks. */
  serializationRegistry?: SerializationRegistry;
  /** Ordering, offset, and prefetch policy for the consuming half. */
  readerOptions?: ChunkStoreReaderOptions;
  /** Batching and backpressure policy for the producing half. */
  writerOptions?: ChunkStoreWriterOptions;
  /** Owning map, used when the node participates in action/session state. */
  nodeMap?: NodeMap;
}

/**
 * One ordered, typed value sequence flowing through an A11 application.
 *
 * Action inputs and outputs are AsyncNodes. The writer serializes application
 * values into sequenced chunks, persists them through a {@link ChunkStore},
 * and optionally tees stored fragments over a wire stream. The reader
 * follows that same ordered log and deserializes values as they arrive, so an
 * agent can expose tokens, audio frames, tool events, or a unary result through
 * one protocol.
 *
 * End the sequence with {@link finalize}: it marks the logical end of the data
 * and closes storage. Finality and closure remain two distinct facts -- see the
 * AsyncNode lifecycle guide -- and {@link close} is the rarer half on its own,
 * for a producer that cannot say which chunk was last. Failures should use
 * {@link abortWithStatus} so local and remote readers see why the sequence
 * ended.
 */
export class AsyncNode {
  /** Ordered storage shared by the node's reader and writer. */
  readonly chunkStore: ChunkStore;
  /** Owning node map, or `null` for a standalone node. */
  readonly nodeMap: NodeMap | null;
  private registry: SerializationRegistry;
  private readerOptions: ChunkStoreReaderOptions;
  private writerOptions: ChunkStoreWriterOptions;
  private readerInternal: ChunkStoreReader;
  private writerInternal: ChunkStoreWriter;
  private expectedMimetypePatterns: string | readonly string[] = '';
  private expectedTag: string | undefined;

  private constructor(
    store: ChunkStore,
    reader: ChunkStoreReader,
    writer: ChunkStoreWriter,
    options: AsyncNodeOptions,
  ) {
    this.chunkStore = store;
    this.nodeMap = options.nodeMap ?? null;
    this.registry = options.serializationRegistry ?? getGlobalSerializationRegistry();
    this.readerOptions = { ...(options.readerOptions ?? {}) };
    this.writerOptions = { ...(options.writerOptions ?? {}) };
    this.readerInternal = reader;
    this.writerInternal = writer;
  }

  /** Build a node over an existing store, preserving its data and id. */
  static fromStore(
    store: ChunkStore,
    options: AsyncNodeOptions = {},
  ): StatusOr<AsyncNode> {
    try {
      if (!hasChunkStoreShape(store)) {
        return invalidArgumentError('store must implement ChunkStore.');
      }
      if (!(options.serializationRegistry ?? getGlobalSerializationRegistry() instanceof SerializationRegistry)) {
        return invalidArgumentError('serializationRegistry must be a SerializationRegistry.');
      }
      const reader = ChunkStoreReader.create(store, options.readerOptions);
      if (!isOk(reader)) return reader;
      const writer = ChunkStoreWriter.create(store, options.writerOptions);
      if (!isOk(writer)) {
        reader.cancel();
        return writer;
      }
      return new AsyncNode(store, reader, writer, options);
    } catch (error) {
      return statusFromUnknown(error, 'AsyncNode could not be created.');
    }
  }

  /** Create a backing store for `nodeId`, then build its reader and writer. */
  static async create(
    nodeId: string,
    options: AsyncNodeOptions & { chunkStoreFactory?: ChunkStoreFactory } = {},
  ): Promise<StatusOr<AsyncNode>> {
    const validation = validateName(nodeId);
    if (!isOk(validation)) return validation;
    const factory = options.chunkStoreFactory ?? ((id: string) => LocalChunkStore.create(id));
    try {
      const store = await factory(nodeId);
      if (!isOk(store)) return store;
      return AsyncNode.fromStore(store, options);
    } catch (error) {
      return statusFromUnknown(error, 'Chunk-store factory raised an exception.');
    }
  }

  /** Return the stable id used by fragments, actions, and sessions. */
  getId(): StatusOr<string> {
    try {
      const id = this.chunkStore.getId();
      if (isStatus(id) && !isOk(id)) return id;
      return typeof id === 'string'
        ? id
        : internalError('ChunkStore.getId() returned an invalid value.');
    } catch (error) {
      return statusFromUnknown(error, 'ChunkStore.getId() raised an exception.');
    }
  }
  /** Low-level consuming cursor; prefer `next` for typed values. */
  get reader(): ChunkStoreReader { return this.readerInternal; }
  /** Low-level producing cursor; prefer `put` for typed values. */
  get writer(): ChunkStoreWriter { return this.writerInternal; }
  /** Codec registry currently used at the application boundary. */
  get serializationRegistry(): SerializationRegistry { return this.registry; }

  /** Replace the codecs used by subsequent typed reads and writes. */
  setSerializationRegistry(registry: SerializationRegistry): Status {
    if (!(registry instanceof SerializationRegistry)) return invalidArgumentError('registry must be a SerializationRegistry.');
    this.registry = registry;
    return okStatus();
  }

  /** Set default MIME/type constraints for subsequent typed reads. */
  setExpectedTypes(
    mimetypePatterns: string | readonly string[] = '',
    expectedTag?: string,
  ): Status {
    try {
      if (typeof mimetypePatterns !== 'string' && !Array.isArray(mimetypePatterns)) {
        return invalidArgumentError('mimetypePatterns must be a string or string array.');
      }
      if (Array.isArray(mimetypePatterns) && mimetypePatterns.some((item) => typeof item !== 'string')) {
        return invalidArgumentError('Every mimetype pattern must be a string.');
      }
      if (expectedTag !== undefined && (typeof expectedTag !== 'string' || expectedTag === '')) {
        return invalidArgumentError('expectedTag must be a non-empty string or omitted.');
      }
      this.expectedMimetypePatterns = mimetypePatterns;
      this.expectedTag = expectedTag;
      return okStatus();
    } catch (error) {
      return invalidArgumentError('Expected type options could not be read.', [], error);
    }
  }

  getReaderOptions(): ChunkStoreReaderOptions { return { ...this.readerOptions }; }
  getWriterOptions(): ChunkStoreWriterOptions { return { ...this.writerOptions }; }

  /** Replace and rewind the independent read cursor, optionally from an offset. */
  resetReader(options?: ChunkStoreReaderOptions): Status {
    try {
      const nextOptions = options ?? this.readerOptions;
      const reader = ChunkStoreReader.create(this.chunkStore, nextOptions);
      if (!isOk(reader)) return reader;
      this.readerInternal.cancel();
      this.readerInternal = reader;
      this.readerOptions = { ...reader.options };
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Resetting AsyncNode reader raised an exception.');
    }
  }

  setReaderOptions(options: ChunkStoreReaderOptions): Status {
    return this.resetReader(options);
  }

  /** Replace writer policy before any write has started. */
  setWriterOptions(options: ChunkStoreWriterOptions): Status {
    try {
      if (this.writerInternal.queueSize !== 0 || !this.writerInternal.isWritable()) {
        return failedPreconditionError('Writer options can only be changed before writing starts.');
      }
      const writer = ChunkStoreWriter.create(this.chunkStore, options);
      if (!isOk(writer)) return writer;
      void this.writerInternal.cancel();
      this.writerInternal = writer;
      this.writerOptions = { ...writer.options };
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Changing AsyncNode writer options raised an exception.');
    }
  }

  getReaderStatus(): Status { return this.readerInternal.getStatus(); }
  getWriterStatus(): Status { return this.writerInternal.getStatus() ?? okStatus(); }
  getWriterAbortStatus(): Status | null { return this.writerInternal.getAbortStatus(); }
  async isWritable(): Promise<StatusOr<boolean>> { return this.writerInternal.isWritable(); }

  /** Persist a raw chunk, optionally at an explicit sequence and/or as final. */
  putChunk(chunk: Chunk, seq: number | null = null, final = false): Promise<StatusOr<number>> {
    return this.writerInternal.putChunk(chunk, seq, final);
  }

  /** Persist a fragment carrying its own sequence and continuation marker. */
  putFragment(fragment: NodeFragment): Promise<StatusOr<number>> {
    if (!(fragment instanceof NodeFragment)) return Promise.resolve(invalidArgumentError('fragment must be a NodeFragment.'));
    if (!(fragment.data instanceof Chunk)) return Promise.resolve(unimplementedError('AsyncNode writers do not resolve NodeRef payloads.'));
    return this.putChunk(fragment.data, fragment.seq, !fragment.continued);
  }

  /** Serialize and persist one application value, respecting writer backpressure. */
  async put(
    value: unknown,
    options: { seq?: number | null; final?: boolean; mimetype?: string } = {},
  ): Promise<StatusOr<number>> {
    try {
      if (typeof options !== 'object' || options === null) {
        return invalidArgumentError('AsyncNode put options must be an object.');
      }
      const seq = options.seq ?? null;
      const final = options.final ?? false;
      const mimetype = options.mimetype ?? '';
      if (typeof final !== 'boolean' || typeof mimetype !== 'string') {
        return invalidArgumentError('final must be boolean and mimetype must be a string.');
      }
      if (value instanceof NodeFragment) {
        if (seq !== null || final || mimetype !== '') return invalidArgumentError('seq, final, and mimetype are carried by a NodeFragment and cannot be supplied separately.');
        return this.putFragment(value);
      }
      if (value instanceof Chunk) {
        if (mimetype !== '') return invalidArgumentError('mimetype cannot be supplied with a raw Chunk.');
        return this.putChunk(value, seq, final);
      }
      const chunk = await this.registry.toChunk(value, mimetype);
      if (!isOk(chunk)) return chunk;
      return this.putChunk(chunk, seq, final);
    } catch (error) {
      return statusFromUnknown(error, 'Writing AsyncNode value raised an exception.');
    }
  }

  /**
   * End the sequence: mark the logical end of the data, and close the writer.
   *
   * The one call an ordinary producer needs. `value` is written as the final
   * fragment; omitting it (or passing `null`/`undefined`) writes a null
   * terminator instead, which is the form to use once the last visible value
   * has already gone out with {@link put}. Unless `close` is `false` the writer
   * is closed too, so readers waiting for data that can no longer arrive are
   * released and a peer's mirror of the node closes as well.
   *
   * It does not wait: the write and the close are carried out by the writer's
   * own pump, so a producer can finalise and move on. Nothing is swallowed -- a
   * failed write or close is reported through {@link getWriterStatus}. Pass
   * `wait: true` to resolve only once the store has confirmed both.
   */
  async finalize(
    value?: unknown,
    options: { seq?: number | null; mimetype?: string; wait?: boolean; close?: boolean } = {},
  ): Promise<Status> {
    try {
      if (typeof options !== 'object' || options === null) {
        return invalidArgumentError('AsyncNode finalize options must be an object.');
      }
      const close = options.close ?? true;
      const wait = options.wait ?? false;
      let seq = options.seq ?? null;
      let chunk: Chunk;
      if (value === undefined || value === null) {
        if (options.mimetype) return invalidArgumentError('mimetype cannot be supplied without a value.');
        chunk = makeNullChunk();
      } else if (value instanceof NodeFragment) {
        if (seq !== null || options.mimetype) {
          return invalidArgumentError('seq and mimetype are carried by a NodeFragment and cannot be supplied separately.');
        }
        if (!(value.data instanceof Chunk)) return unimplementedError('AsyncNode writers do not resolve NodeRef payloads.');
        chunk = value.data;
        seq = value.seq;
      } else if (value instanceof Chunk) {
        if (options.mimetype) return invalidArgumentError('mimetype cannot be supplied with a raw Chunk.');
        chunk = value;
      } else {
        const encoded = await this.registry.toChunk(value, options.mimetype ?? '');
        if (!isOk(encoded)) return encoded;
        chunk = encoded;
      }
      return this.writerInternal.finalize(chunk, seq, wait, close);
    } catch (error) {
      return statusFromUnknown(error, 'Finalizing an AsyncNode raised an exception.');
    }
  }

  /** Read the next raw fragment, or `null` at the clean end of sequence. */
  nextFragment(timeoutMs?: number): Promise<StatusOr<NodeFragment | null>> {
    return this.readerInternal.next(timeoutMs);
  }

  /** Read the next inline chunk without deserializing its payload. */
  async nextChunk(timeoutMs?: number): Promise<StatusOr<Chunk | null>> {
    const fragment = await this.nextFragment(timeoutMs);
    if (!isOk(fragment) || fragment === null) return fragment;
    return fragment.getChunk();
  }

  /**
   * The next fragment carrying a value, or `null` once the node ends.
   *
   * A null chunk is a marker, not a value: a final one says the node is
   * finished, and a non-final one says nothing at all. Neither is something a
   * reader asked for, so both are skipped here rather than surfaced as a value
   * or rejected — which is what lets a node be closed with nothing in it.
   */
  private async nextValueFragment(
    timeoutMs?: number,
  ): Promise<StatusOr<NodeFragment | null>> {
    const started = Date.now();
    for (;;) {
      const remaining = timeoutMs === undefined
        ? undefined
        : Math.max(0, timeoutMs - (Date.now() - started));
      const fragment = await this.nextFragment(remaining);
      if (!isOk(fragment) || fragment === null) return fragment;
      const chunk = fragment.getChunk();
      if (!isOk(chunk)) return chunk;
      if (!chunk.isNull) return fragment;
      if (!fragment.continued) return null;
    }
  }

  /** Read one value, or `null` at finality, clean closure, or the reader limit. */
  async next<T = unknown>(
    optionsOrTimeout: {
      timeoutMs?: number;
      mimetypePatterns?: string | readonly string[];
      expectedTag?: string;
    } | number = {},
  ): Promise<StatusOr<T | null>> {
    try {
      const options = typeof optionsOrTimeout === 'number'
        ? { timeoutMs: optionsOrTimeout }
        : optionsOrTimeout;
      if (typeof options !== 'object' || options === null) {
        return invalidArgumentError('AsyncNode next options must be an object or timeout in milliseconds.');
      }
      const fragment = await this.nextValueFragment(options.timeoutMs);
      if (!isOk(fragment) || fragment === null) return fragment;
      const chunk = fragment.getChunk();
      if (!isOk(chunk)) return chunk;
      return this.registry.fromChunk<T>(
        chunk,
        options.mimetypePatterns ?? this.expectedMimetypePatterns,
        options.expectedTag ?? this.expectedTag,
      );
    } catch (error) {
      return statusFromUnknown(error, 'Reading AsyncNode value raised an exception.');
    }
  }

  /**
   * Consume exactly one whole value's fragment and validate its terminator.
   * Use this for unary action ports; streaming ports should call `next`.
   */
  async consumeFragment(
    options: { timeoutMs?: number; allowNone?: boolean } = {},
  ): Promise<StatusOr<NodeFragment | null>> {
    try {
      return await this.consumeFragmentInternal(options);
    } catch (error) {
      return statusFromUnknown(error, 'Consuming AsyncNode fragment raised an exception.');
    }
  }

  private async consumeFragmentInternal(
    options: { timeoutMs?: number; allowNone?: boolean },
  ): Promise<StatusOr<NodeFragment | null>> {
    if (typeof options !== 'object' || options === null) {
      return invalidArgumentError('AsyncNode consume options must be an object.');
    }
    if (options.allowNone !== undefined && typeof options.allowNone !== 'boolean') {
      return invalidArgumentError('allowNone must be boolean or omitted.');
    }
    if ((this.readerOptions.ordered ?? true) !== true) return failedPreconditionError('consume() requires an ordered reader.');
    const started = Date.now();
    const first = await this.nextValueFragment(options.timeoutMs);
    if (!isOk(first)) return first;
    if (first === null) return options.allowNone
      ? null
      : failedPreconditionError('AsyncNode is empty at the current reader offset.');
    if (!first.continued) return first;
    const remaining = options.timeoutMs === undefined
      ? undefined
      : Math.max(0, options.timeoutMs - (Date.now() - started));
    const terminator = await this.nextFragment(remaining);
    if (!isOk(terminator)) return terminator;
    if (terminator === null) return failedPreconditionError('A continued consumed value must be followed by a null final chunk.');
    const terminatorChunk = terminator.getChunk();
    if (!isOk(terminatorChunk)) return terminatorChunk;
    if (terminator.continued || !terminatorChunk.isNull) {
      return failedPreconditionError('The only fragment allowed after a consumed value is a null final chunk.');
    }
    return first;
  }

  async consumeChunk(
    options: { timeoutMs?: number; allowNone?: boolean } = {},
  ): Promise<StatusOr<Chunk | null>> {
    const fragment = await this.consumeFragment(options);
    if (!isOk(fragment) || fragment === null) return fragment;
    return fragment.getChunk();
  }

  /** Consume and deserialize exactly one whole unary value. */
  async consume<T = unknown>(
    options: {
      timeoutMs?: number;
      allowNone?: boolean;
      mimetypePatterns?: string | readonly string[];
      expectedTag?: string;
      raw?: 'fragment' | 'chunk';
    } = {},
  ): Promise<StatusOr<T | NodeFragment | Chunk | null>> {
    try {
      const fragment = await this.consumeFragment(options);
      if (!isOk(fragment) || fragment === null) return fragment;
      if (
        options.raw !== undefined &&
        options.raw !== 'fragment' &&
        options.raw !== 'chunk'
      ) {
        return invalidArgumentError("raw must be 'fragment', 'chunk', or omitted.");
      }
      if (options.raw === 'fragment') return fragment;
      const chunk = fragment.getChunk();
      if (!isOk(chunk)) return chunk;
      if (options.raw === 'chunk') return chunk;
      return this.registry.fromChunk<T>(
        chunk,
        options.mimetypePatterns ?? this.expectedMimetypePatterns,
        options.expectedTag ?? this.expectedTag,
      );
    } catch (error) {
      return statusFromUnknown(error, 'Consuming AsyncNode value raised an exception.');
    }
  }

  /** Iterate to the clean reader end, yielding at most one terminal error. */
  async *values<T = unknown>(options: {
    timeoutMs?: number;
    mimetypePatterns?: string | readonly string[];
    expectedTag?: string;
  } = {}): AsyncGenerator<StatusOr<T>, void, void> {
    while (true) {
      const value = await this.next<T>(options);
      if (!isOk(value)) { yield value; return; }
      if (value === null) return;
      yield value;
    }
  }

  [Symbol.asyncIterator](): AsyncGenerator<StatusOr<unknown>, void, void> { return this.values(); }

  /** Await outstanding writes without closing or adding a final marker. */
  waitForBufferToDrain(): Promise<Status> { return this.writerInternal.waitForBufferToDrain(); }
  /**
   * Flush queued writes and close the writer, marking nothing final.
   *
   * The specialised half of {@link finalize}: closure without finality, for a
   * producer that cannot say which chunk was the last one -- a log, say -- but
   * can say that no more are coming. Closing always drains.
   */
  close(): Promise<Status> { return this.writerInternal.drainAndClose(); }
  /** Fail the producing half so readers observe a structured terminal error. */
  abortWithStatus(status: Status): Promise<Status> { return this.writerInternal.abortWithStatus(status); }
  /** Tee stored writes to a transport; `send` admission is not peer delivery. */
  attachStream(stream: WritableWireStream): Status { return this.writerInternal.attachStream(stream); }
  /** Stop mirroring writes to one transport. */
  detachStream(stream: WritableWireStream): Status { return this.writerInternal.detachStream(stream); }
  /** Stop this node's independent consuming cursor. */
  cancelReader(): Status { return this.readerInternal.cancel(); }
  /** Abandon queued writes and stop the producing cursor. */
  cancelWriter(): Promise<Status> { return this.writerInternal.cancel(); }
  /** Cancel both halves of this local node object. */
  async cancel(): Promise<Status> {
    this.readerInternal.cancel();
    return this.writerInternal.cancel();
  }
}

/**
 * Registry of lazily created {@link AsyncNode}s keyed by stable node id.
 *
 * Sessions share a NodeMap so fragments arriving before or after an action
 * port is opened converge on the same store and node object. The factory is
 * the persistence extension point: use an in-memory store locally or inject a
 * distributed store when several agent processes must share stream state.
 */
export class NodeMap {
  private readonly nodes = new Map<string, AsyncNode>();

  constructor(private readonly factory: ChunkStoreFactory = (id) => LocalChunkStore.create(id)) {}

  /** Return the canonical node, creating its store on first access. */
  async get(nodeId: string): Promise<StatusOr<AsyncNode>> {
    const validation = validateName(nodeId);
    if (!isOk(validation)) return validation;
    const existing = this.nodes.get(nodeId);
    if (existing !== undefined) return existing;
    try {
      const store = await this.factory(nodeId);
      if (!isOk(store)) return store;
      const node = AsyncNode.fromStore(store, { nodeMap: this });
      if (!isOk(node)) return node;
      const raced = this.nodes.get(nodeId);
      if (raced !== undefined) return raced;
      this.nodes.set(nodeId, node);
      return node;
    } catch (error) {
      return statusFromUnknown(error, 'Chunk-store factory raised an exception.');
    }
  }

  /** Return an existing node without invoking the store factory. */
  getIfExists(nodeId: string): StatusOr<AsyncNode | null> {
    const validation = validateName(nodeId);
    return isOk(validation) ? this.nodes.get(nodeId) ?? null : validation;
  }

  /** Remove a node, optionally only if it is the expected instance. */
  discard(nodeId: string, expected?: AsyncNode): StatusOr<AsyncNode | null> {
    const validation = validateName(nodeId);
    if (!isOk(validation)) return validation;
    const found = this.nodes.get(nodeId);
    if (found === undefined || (expected !== undefined && found !== expected)) return null;
    this.nodes.delete(nodeId);
    return found;
  }

  contains(nodeId: string): boolean { return this.nodes.has(nodeId); }
  get size(): number { return this.nodes.size; }
  entries(): IterableIterator<[string, AsyncNode]> { return this.nodes.entries(); }
}
