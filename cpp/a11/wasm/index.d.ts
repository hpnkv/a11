export class A11WasmError extends Error {
  readonly code: number;
  readonly details: object[];
  readonly status: {code: number; message: string; details: object[]};
  constructor(code: number, message: string, details?: object[]);
  constructor(status: {code: number; message: string; details?: object[]});
}

export interface StorageCredentials {
  scheme: string;
  fields?: Record<string, string>;
}

export interface StorageIdentity {
  id: string;
  credentials?: StorageCredentials;
  services?: Array<{
    role: string;
    endpoint: string;
    credentials?: StorageCredentials;
  }>;
}

export interface NodeRef {
  id: string;
  offset?: number;
  length?: number;
  storage?: StorageIdentity;
}

export interface ActionMessage {
  id: string;
  name: string;
  inputs?: Array<{ name: string; id: string }>;
  outputs?: Array<{ name: string; id: string }>;
  headers?: Record<string, string>;
}

export interface ByteChunkingOptions {
  packetSize?: number;
  maxMessageSize?: number;
  maxPendingMessages?: number;
  maxPendingBytes?: number;
}

export interface WasmRuntime {
  register(name: string): void;
  invoke(action: ActionMessage): void;
  nextAction(): ActionMessage | null;
  finishAction(id: string, code?: number, message?: string): void;
  append(id: string, bytes: Uint8Array, mimetype?: string): void;
  appendRef(id: string, ref: NodeRef): void;
  putFragment(fragment: Record<string, unknown> & { id: string }): number;
  putMany(fragments: Array<Record<string, unknown> & { id: string }>): number[];
  close(id: string, code?: number, message?: string): void;
  readBytes(id: string, index: number): Uint8Array;
  readData(id: string, index: number): Uint8Array;
  read(id: string, index: number): Record<string, unknown>;
  readByArrival(id: string, arrival: number): Record<string, unknown>;
  clearData(id: string, seq: number): Record<string, unknown>;
  arrivalSeq(id: string, arrival: number): number;
  finalSeq(id: string): number | null;
  waitForFragment(id: string, index: number, wait?: {
    deadline?: number | Date | null;
    signal?: AbortSignal;
  }): Promise<Record<string, unknown>>;
  waitForChange(id: string, wait?: {
    deadline?: number | Date | null;
    signal?: AbortSignal;
  }): Promise<void>;
  stream(id: string, wait?: {
    from?: number;
    deadline?: number | Date | null;
    signal?: AbortSignal;
  }): AsyncIterable<Record<string, unknown>>;
  count(id: string): number;
  closed(id: string): boolean;
  resolve(ref: NodeRef, limits?: { maxDepth?: number; maxChunks?: number }):
    Array<Record<string, unknown>>;
  resolveAsync(ref: NodeRef, limits?: { maxDepth?: number; maxChunks?: number }):
    Promise<Array<Record<string, unknown>>>;
  exportSnapshot(): Uint8Array;
  importSnapshot(bytes: Uint8Array): void;
  runPending(handlers: Record<string, (action: ActionMessage,
    runtime: WasmRuntime) => void | Promise<void>>): Promise<void>;
  dispose(): void;
}

export interface SnapshotStore {
  load(): Promise<Uint8Array | null>;
  save(bytes: Uint8Array): Promise<void>;
}

export interface PersistentWasmRuntime extends WasmRuntime {
  flush(): Promise<void>;
  appendDurable(id: string, bytes: Uint8Array, mimetype?: string): Promise<void>;
  appendRefDurable(id: string, ref: NodeRef): Promise<void>;
  closeDurable(id: string, code?: number, message?: string): Promise<void>;
  finishActionDurable(id: string, code?: number, message?: string): Promise<void>;
  disposeDurable(): Promise<void>;
}

export interface A11Wasm {
  flow(request: Record<string, unknown>): Record<string, unknown>;
  wire: {
    encode(value: Record<string, unknown>): Uint8Array;
    decode(bytes: Uint8Array): Record<string, unknown>;
    decodeFragment(bytes: Uint8Array): Record<string, unknown>;
  };
  framing: {
    split(bytes: Uint8Array, transientId: bigint | number,
      packetSize?: number): Uint8Array[];
    reassembler(options?: ByteChunkingOptions): {
      feed(packet: Uint8Array): Uint8Array | null;
      dispose(): void;
    };
  };
  runtime(storageId?: string, options?: {
    resolveStorage?: (storage: NonNullable<NodeRef["storage"]>, ref: NodeRef,
      limits: { maxDepth?: number; maxChunks?: number }) =>
      Promise<Array<Record<string, unknown>>>;
  }): WasmRuntime;
  openPersistentRuntime(adapter: SnapshotStore, storageId?: string, options?: {
    resolveStorage?: (storage: StorageIdentity, ref: NodeRef,
      limits: { maxDepth?: number; maxChunks?: number }) =>
      Promise<Array<Record<string, unknown>>>;
  }): Promise<PersistentWasmRuntime>;
}

export function createA11Wasm(options?: {
  factory?: (options: { locateFile?: (path: string) => string }) => Promise<unknown>;
  locateFile?: (path: string) => string;
}): Promise<A11Wasm>;
