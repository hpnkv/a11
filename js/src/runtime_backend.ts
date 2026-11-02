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

import { ActionSchema, type ActionSettings } from './action_schema.js';
import {base64Encode, normalizeByteMap} from './bytes.js';
import { WorkerChunkStore, type WorkerStoreHandle } from './worker_chunk_store.js';
import {installFramingBridge, type FramingBridge} from './internal/wasm/native_framing.js';
import {installDataCodecBridge, type DataCodecBridge} from './internal/wasm/native_data_codec.js';
import {installSchemaBridge} from './internal/wasm/native_schema.js';
import {installChannelBridge, type ChannelBridge} from './internal/wasm/native_channel.js';
import {installStreamReferenceBridge,
  type StreamReferenceBridge} from './internal/wasm/native_stream_reference.js';
import {installAuthorizationBridge,
  type AuthorizationBridge} from './internal/wasm/native_authorization.js';
import {installSerializationBridge,
  type SerializationBridge} from './internal/wasm/native_serialization.js';
import {actionSchemaToBridge} from './internal/wasm/action_schema_bridge.js';
import {statusFromBridgeFailure} from './internal/wasm/status_bridge.js';
import {installStatusCodecBridge,
  type StatusCodecBridge} from './internal/wasm/native_status_codec.js';
import { WireMessage } from './data.js';
import { invalidArgumentError, isOk, okStatus, statusFromUnknown,
  unimplementedError, type Status,
  type StatusOr } from './status.js';
import {flowToolBridge, installFlowToolingService, runFlowBridge,
  type FlowRunnerBridge} from './internal/wasm/flow_bridge.js';
import {GraphAction, GraphNode, GraphSession, GraphSessionWithRecv, RuntimeGraph,
  type GraphBinding, type SessionCreateOptions} from './runtime_graph.js';
import {decodeWireBridge, encodeWireBridge,
  type WireCodecBridge} from './internal/wasm/wire_bridge.js';

/** The runtime executes through the native WASM backend. */
export type A11Backend = 'wasm';
export type A11Feature = 'codecs' | 'flowTooling' | 'chunkStore' |
  'nodes' | 'actions' | 'sessions' | 'flowExecution';

export interface A11WasmModule {
  flow(request: Record<string, unknown>): Record<string, unknown>;
  wire: WireCodecBridge;
  data: DataCodecBridge;
  schema: {
    validate(value: Record<string, unknown>): Uint8Array;
    describe(value: Record<string, unknown>, runnable: boolean,
      allPorts: boolean): Record<string, unknown>;
    parse(value: Record<string, unknown>): Record<string, unknown>;
    entries(value: unknown): Array<Record<string, unknown>>;
  };
  streamReference: StreamReferenceBridge;
  authorization: AuthorizationBridge;
  serialization: SerializationBridge;
  status: StatusCodecBridge;
  framing: FramingBridge;
}

/** Host persistence for a WASM store namespace. */
export interface A11SnapshotStore {
  load(): Promise<Uint8Array | null>;
  save(bytes: Uint8Array): Promise<void>;
}

export interface A11WasmWorkerOptions {
  storageId: string;
  persistence?: A11SnapshotStore;
  actions?: {env_get?: boolean; make_http_request?: boolean;
    'web-fetch'?: boolean};
  environment?: Record<string, string>;
  allowedEnvironmentNames?: string[];
  /** A transport implementing the `@curiositystack/a11/wasm/http` contract. */
  httpTransport?: unknown;
}

export interface A11FlowRunnerBridge extends FlowRunnerBridge, GraphBinding,
    ChannelBridge {
  store(id: string): WorkerStoreHandle;
  flushPersistence?(): Promise<void>;
}

export interface A11RuntimeOptions {
  /** WASM is the only runtime backend. */
  backend?: A11Backend;
  /** Retained for migration; each selected feature must use WASM. */
  features?: Partial<Record<A11Feature, A11Backend>>;
  wasm?: {
    /** Directory URL containing packaged index.mjs and native.mjs loaders. */
    assetsBaseUrl?: string | URL;
    /** Absolute URL of a packaged, content-hashed WASM module loader. */
    moduleUrl?: string | URL;
    /** A host-owned loader can supply bundler-specific asset handling. */
    load?: () => Promise<A11WasmModule>;
    storageId?: string;
    persistence?: A11SnapshotStore;
    actions?: A11WasmWorkerOptions['actions'];
    environment?: A11WasmWorkerOptions['environment'];
    allowedEnvironmentNames?: A11WasmWorkerOptions['allowedEnvironmentNames'];
    httpTransport?: A11WasmWorkerOptions['httpTransport'];
    /** Packaged native worker loader for stores and Flow execution. */
    flowRuntimeUrl?: string | URL;
    loadFlowRuntime?: (options: A11WasmWorkerOptions) =>
      Promise<A11FlowRunnerBridge>;
  };
}

