import type { A11HttpClient, HttpTransport } from './http-client.mjs';
export {createHttpClient} from './http-client.mjs';

export interface WasmFlowRun {
  source: string;
  flow: string;
  inputs?: Record<string, Array<Record<string, unknown>>>;
  timeoutMs?: number;
  maxOutputChunks?: number;
  maxOutputBytes?: number;
}

export interface WasmHostActionSchema {
  name: string;
  description?: string;
  inputs?: Array<{name: string; type: string; required?: boolean;
    unary?: boolean; description?: string}>;
  outputs?: Array<{name: string; type: string; required?: boolean;
    unary?: boolean; description?: string}>;
}

export interface WasmHostWireStream {
  readonly id?: string;
  getId?(): string;
  send(message: Record<string, unknown>):
    {code: number; message: string} | Promise<{code: number; message: string}>;
  halfClose(message: Record<string, unknown>):
    {code: number; message: string} | Promise<{code: number; message: string}>;
  abort(status: {code: number; message: string}):
    {code: number; message: string} | Promise<{code: number; message: string}>;
  getStatus?(): {code: number; message: string};
  start(onMessage: (message: Record<string, unknown> | null,
      trailers?: Record<string, unknown>) =>
      Promise<{code: number; message: string}>,
    onDone: () => Promise<{code: number; message: string}>):
      Promise<{code: number; message: string}>;
  accept(onMessage: (message: Record<string, unknown> | null,
      trailers?: Record<string, unknown>) =>
      Promise<{code: number; message: string}>,
    onDone: () => Promise<{code: number; message: string}>):
      Promise<{code: number; message: string}>;
}

export interface WasmSessionOptions {
  withRecv?: boolean;
  headers?: Record<string, string>;
  maxBufferedMessagesTotal?: number;
  maxBufferedMessagesPerStream?: number;
  maxConcurrentRootActions?: number;
  maxConcurrentNestedActions?: number;
  maxSingleMessageSize?: number;
  maxBufferedBytesTotal?: number;
  maxBufferedBytesPerStream?: number;
  noStreamTimeoutMs?: number | null;
  deadlineMs?: number | null;
}

export interface A11WasmRuntime {
  http: A11HttpClient;
  command(request: Record<string, unknown>, options?: {signal?: AbortSignal}):
    Promise<unknown>;
  node(id: string, scopeId?: string): WasmNode;
  hasNode(id: string): Promise<boolean>;
  nodeMapSize(): Promise<number>;
  nodeMapIds(): Promise<string[]>;
  discardNode(id: string): Promise<boolean>;
  resolveNode(reference: Record<string, unknown>): Promise<{id: string}>;
  store(id: string, scopeId?: string): WasmStore;
  action(name: string, id?: string, sessionId?: string,
    streamId?: string, settings?: Record<string, unknown>,
    schema?: WasmHostActionSchema): WasmAction;
  actionExisting(name: string, id: string): WasmAction;
  session(id?: string, options?: WasmSessionOptions): WasmSession;
  attachWireStream(sessionId: string, stream: WasmHostWireStream,
    mode?: 'start' | 'accept'): Promise<string>;
  detachWireStream(id: string): Promise<void>;
  flushPersistence(): Promise<void>;
  registerAction(schema: WasmHostActionSchema,
    handler: (action: WasmAction, context: {signal?: AbortSignal}) =>
      void | {code: number; message: string; details?: object[]} |
      Promise<void | {code: number; message: string; details?: object[]}>):
        () => void;
  registerRunnableAction(schema: WasmHostActionSchema,
    handler: (action: WasmAction, context: {signal?: AbortSignal}) =>
      void | {code: number; message: string; details?: object[]} |
      Promise<void | {code: number; message: string; details?: object[]}>):
        Promise<() => Promise<void>>;
  registerSchema(schema: WasmHostActionSchema):
    Promise<() => Promise<void>>;
  actionRegistryHas(name: string): Promise<boolean>;
  actionRegistrySchema(name: string): Promise<Record<string, unknown>>;
  actionRegistryNames(): Promise<string[]>;
  actionRegistryMessage(name: string, actionId?: string):
    Promise<Record<string, unknown>>;
  unregisterRegisteredAction(name: string): Promise<void>;
  runFlow(request: WasmFlowRun, options?: { signal?: AbortSignal;
    onOutput?: (port: string, chunk: Record<string, unknown>) => void }):
    Promise<Record<string, Array<Record<string, unknown>>>>;
  runFlowWithNodes(request: WasmFlowRun, options?: { signal?: AbortSignal;
    onOutput?: (port: string, chunk: Record<string, unknown>) => void }):
    Promise<{outputs: Record<string, Array<Record<string, unknown>>>;
      node(id: string): WasmNode; store(id: string): WasmStore;
      release(): Promise<void>}>;
  dispose(): Promise<void>;
}

