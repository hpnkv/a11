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

export type ChunkStoreFactory = (
  nodeId: string,
) => StatusOr<ChunkStore> | Promise<StatusOr<ChunkStore>>;

export interface AsyncNodeOptions {
  serializationRegistry?: SerializationRegistry;
  readerOptions?: ChunkStoreReaderOptions;
  writerOptions?: ChunkStoreWriterOptions;
  nodeMap?: NodeMap;
}

/** A single ordered, typed, asynchronously streamed A11 value sequence. */
export class AsyncNode {
  readonly chunkStore: ChunkStore;
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
  get reader(): ChunkStoreReader { return this.readerInternal; }
  get writer(): ChunkStoreWriter { return this.writerInternal; }
  get serializationRegistry(): SerializationRegistry { return this.registry; }

  setSerializationRegistry(registry: SerializationRegistry): Status {
    if (!(registry instanceof SerializationRegistry)) return invalidArgumentError('registry must be a SerializationRegistry.');
    this.registry = registry;
    return okStatus();
  }

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

  putChunk(chunk: Chunk, seq: number | null = null, final = false): Promise<StatusOr<number>> {
    return this.writerInternal.putChunk(chunk, seq, final);
  }

  putFragment(fragment: NodeFragment): Promise<StatusOr<number>> {
    if (!(fragment instanceof NodeFragment)) return Promise.resolve(invalidArgumentError('fragment must be a NodeFragment.'));
    if (!(fragment.data instanceof Chunk)) return Promise.resolve(unimplementedError('AsyncNode writers do not resolve NodeRef payloads.'));
    return this.putChunk(fragment.data, fragment.seq, !fragment.continued);
  }

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

  putFinal(value: unknown, seq: number | null = null, mimetype = ''): Promise<StatusOr<number>> {
    return this.put(value, { seq, final: true, mimetype });
  }

  putNullFinal(seq: number | null = null): Promise<StatusOr<number>> {
    return this.putChunk(makeNullChunk(), seq, true);
  }

  nextFragment(timeoutMs?: number): Promise<StatusOr<NodeFragment | null>> {
    return this.readerInternal.next(timeoutMs);
  }

  async nextChunk(timeoutMs?: number): Promise<StatusOr<Chunk | null>> {
    const fragment = await this.nextFragment(timeoutMs);
    if (!isOk(fragment) || fragment === null) return fragment;
    return fragment.getChunk();
  }

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
      const fragment = await this.nextFragment(options.timeoutMs);
      if (!isOk(fragment) || fragment === null) return fragment;
      const chunk = fragment.getChunk();
      if (!isOk(chunk)) return chunk;
      if (chunk.isNull) {
        return fragment.continued
          ? failedPreconditionError('A null stream marker must be final.')
          : null;
      }
      return this.registry.fromChunk<T>(
        chunk,
        options.mimetypePatterns ?? this.expectedMimetypePatterns,
        options.expectedTag ?? this.expectedTag,
      );
    } catch (error) {
      return statusFromUnknown(error, 'Reading AsyncNode value raised an exception.');
    }
  }

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
    const first = await this.nextFragment(options.timeoutMs);
    if (!isOk(first)) return first;
    if (first === null) return options.allowNone
      ? null
      : failedPreconditionError('AsyncNode is empty at the current reader offset.');
    const chunk = first.getChunk();
    if (!isOk(chunk)) return chunk;
    if (chunk.isNull) return failedPreconditionError('AsyncNode cannot consume a null chunk as its value.');
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

  waitForBufferToDrain(): Promise<Status> { return this.writerInternal.waitForBufferToDrain(); }
  drainAndClose(): Promise<Status> { return this.writerInternal.drainAndClose(); }
  abortWithStatus(status: Status): Promise<Status> { return this.writerInternal.abortWithStatus(status); }
  attachStream(stream: WritableWireStream): Status { return this.writerInternal.attachStream(stream); }
  detachStream(stream: WritableWireStream): Status { return this.writerInternal.detachStream(stream); }
  cancelReader(): Status { return this.readerInternal.cancel(); }
  cancelWriter(): Promise<Status> { return this.writerInternal.cancel(); }
  async cancel(): Promise<Status> {
    this.readerInternal.cancel();
    return this.writerInternal.cancel();
  }
}

/** Registry of lazily-created AsyncNodes, keyed by validated node id. */
export class NodeMap {
  private readonly nodes = new Map<string, AsyncNode>();

  constructor(private readonly factory: ChunkStoreFactory = (id) => LocalChunkStore.create(id)) {}

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

  getIfExists(nodeId: string): StatusOr<AsyncNode | null> {
    const validation = validateName(nodeId);
    return isOk(validation) ? this.nodes.get(nodeId) ?? null : validation;
  }

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