export interface RuntimeActionOptions {
  id?: string;
  settings?: ActionSettings;
}

const FEATURES: readonly A11Feature[] = [
  'codecs', 'flowTooling', 'chunkStore', 'nodes', 'actions', 'sessions',
  'flowExecution',
];

function sessionBridgeOptions(options: SessionCreateOptions):
    StatusOr<Record<string, unknown>> {
  const {id: _id, deadline, headers, onStreamMessage: _onMessage,
    onStreamDone: _onDone, ...limits} = options;
  const values: Record<string, unknown> = {...limits};
  if (deadline !== undefined) {
    values.deadlineMs = deadline instanceof Date ? deadline.getTime() : deadline;
  }
  if (headers !== undefined) {
    const bytes = normalizeByteMap(headers);
    if (!isOk(bytes)) return bytes;
    values.headers = Object.fromEntries(
      [...bytes].map(([name, value]) => [name, base64Encode(value)]));
  }
  return values;
}

/** Runtime factory for the packaged native WASM implementation. */
export class A11Runtime {
  private readonly choices: Readonly<Record<A11Feature, A11Backend>>;
  private readonly native: A11WasmModule | null;
  private readonly nativeFlow: A11FlowRunnerBridge | null;
  private readonly graphRuntime: RuntimeGraph | null;
  private readonly persistence: A11SnapshotStore | undefined;
  private disposed = false;

  private constructor(choices: Record<A11Feature, A11Backend>, native: A11WasmModule | null,
                      nativeFlow: A11FlowRunnerBridge | null,
                      persistence: A11SnapshotStore | undefined) {
    this.choices = Object.freeze(choices);
    this.native = native;
    this.nativeFlow = nativeFlow;
    this.graphRuntime = nativeFlow === null ? null : new RuntimeGraph(nativeFlow);
    this.persistence = persistence;
  }