export interface WasmNode {
  readonly id: string;
  ready(): Promise<{id: string}>;
  ref(options?: {offset?: number; length?: number | null;
    storage?: 'auto' | 'include' | 'omit'}): Promise<Record<string, unknown>>;
  putChunk(chunk: {data: string; metadata: Record<string, unknown>},
    options?: {sequence?: number; final?: boolean; signal?: AbortSignal}):
    Promise<{sequence: number}>;
  putFragment(fragment: Record<string, unknown>,
    options?: {signal?: AbortSignal}): Promise<{sequence: number}>;
  nextChunk(options?: {timeoutMs?: number; signal?: AbortSignal}):
    Promise<{data: string; metadata: Record<string, unknown>} | null>;
  nextFragment(options?: {timeoutMs?: number; signal?: AbortSignal}):
    Promise<Record<string, unknown> | null>;
  close(options?: {signal?: AbortSignal}): Promise<unknown>;
  abortWithStatus(status: {code: number; message: string; details?: object[]},
    options?: {signal?: AbortSignal}): Promise<unknown>;
  state(options?: {signal?: AbortSignal}): Promise<{
    readerStatus: {code: number; message: string; details?: object[]};
    writerStatus: {code: number; message: string; details?: object[]};
    writerAbortStatus: {code: number; message: string;
      details?: object[]} | null}>;
  readerOptions(): Promise<Record<string, unknown>>;
  setReaderOptions(options: Record<string, unknown>): Promise<unknown>;
  resetReader(options?: Record<string, unknown>): Promise<unknown>;
  writerOptions(): Promise<Record<string, unknown>>;
  setWriterOptions(options: Record<string, unknown>): Promise<unknown>;
  isWritable(options?: {signal?: AbortSignal}): Promise<boolean>;
  waitForBufferToDrain(options?: {timeoutMs?: number;
    signal?: AbortSignal}): Promise<unknown>;
  cancelReader(options?: {signal?: AbortSignal}): Promise<unknown>;
  cancelWriter(options?: {signal?: AbortSignal}): Promise<unknown>;
  finalize(value?: {chunk?: Record<string, unknown>;
    reference?: Record<string, unknown>; sequence?: number;
    close?: boolean; wait?: boolean},
    options?: {signal?: AbortSignal}): Promise<unknown>;
  cancel(options?: {signal?: AbortSignal}): Promise<unknown>;
  release(): Promise<unknown>;
}

export interface WasmStore {
  readonly id: string;
  ready(): Promise<number>;
  get(sequence: number, options?: {timeoutMs?: number; deadlineMs?: number;
    signal?: AbortSignal}):
    Promise<Record<string, unknown>>;
  getByArrival(order: number, options?: {timeoutMs?: number; deadlineMs?: number;
    signal?: AbortSignal}):
    Promise<Record<string, unknown>>;
  next(limit?: number, options?: {timeoutMs?: number; deadlineMs?: number;
    signal?: AbortSignal}):
    Promise<Array<Record<string, unknown> | null>>;
  putMany(fragments: Array<Record<string, unknown>>,
    options?: {signal?: AbortSignal}): Promise<number[]>;
  clearData(sequence: number, options?: {signal?: AbortSignal}):
    Promise<Record<string, unknown>>;
  arrivalSequence(order: number, options?: {signal?: AbortSignal}): Promise<number>;
  finalSequence(options?: {signal?: AbortSignal}): Promise<number | null>;
  size(options?: {signal?: AbortSignal}): Promise<number>;
  close(status?: {code: number; message: string},
    options?: {signal?: AbortSignal; returnExisting?: boolean}):
      Promise<{code: number; message: string}>;
}

export interface WasmAction {
  readonly id: string;
  readonly name: string;
  ready(): Promise<{id: string}>;
  input(name: string, bindStream?: boolean): Promise<WasmNode>;
  output(name: string, bindStream?: boolean): Promise<WasmNode>;
  port(name: string): Promise<WasmNode>;
  containsPort(name: string): Promise<boolean>;
  logNode(): Promise<WasmNode>;
  log(chunk: Record<string, unknown>): Promise<void>;
  sessionId(): Promise<string | null>;
  streamSourcesNode(requestPayload?: boolean): Promise<WasmNode>;
  exposeStreamSources(port: string, purpose: string): Promise<void>;
  registerStream(purpose: string, source: Record<string, unknown>,
    producerId?: string, parentProducerId?: string):
    Promise<Record<string, unknown>>;
  resolveNode(reference: Record<string, unknown>): Promise<WasmNode>;
  node(id: string): Promise<WasmNode>;
  run(options?: {signal?: AbortSignal}): Promise<unknown>;
  call(options?: {signal?: AbortSignal}): Promise<unknown>;
  state(options?: {signal?: AbortSignal}): Promise<{id: string;
    status: {code: number; message: string; details?: object[]};
    run: boolean; called: boolean; done: boolean;
    dispatchStatus?: {code: number; message: string;
      details?: object[]}}>;
  headers(options?: {signal?: AbortSignal}): Promise<Record<string, string>>;
  schema(): Promise<Record<string, unknown>>;
  settings(): Promise<Record<string, unknown>>;
  setSettings(settings: Record<string, unknown>): Promise<unknown>;
  makeNested(name: string, options?: {propagateIo?: boolean;
    forwardHeaders?: boolean;
    id?: string;
    schema?: WasmHostActionSchema}): Promise<WasmAction>;
  getHeader(name: string): Promise<string | null>;
  hasHeader(name: string): Promise<boolean>;
  setHeader(name: string, base64Value: string,
    options?: {signal?: AbortSignal}): Promise<unknown>;
  removeHeader(name: string, options?: {signal?: AbortSignal}):
    Promise<unknown>;
  wait(options?: {timeoutMs?: number; signal?: AbortSignal}): Promise<unknown>;
  waitForDispatch(options?: {timeoutMs?: number;
    signal?: AbortSignal}): Promise<{code: number; message: string;
      details?: object[]}>;
  cancel(options?: {signal?: AbortSignal}): Promise<unknown>;
}

export interface WasmSession {
  readonly id: string;
  ready(): Promise<{id: string}>;
  state(options?: {signal?: AbortSignal}): Promise<{id: string; closed: boolean;
    done: boolean; deadlineMs: number | null;
    status: {code: number; message: string; details?: object[]}}>;
  setDeadline(deadlineMs: number | null): Promise<unknown>;
  receiveWithStreamId(options?: {timeoutMs?: number; signal?: AbortSignal}):
    Promise<{message: Record<string, unknown>; streamId: string} | null>;
  done(options?: {timeoutMs?: number; signal?: AbortSignal}):
    Promise<{code: number; message: string; details?: object[]}>;
  halfClose(options?: {signal?: AbortSignal}): Promise<unknown>;
  cancelActions(options?: {signal?: AbortSignal}): Promise<unknown>;
  cancelAction(actionId: string,
    options?: {signal?: AbortSignal}): Promise<unknown>;
  actions(): Promise<Array<{id: string; action: WasmAction}>>;
  existingAction(actionId: string): Promise<WasmAction>;
  awaitAllActions(options?: {timeoutMs?: number;
    signal?: AbortSignal}): Promise<unknown>;
  abort(status?: {code: number; message: string; details?: object[]},
    options?: {signal?: AbortSignal}): Promise<unknown>;
  send(message: Record<string, unknown>, options?: {signal?: AbortSignal;
    streamId?: string}):
    Promise<unknown>;
  dispatchWireMessage(message: Record<string, unknown>, options?: {
    signal?: AbortSignal; streamId?: string}): Promise<unknown>;
  addStream(stream: WasmHostWireStream,
    mode?: 'start' | 'accept'): Promise<string>;
  action(name: string, id?: string, streamId?: string,
    settings?: Record<string, unknown>,
    schema?: WasmHostActionSchema): WasmAction;
}

export function createA11WasmRuntime(options?: {
  workerUrl?: URL | string;
  moduleUrl?: URL | string;
  wasmUrl?: URL | string;
  actions?: { env_get?: boolean; make_http_request?: boolean;
    'web-fetch'?: boolean };
  environment?: Record<string, string>;
  allowedEnvironmentNames?: string[];
  storageId?: string;
  persistence?: {load(): Promise<Uint8Array | null>;
    save(bytes: Uint8Array): Promise<void>};
  httpTransport?: HttpTransport;
}): Promise<A11WasmRuntime>;