  static async create(options: A11RuntimeOptions = {}): Promise<StatusOr<A11Runtime>> {
    if (options.backend !== undefined && options.backend !== 'wasm') {
      return unimplementedError('A11Runtime supports only the WASM backend.');
    }
    const choices = {} as Record<A11Feature, A11Backend>;
    if (options.wasm?.assetsBaseUrl) {
      try {
        const base = new URL(String(options.wasm.assetsBaseUrl));
        if (!base.pathname.endsWith('/') || base.search || base.hash) {
          return invalidArgumentError('wasm.assetsBaseUrl must be a directory URL ending in /.');
        }
      } catch (error) {
        return statusFromUnknown(error, 'wasm.assetsBaseUrl is not a URL.');
      }
    }
    for (const feature of FEATURES) {
      const selected = options.features?.[feature] ?? 'wasm';
      if (selected !== 'wasm') {
        return unimplementedError(
          `${feature} supports only the WASM backend.`,
        );
      }
      choices[feature] = selected;
    }
    let native: A11WasmModule | null = null;
    let nativeFlow: A11FlowRunnerBridge | null = null;
    try {
      {
        if (options.wasm?.load) native = await options.wasm.load();
        else {
          const moduleUrl = options.wasm?.moduleUrl ?? (options.wasm?.assetsBaseUrl
            ? new URL('index.mjs', options.wasm.assetsBaseUrl)
            : new URL('./wasm/index.mjs', import.meta.url));
          const loaded = await import(/* webpackIgnore: true */ String(moduleUrl)) as {
            createA11Wasm?: () => Promise<A11WasmModule>;
          };
          if (!loaded.createA11Wasm) return unimplementedError('WASM module has no createA11Wasm export.');
          native = await loaded.createA11Wasm();
        }
      }
      {
        const workerOptions: A11WasmWorkerOptions = {
          storageId: options.wasm?.storageId ?? 'local',
          persistence: options.wasm?.persistence,
          actions: options.wasm?.actions,
          environment: options.wasm?.environment,
          allowedEnvironmentNames: options.wasm?.allowedEnvironmentNames,
          httpTransport: options.wasm?.httpTransport,
        };
        if (options.wasm?.loadFlowRuntime) {
          nativeFlow = await options.wasm.loadFlowRuntime(workerOptions);
        }
        else {
          const flowRuntimeUrl = options.wasm?.flowRuntimeUrl ??
            (options.wasm?.assetsBaseUrl
              ? new URL('native.mjs', options.wasm.assetsBaseUrl)
              : new URL('./wasm/native.mjs', import.meta.url));
          const loaded = await import(/* webpackIgnore: true */ String(flowRuntimeUrl)) as {
            createA11WasmRuntime?: (options: A11WasmWorkerOptions) =>
              Promise<A11FlowRunnerBridge>;
          };
          if (!loaded.createA11WasmRuntime) return unimplementedError('Native Flow module has no createA11WasmRuntime export.');
          nativeFlow = await loaded.createA11WasmRuntime(workerOptions);
        }
      }
      installFramingBridge(native!.framing);
      installFlowToolingService(native!);
      installDataCodecBridge(native!.data);
      installStreamReferenceBridge(native!.streamReference);
      installAuthorizationBridge(native!.authorization);
      installSerializationBridge(native!.serialization);
      installSchemaBridge({validate(schema) {
        const value = actionSchemaToBridge(schema);
        if (!isOk(value)) return value;
        try { native!.schema.validate(value); return okStatus(); }
        catch (error) { return statusFromBridgeFailure(error,
          'Native action schema validation failed.'); }
      }, describe(schema, runnable, allPorts) {
        const value = actionSchemaToBridge(schema);
        if (!isOk(value)) throw new Error(value.message);
        return native!.schema.describe(value, runnable, allPorts);
      }, parse(entry) {
        return native!.schema.parse(entry);
      }, entries(document) {
        return native!.schema.entries(document);
      }});
      if (nativeFlow) installChannelBridge(nativeFlow);
      installStatusCodecBridge(native!.status);
      return new A11Runtime(choices, native, nativeFlow,
        options.wasm?.persistence);
    } catch (error) {
      await nativeFlow?.dispose();
      return statusFromUnknown(error, 'A11 WASM initialization failed.');
    }
  }

  /** Report the implementation of a runtime feature. */
  backendFor(feature: A11Feature): A11Backend { return this.choices[feature]; }

  /** Report the backend of an object this factory produced, when known. */
  backendOf(value: object): A11Backend | null {
    if (value instanceof GraphNode || value instanceof GraphAction ||
        value instanceof GraphSession || value instanceof RuntimeGraph) return 'wasm';
    return value instanceof WorkerChunkStore ? 'wasm' : null;
  }

  /** Access the C++ node, action, and session graph in the worker. */
  graph(): StatusOr<RuntimeGraph> {
    const live = this.live();
    if (!isOk(live)) return live;
    return this.graphRuntime ?? unimplementedError(
      'Native graph requires WASM storage or Flow execution.');
  }

  /** Register a TypeScript callback as a runnable C++ Action. */
  async registerAction(schema: ActionSchema,
      handler: (action: GraphAction, context: {signal?: AbortSignal}) =>
        void | Status | Promise<void | Status>):
      Promise<StatusOr<() => Promise<Status>>> {
    const graph = this.graph();
    return isOk(graph) ? graph.registerAction(schema, handler) : graph;
  }

  /** Register a remote Action contract without a local handler. */
  async registerSchema(schema: ActionSchema):
      Promise<StatusOr<() => Promise<Status>>> {
    const graph = this.graph();
    return isOk(graph) ? graph.registerSchema(schema) : graph;
  }

  private live(): Status {
    return this.disposed ? unimplementedError('A11 runtime is disposed.') : okStatus();
  }

  private flush = async (): Promise<void> => {
    if (!this.persistence || !this.nativeFlow) return;
    if (!this.nativeFlow.flushPersistence) {
      throw new Error('Native worker does not expose persistence.');
    }
    await this.nativeFlow.flushPersistence();
  };

  /** Create a familiar LocalChunkStore backed by the native fragment log. */
  createChunkStore(id: string): StatusOr<WorkerChunkStore> {
    const live = this.live();
    if (!isOk(live)) return live;
    return WorkerChunkStore.fromHandle(id, this.nativeFlow!.store(id));
  }

  /** Create the native node namespace. */
  createNodeMap(): StatusOr<RuntimeGraph> {
    const live = this.live();
    if (!isOk(live)) return live;
    return this.graphRuntime!;
  }

  async createNode(id: string): Promise<StatusOr<GraphNode>> {
    const graph = this.graph();
    return isOk(graph) ? graph.node(id) : graph;
  }

  async createAction(schema: ActionSchema, options: RuntimeActionOptions = {}):
      Promise<StatusOr<GraphAction>> {
    const live = this.live();
    if (!isOk(live)) return live;
    if (!(schema instanceof ActionSchema)) {
      return invalidArgumentError('schema must be an ActionSchema.');
    }
    const valid = schema.validate();
    if (!isOk(valid)) return valid;
    const unsupported = Object.keys(options).find(name =>
      name !== 'id' && name !== 'settings');
    if (unsupported !== undefined) {
      return invalidArgumentError(`Runtime action does not support ${unsupported}.`);
    }
    return this.graphRuntime!.action(schema.name, options.id,
      options.settings, schema);
  }

  async createSession(options: SessionCreateOptions = {}):
      Promise<StatusOr<GraphSession>> {
    const live = this.live();
    if (!isOk(live)) return live;
    if (['nodeMap', 'actionRegistry'].some(name => name in options)) {
      return invalidArgumentError('Native sessions own their graph state.');
    }
    if ((options.onStreamMessage !== undefined &&
        typeof options.onStreamMessage !== 'function') ||
        (options.onStreamDone !== undefined &&
        typeof options.onStreamDone !== 'function')) {
      return invalidArgumentError('Session callbacks must be callable.');
    }
    const bridged = sessionBridgeOptions(options);
    return isOk(bridged) ? this.graphRuntime!.session(options.id, bridged,
      options) : bridged;
  }

  /** Create a native pull-style session for inbound wire messages. */
  async createSessionWithRecv(options: SessionCreateOptions = {}):
      Promise<StatusOr<GraphSessionWithRecv>> {
    const graph = this.graph();
    if (!isOk(graph)) return graph;
    if (['nodeMap', 'actionRegistry', 'onStreamMessage',
      'onStreamDone'].some(name => name in options)) {
      return invalidArgumentError(
        'Native receiving sessions own their nodes and action registry.');
    }
    const bridged = sessionBridgeOptions(options);
    return isOk(bridged) ? graph.sessionWithRecv(options.id, bridged) : bridged;
  }

  /** Encode through the C++ wire codec while retaining WireMessage values. */
  encodeWire(message: WireMessage): StatusOr<Uint8Array> {
    const live = this.live();
    if (!isOk(live)) return live;
    return encodeWireBridge(this.native!.wire, message);
  }

  decodeWire(bytes: Uint8Array): StatusOr<WireMessage> {
    const live = this.live();
    if (!isOk(live)) return live;
    return decodeWireBridge(this.native!.wire, bytes);
  }

  /** Native Flow service envelope. */
  flowTool(request: Record<string, unknown>): StatusOr<Record<string, unknown>> {
    const live = this.live();
    if (!isOk(live)) return live;
    return flowToolBridge(this.native!, request);
  }

  /** Execute a Flow in the pinned native worker, returning chunk JSON by port. */
  async runFlow(request: Record<string, unknown>, options: {signal?: AbortSignal;
      onOutput?: (port: string, chunk: Record<string, unknown>) => void} = {}):
      Promise<StatusOr<Record<string, unknown>>> {
    const live = this.live();
    if (!isOk(live)) return live;
    return runFlowBridge(this.nativeFlow, request, options);
  }

  async flushPersistence(): Promise<Status> {
    try { await this.flush(); return okStatus(); }
    catch (error) { return statusFromUnknown(error, 'WASM persistence failed.'); }
  }

  /** Flush host persistence and release the worker, returning any save failure. */
  async dispose(): Promise<Status> {
    if (this.disposed) return okStatus();
    this.disposed = true;
    const persisted = await this.flushPersistence();
    await this.nativeFlow?.dispose();
    return persisted;
  }
}

/** Initialize a runtime with the packaged WASM implementation by default. */
export function createA11Runtime(options: A11RuntimeOptions = {}): Promise<StatusOr<A11Runtime>> {
  return A11Runtime.create(options);
}
