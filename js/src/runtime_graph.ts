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

import {ActionSchema, type ActionSettings} from './action_schema.js';
import {actionSchemaFromBridge, actionSchemaToBridge}
  from './internal/wasm/action_schema_bridge.js';
import {type A11Runtime} from './runtime_backend.js';
import type {SessionOptions, ChunkStoreReaderOptions,
  ChunkStoreWriterOptions} from './runtime_options.js';
import {base64Decode, base64Encode, toBytes, type ByteMap,
  type ByteMapInput,
  type ByteSource} from './bytes.js';
import {ActionMessage, Chunk, NodeFragment, NodeRef, validateName,
  WireMessage, wireMessageFromJsonValue, wireMessageToJsonValue} from './data.js';
import {fromRaw, toRaw} from './internal/wasm/fragment_bridge.js';
import {fromChunk, getGlobalSerializationRegistry,
  toChunk} from './serialization.js';
import {parseStreamReference, type StreamReference}
  from './stream_reference.js';
import {failedPreconditionError, invalidArgumentError, isOk, isStatus,
  okStatus,
  type Status, type StatusOr} from './status.js';
import {statusFromBridgeFailure, statusFromBridgeValue} from './internal/wasm/status_bridge.js';
import {bridgeWireStream} from './internal/wasm/wire_stream_bridge.js';
import {type ChannelHandle} from './internal/wasm/native_channel.js';
import {type WireStream} from './wire_stream.js';

/** Worker operations used by the typed node convenience. */
export interface NodeBinding {
  readonly id: string;
  ready(): Promise<unknown>;
  ref(options?: {offset?: number; length?: number | null;
    storage?: 'auto' | 'include' | 'omit'}): Promise<Record<string, unknown>>;
  putChunk(chunk: Record<string, unknown>, options?: {sequence?: number;
    final?: boolean}): Promise<{sequence: number}>;
  putFragment(fragment: Record<string, unknown>): Promise<{sequence: number}>;
  nextChunk(options?: {timeoutMs?: number; signal?: AbortSignal}):
    Promise<Record<string, unknown> | null>;
  nextFragment(options?: {timeoutMs?: number; signal?: AbortSignal}):
    Promise<Record<string, unknown> | null>;
  close(): Promise<unknown>;
  abortWithStatus(status: Status): Promise<unknown>;
  state(): Promise<{readerStatus: Status; writerStatus: Status;
    writerAbortStatus: Status | null}>;
  readerOptions(): Promise<ChunkStoreReaderOptions>;
  setReaderOptions(options: ChunkStoreReaderOptions): Promise<unknown>;
  resetReader(options?: ChunkStoreReaderOptions): Promise<unknown>;
  writerOptions(): Promise<ChunkStoreWriterOptions>;
  setWriterOptions(options: ChunkStoreWriterOptions): Promise<unknown>;
  isWritable(): Promise<boolean>;
  waitForBufferToDrain(options?: {timeoutMs?: number}): Promise<unknown>;
  cancelReader(): Promise<unknown>;
  cancelWriter(): Promise<unknown>;
  finalize(value?: {chunk?: Record<string, unknown>;
    reference?: Record<string, unknown>; sequence?: number;
    close?: boolean; wait?: boolean}): Promise<unknown>;
  cancel(): Promise<unknown>;
}

/** Worker operations used by the typed action convenience. */
export interface ActionBinding {
  readonly id: string;
  readonly name: string;
  ready(): Promise<unknown>;
  input(name: string, bindStream?: boolean): Promise<NodeBinding>;
  output(name: string, bindStream?: boolean): Promise<NodeBinding>;
  port(name: string): Promise<NodeBinding>;
  containsPort(name: string): Promise<boolean>;
  logNode(): Promise<NodeBinding>;
  log(chunk: Record<string, unknown>): Promise<void>;
  sessionId(): Promise<string | null>;
  streamSourcesNode(requestPayload?: boolean): Promise<NodeBinding>;
  exposeStreamSources(port: string, purpose: string): Promise<void>;
  registerStream(purpose: string, source: Record<string, unknown>,
    producerId?: string, parentProducerId?: string):
    Promise<Record<string, unknown>>;
  node(id: string): Promise<NodeBinding>;
  resolveNode(reference: Record<string, unknown>): Promise<NodeBinding>;
  run(): Promise<unknown>;
  call(): Promise<unknown>;
  wait(options?: {timeoutMs?: number}): Promise<unknown>;
  state(): Promise<{status: Status; run: boolean; called: boolean;
    done: boolean;
    dispatchStatus?: Status}>;
  headers(): Promise<Record<string, string>>;
  schema(): Promise<Record<string, unknown>>;
  settings(): Promise<Record<string, unknown>>;
  setSettings(settings: ActionSettings): Promise<unknown>;
  makeNested(name: string, options?: {propagateIo?: boolean;
    forwardHeaders?: boolean;
    id?: string;
    schema?: Record<string, unknown>}): Promise<ActionBinding>;
  getHeader(name: string): Promise<string | null>;
  hasHeader(name: string): Promise<boolean>;
  setHeader(name: string, value: string): Promise<unknown>;
  removeHeader(name: string): Promise<unknown>;
  waitForDispatch(options?: {timeoutMs?: number}): Promise<Status>;
  cancel(): Promise<unknown>;
  release(): Promise<unknown>;
}

/** Worker operations used by the typed session convenience. */
export interface SessionBinding {
  readonly id: string;
  ready(): Promise<unknown>;
  state(): Promise<{id: string; closed: boolean; done: boolean;
    deadlineMs: number | null;
    status: Status}>;
  setDeadline(deadlineMs: number | null): Promise<unknown>;
  receiveWithStreamId(options?: {timeoutMs?: number;
    signal?: AbortSignal}): Promise<{message: Record<string, unknown>;
      streamId: string} | null>;
  action(name: string, id?: string, streamId?: string,
    settings?: ActionSettings,
    schema?: Record<string, unknown>): ActionBinding;
  addStream(stream: ReturnType<typeof bridgeWireStream> | ChannelHandle,
    mode?: 'start' | 'accept'): Promise<string>;
  halfClose(): Promise<unknown>;
  cancelActions(): Promise<unknown>;
  cancelAction(actionId: string): Promise<unknown>;
  actions(): Promise<Array<{id: string; action: ActionBinding}>>;
  existingAction(actionId: string): Promise<ActionBinding>;
  awaitAllActions(options?: {timeoutMs?: number}): Promise<unknown>;
  send(message: Record<string, unknown>, options?: {streamId?: string}):
    Promise<unknown>;
  dispatchWireMessage(message: Record<string, unknown>,
    options?: {streamId?: string}): Promise<unknown>;
  done(options?: {timeoutMs?: number}): Promise<Status>;
  abort(status?: Status): Promise<unknown>;
}

/** Stable worker bridge consumed by the SDK's native graph conveniences. */
export interface GraphBinding {
  node(id: string): NodeBinding;
  hasNode(id: string): Promise<boolean>;
  nodeMapSize(): Promise<number>;
  nodeMapIds(): Promise<string[]>;
  discardNode(id: string): Promise<boolean>;
  resolveNode(reference: Record<string, unknown>): Promise<{id: string}>;
  action(name: string, id?: string, sessionId?: string,
    streamId?: string, settings?: ActionSettings,
    schema?: Record<string, unknown>): ActionBinding;
  session(id?: string, options?: Record<string, unknown>, callbacks?: {
    onMessage?: (message: Record<string, unknown> | null,
      stream: WireStream) => Promise<Status | void>;
    onDone?: (stream: WireStream) => Promise<Status | void>;
  }): SessionBinding;
  registerRunnableAction(schema: Record<string, unknown>,
    handler: (action: ActionBinding, context: {signal?: AbortSignal}) =>
      Promise<void | Status>): Promise<() => Promise<void>>;
  registerSchema(schema: Record<string, unknown>):
    Promise<() => Promise<void>>;
  actionRegistryHas(name: string): Promise<boolean>;
  actionRegistrySchema(name: string): Promise<Record<string, unknown>>;
  actionRegistryNames(): Promise<string[]>;
  actionRegistryMessage(name: string, actionId?: string):
    Promise<Record<string, unknown>>;
  unregisterRegisteredAction(name: string): Promise<void>;
}

function chunkFromRaw(id: string, value: Record<string, unknown>):
    StatusOr<Chunk> {
  const decoded = wireMessageFromJsonValue({node_fragments: [{id, data: value}]});
  if (!isOk(decoded)) return decoded;
  return decoded.nodeFragments[0]!.getChunk();
}

export interface AsyncNodeOptions {
  runtime: A11Runtime;
  readerOptions?: ChunkStoreReaderOptions;
  writerOptions?: ChunkStoreWriterOptions;
}

export interface ActionCreateOptions {
  runtime?: A11Runtime;
  session?: GraphSession;
  id?: string;
  streamId?: string;
  settings?: ActionSettings;
}

export interface SessionCreateOptions extends SessionOptions {
  id?: string;
  headers?: ByteMapInput;
  onStreamMessage?: (message: WireMessage | null, stream: WireStream,
    session: GraphSession) => void | Status | Promise<void | Status>;
  onStreamDone?: (stream: WireStream, session: GraphSession) =>
    void | Status | Promise<void | Status>;
}

/** A typed value stream whose reader and writer are C++ AsyncNode objects. */
export class GraphNode {
  constructor(private readonly binding: NodeBinding) {}

  static async create(id: string, options: AsyncNodeOptions):
      Promise<StatusOr<GraphNode>> {
    const node = await options.runtime.createNode(id);
    if (!isOk(node)) return node;
    if (options.readerOptions !== undefined) {
      const status = await node.setReaderOptions(options.readerOptions);
      if (!isOk(status)) return status;
    }
    if (options.writerOptions !== undefined) {
      const status = await node.setWriterOptions(options.writerOptions);
      if (!isOk(status)) return status;
    }
    return node;
  }

  getId(): string { return this.binding.id; }

  async getReaderOptions(): Promise<StatusOr<ChunkStoreReaderOptions>> {
    try { return await this.binding.readerOptions(); }
    catch (error) { return statusFromBridgeFailure(error,
      'Node reader options failed.'); }
  }
  async setReaderOptions(options: ChunkStoreReaderOptions): Promise<Status> {
    try { await this.binding.setReaderOptions(options); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error,
      'Node reader options failed.'); }
  }
  async resetReader(options?: ChunkStoreReaderOptions): Promise<Status> {
    try { await this.binding.resetReader(options); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error,
      'Node reader reset failed.'); }
  }
  async getWriterOptions(): Promise<StatusOr<ChunkStoreWriterOptions>> {
    try { return await this.binding.writerOptions(); }
    catch (error) { return statusFromBridgeFailure(error,
      'Node writer options failed.'); }
  }
  async setWriterOptions(options: ChunkStoreWriterOptions): Promise<Status> {
    try { await this.binding.setWriterOptions(options); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error,
      'Node writer options failed.'); }
  }

  async ref(options: {offset?: number; length?: number | null;
      storage?: 'auto' | 'include' | 'omit'} = {}):
      Promise<StatusOr<NodeRef>> {
    try {
      const raw = await this.binding.ref(options);
      const fragment = fromRaw({id: this.binding.id, data: raw});
      return isOk(fragment) ? fragment.getNodeRef() : fragment;
    } catch (error) {
      return statusFromBridgeFailure(error, 'Node reference failed.');
    }
  }

  async putChunk(chunk: Chunk | NodeRef, sequence?: number,
      final = false): Promise<StatusOr<number>> {
    const fragment = new NodeFragment({id: this.binding.id, data: chunk,
      seq: sequence ?? null, continued: !final});
    const raw = toRaw(fragment);
    if (!isOk(raw)) return raw;
    try {
      const written = chunk instanceof NodeRef
        ? await this.binding.putFragment(raw)
        : await this.binding.putChunk(raw.data as Record<string, unknown>,
          {sequence, final});
      return written.sequence;
    } catch (error) {
      return statusFromBridgeFailure(error, 'Node write failed.');
    }
  }

  async putFragment(fragment: NodeFragment): Promise<StatusOr<number>> {
    if (fragment.id !== this.binding.id) {
      return invalidArgumentError('Fragment belongs to another node.');
    }
    const raw = toRaw(fragment);
    if (!isOk(raw)) return raw;
    try { return (await this.binding.putFragment(raw)).sequence; }
    catch (error) { return statusFromBridgeFailure(error, 'Node write failed.'); }
  }

  async put(value: unknown, options: {seq?: number; final?: boolean;
      mimetype?: string} = {}): Promise<StatusOr<number>> {
    if (value instanceof NodeFragment) {
      if (options.seq !== undefined || options.final !== undefined ||
          options.mimetype !== undefined) {
        return invalidArgumentError('NodeFragment carries its own write options.');
      }
      return this.putFragment(value);
    }
    if (value instanceof Chunk || value instanceof NodeRef) {
      return this.putChunk(value, options.seq, options.final);
    }
    const chunk = await getGlobalSerializationRegistry().toChunk(value,
      options.mimetype ?? '');
    return isOk(chunk) ? this.putChunk(chunk, options.seq,
      options.final) : chunk;
  }

  async nextFragment(timeoutMs?: number, signal?: AbortSignal):
      Promise<StatusOr<NodeFragment | null>> {
    try {
      const raw = await this.binding.nextFragment({timeoutMs, signal});
      return raw === null ? null : fromRaw(raw);
    } catch (error) {
      return statusFromBridgeFailure(error, 'Node read failed.');
    }
  }

  async nextChunk(timeoutMs?: number, signal?: AbortSignal):
      Promise<StatusOr<Chunk | null>> {
    try {
      const raw = await this.binding.nextChunk({timeoutMs, signal});
      return raw === null ? null : chunkFromRaw(this.binding.id, raw);
    } catch (error) {
      return statusFromBridgeFailure(error, 'Node read failed.');
    }
  }

  async next<T = unknown>(timeoutOrOptions?: number | {timeoutMs?: number;
      signal?: AbortSignal; expectedTag?: string}, signal?: AbortSignal):
      Promise<StatusOr<T | null>> {
    const options = typeof timeoutOrOptions === 'number' ?
      {timeoutMs: timeoutOrOptions, signal} : timeoutOrOptions ?? {signal};
    const chunk = await this.nextChunk(options.timeoutMs, options.signal);
    if (!isOk(chunk)) return chunk;
    if (chunk === null || chunk.isNull) return null;
    return getGlobalSerializationRegistry().fromChunk<T>(chunk, [],
      options.expectedTag);
  }

  async consume<T = unknown>(options: {timeoutMs?: number;
      allowNone?: boolean} = {}): Promise<StatusOr<T | null>> {
    const started = Date.now();
    const first = await this.next<T>(options.timeoutMs);
    if (!isOk(first)) return first;
    if (first === null) {
      return options.allowNone ? null : failedPreconditionError(
        'AsyncNode is empty at the current reader offset.');
    }
    const remaining = options.timeoutMs === undefined ? undefined :
      Math.max(0, options.timeoutMs - (Date.now() - started));
    const second = await this.next<T>(remaining);
    if (!isOk(second)) return second;
    return second === null ? first : failedPreconditionError(
      'A consumed node contains more than one value.');
  }

  async *values<T = unknown>(options: {timeoutMs?: number;
      signal?: AbortSignal} = {}): AsyncGenerator<StatusOr<T>, void, void> {
    while (true) {
      const value = await this.next<T>(options.timeoutMs, options.signal);
      if (!isOk(value)) { yield value; return; }
      if (value === null) return;
      yield value;
    }
  }

  [Symbol.asyncIterator](): AsyncGenerator<StatusOr<unknown>, void, void> {
    return this.values();
  }

  async finalize(value?: unknown, options: {seq?: number; mimetype?: string;
      close?: boolean; wait?: boolean} = {}): Promise<Status> {
    if (value === undefined || value === null) {
      if (options.mimetype) {
        return invalidArgumentError('mimetype requires a final value.');
      }
    } else if (value instanceof Chunk || value instanceof NodeRef ||
        value instanceof NodeFragment) {
      if (options.mimetype) {
        return invalidArgumentError('mimetype is carried by a raw value.');
      }
    }
    let payload: Record<string, unknown> = {};
    if (value !== undefined && value !== null) {
      const encoded = value instanceof NodeFragment ? value.data :
        value instanceof Chunk || value instanceof NodeRef ? value :
        await getGlobalSerializationRegistry().toChunk(value,
          options.mimetype ?? '');
      if (!isOk(encoded)) return encoded;
      if (value instanceof NodeFragment && options.seq !== undefined) {
        return invalidArgumentError('seq is carried by a NodeFragment.');
      }
      if (value instanceof NodeFragment && value.id !== this.binding.id) {
        return invalidArgumentError('Fragment belongs to another node.');
      }
      const raw = toRaw(new NodeFragment({id: this.binding.id, data: encoded,
        seq: value instanceof NodeFragment ? value.seq : options.seq ?? null,
        continued: false}));
      if (!isOk(raw)) return raw;
      payload = encoded instanceof NodeRef ?
        {reference: raw.data as Record<string, unknown>} :
        {chunk: raw.data as Record<string, unknown>};
      if (value instanceof NodeFragment) options = {...options,
        seq: value.seq ?? undefined};
    }
    try {
      await this.binding.finalize({...payload, sequence: options.seq,
        close: options.close, wait: options.wait});
      return okStatus();
    }
    catch (error) { return statusFromBridgeFailure(error, 'Node finalize failed.'); }
  }

  async cancel(): Promise<Status> {
    try { await this.binding.cancel(); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error, 'Node cancel failed.'); }
  }

  async close(): Promise<Status> {
    try { await this.binding.close(); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error, 'Node close failed.'); }
  }

  async abortWithStatus(status: Status): Promise<Status> {
    if (isOk(status)) return invalidArgumentError('Abort status must be non-OK.');
    try { await this.binding.abortWithStatus(status); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error, 'Node abort failed.'); }
  }

  async isWritable(): Promise<StatusOr<boolean>> {
    try { return await this.binding.isWritable(); }
    catch (error) {
      return statusFromBridgeFailure(error, 'Node writability check failed.');
    }
  }
  async waitForBufferToDrain(timeoutMs?: number): Promise<Status> {
    try { await this.binding.waitForBufferToDrain({timeoutMs}); return okStatus(); }
    catch (error) {
      return statusFromBridgeFailure(error, 'Node buffer wait failed.');
    }
  }
  async cancelReader(): Promise<Status> {
    try { await this.binding.cancelReader(); return okStatus(); }
    catch (error) {
      return statusFromBridgeFailure(error, 'Node reader cancellation failed.');
    }
  }
  async cancelWriter(): Promise<Status> {
    try { await this.binding.cancelWriter(); return okStatus(); }
    catch (error) {
      return statusFromBridgeFailure(error, 'Node writer cancellation failed.');
    }
  }

  async state(): Promise<StatusOr<{readerStatus: Status; writerStatus: Status;
      writerAbortStatus: Status | null}>> {
    try {
      const value = await this.binding.state();
      if (!isStatus(value.readerStatus) || !isStatus(value.writerStatus) ||
          (value.writerAbortStatus !== null &&
            !isStatus(value.writerAbortStatus))) {
        return statusFromBridgeFailure(new TypeError('Node status is malformed.'),
          'Node state failed.');
      }
      const readerStatus = statusFromBridgeValue(value.readerStatus,
        'Node reader status is malformed.');
      const writerStatus = statusFromBridgeValue(value.writerStatus,
        'Node writer status is malformed.');
      const writerAbortStatus = value.writerAbortStatus === null ? null :
        statusFromBridgeValue(value.writerAbortStatus,
          'Node abort status is malformed.');
      return {readerStatus, writerStatus, writerAbortStatus};
    } catch (error) {
      return statusFromBridgeFailure(error, 'Node state failed.');
    }
  }
  async getReaderStatus(): Promise<Status> {
    const current = await this.state();
    return isOk(current) ? current.readerStatus : current;
  }
  async getWriterStatus(): Promise<Status> {
    const current = await this.state();
    return isOk(current) ? current.writerStatus : current;
  }
  async getWriterAbortStatus(): Promise<Status | null> {
    const current = await this.state();
    return isOk(current) ? current.writerAbortStatus : current;
  }
}

/** One C++ Action with familiar input, output, wait, and cancellation methods. */
export class GraphAction {
  constructor(private readonly binding: ActionBinding) {}
  static makeNodeId(actionId: string, nodeName: string): StatusOr<string> {
    const action = validateName(actionId);
    if (!isOk(action)) return action;
    const port = validateName(nodeName);
    if (!isOk(port)) return port;
    const id = `${actionId}#${nodeName}`;
    const valid = validateName(id);
    return isOk(valid) ? id : valid;
  }
  static create(schema: ActionSchema, options: ActionCreateOptions = {}):
      Promise<StatusOr<GraphAction>> {
    if (typeof options !== 'object' || options === null) {
      return Promise.resolve(invalidArgumentError(
        'Action.create options must be an object.'));
    }
    const unsupported = Object.keys(options).find(name =>
      !['runtime', 'session', 'id', 'streamId', 'settings'].includes(name));
    if (unsupported !== undefined) {
      return Promise.resolve(invalidArgumentError(
        `Action.create does not support ${unsupported}.`));
    }
    if (options.runtime !== undefined && options.session !== undefined) {
      return Promise.resolve(invalidArgumentError(
        'Action.create accepts one owner: runtime or session.'));
    }
    if (options.streamId !== undefined && options.session === undefined) {
      return Promise.resolve(invalidArgumentError(
        'Action.create streamId requires a session.'));
    }
    if (options.session !== undefined) {
      return options.session.createAction(schema, options.id,
        options.streamId, options.settings);
    }
    if (options.runtime !== undefined) {
      return options.runtime.createAction(schema, {id: options.id,
        settings: options.settings});
    }
    return Promise.resolve(invalidArgumentError(
      'Action.create requires an initialized runtime or session.'));
  }
  getId(): string { return this.binding.id; }
  getName(): string { return this.binding.name; }

  async getSchema(): Promise<StatusOr<ActionSchema>> {
    try { return actionSchemaFromBridge(await this.binding.schema()); }
    catch (error) { return statusFromBridgeFailure(error, 'Action schema failed.'); }
  }
  async getSettings(): Promise<StatusOr<ActionSettings>> {
    try {
      const values = await this.binding.settings();
      for (const name of ['bindStreamsOnInputsByDefault',
        'bindStreamsOnOutputsByDefault']) {
        const value = values[name];
        if (value !== null && value !== undefined &&
            typeof value !== 'boolean') {
          return statusFromBridgeFailure(new TypeError(
            `Action setting ${name} is malformed.`), 'Action settings failed.');
        }
      }
      for (const name of ['clearInputsAfterRun', 'clearOutputsAfterRun']) {
        if (typeof values[name] !== 'boolean') {
          return statusFromBridgeFailure(new TypeError(
            `Action setting ${name} is malformed.`), 'Action settings failed.');
        }
      }
      return {
        ...(typeof values.bindStreamsOnInputsByDefault === 'boolean'
          ? {bindStreamsOnInputsByDefault: values.bindStreamsOnInputsByDefault}
          : {}),
        ...(typeof values.bindStreamsOnOutputsByDefault === 'boolean'
          ? {bindStreamsOnOutputsByDefault: values.bindStreamsOnOutputsByDefault}
          : {}),
        clearInputsAfterRun: values.clearInputsAfterRun as boolean,
        clearOutputsAfterRun: values.clearOutputsAfterRun as boolean,
      };
    } catch (error) {
      return statusFromBridgeFailure(error, 'Action settings failed.');
    }
  }
  async setSettings(settings: ActionSettings): Promise<Status> {
    try { await this.binding.setSettings(settings); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error, 'Action settings failed.'); }
  }
  async makeNested(schemaOrName: ActionSchema | string,
      options: {propagateIo?: boolean; forwardHeaders?: boolean;
        id?: string} = {}):
      Promise<StatusOr<GraphAction>> {
    const name = schemaOrName instanceof ActionSchema ? schemaOrName.name :
      schemaOrName;
    if (schemaOrName instanceof ActionSchema) {
      const valid = schemaOrName.validate();
      if (!isOk(valid)) return valid;
    }
    const schema = schemaOrName instanceof ActionSchema ?
      actionSchemaToBridge(schemaOrName) : undefined;
    if (schema !== undefined && !isOk(schema)) return schema;
    try { return new GraphAction(await this.binding.makeNested(name, {
      ...options,
      ...(schema === undefined ? {} : {schema}),
    })); }
    catch (error) { return statusFromBridgeFailure(error, 'Nested action failed.'); }
  }

  async getInput(name: string, bindStream?: boolean): Promise<StatusOr<GraphNode>> {
    try { return new GraphNode(await this.binding.input(name, bindStream)); }
    catch (error) { return statusFromBridgeFailure(error, 'Action input failed.'); }
  }
  async getOutput(name: string, bindStream?: boolean): Promise<StatusOr<GraphNode>> {
    try { return new GraphNode(await this.binding.output(name, bindStream)); }
    catch (error) { return statusFromBridgeFailure(error, 'Action output failed.'); }
  }
  async getPort(name: string): Promise<StatusOr<GraphNode>> {
    try { return new GraphNode(await this.binding.port(name)); }
    catch (error) { return statusFromBridgeFailure(error, 'Action port failed.'); }
  }
  async containsPort(name: string): Promise<StatusOr<boolean>> {
    try { return await this.binding.containsPort(name); }
    catch (error) { return statusFromBridgeFailure(error, 'Action port failed.'); }
  }
  async getLogNode(): Promise<StatusOr<GraphNode>> {
    try { return new GraphNode(await this.binding.logNode()); }
    catch (error) { return statusFromBridgeFailure(error, 'Action log node failed.'); }
  }
  async log(value: unknown): Promise<Status> {
    const chunk = await toChunk(value);
    if (!isOk(chunk)) return chunk;
    const raw = toRaw(new NodeFragment({id: this.binding.id, data: chunk}));
    if (!isOk(raw)) return raw;
    try {
      await this.binding.log(raw.data as Record<string, unknown>);
      return okStatus();
    } catch (error) {
      return statusFromBridgeFailure(error, 'Action log failed.');
    }
  }
  async getSessionId(): Promise<StatusOr<string | null>> {
    try { return await this.binding.sessionId(); }
    catch (error) {
      return statusFromBridgeFailure(error, 'Action session lookup failed.');
    }
  }
  async getStreamSourcesNode(requestPayload = true):
      Promise<StatusOr<GraphNode>> {
    try {
      return new GraphNode(
        await this.binding.streamSourcesNode(requestPayload));
    } catch (error) {
      return statusFromBridgeFailure(error, 'Stream source node failed.');
    }
  }
  async exposeStreamSources(port: string, purpose: string): Promise<Status> {
    try {
      await this.binding.exposeStreamSources(port, purpose);
      return okStatus();
    } catch (error) {
      return statusFromBridgeFailure(error, 'Stream exposure failed.');
    }
  }
  async registerStream(purpose: string, source: Record<string, unknown>,
      producerId?: string, parentProducerId?: string):
      Promise<StatusOr<StreamReference>> {
    const encoded = await toChunk(source, 'application/json');
    if (!isOk(encoded)) return encoded;
    const raw = toRaw(new NodeFragment({id: this.binding.id,
      data: encoded}));
    if (!isOk(raw)) return raw;
    try {
      const registered = await this.binding.registerStream(purpose,
        raw.data as Record<string, unknown>, producerId, parentProducerId);
      const chunk = chunkFromRaw(this.binding.id, registered);
      if (!isOk(chunk)) return chunk;
      const value = await fromChunk(chunk);
      return isOk(value) ? parseStreamReference(value) : value;
    } catch (error) {
      return statusFromBridgeFailure(error, 'Stream registration failed.');
    }
  }
  async getNode(id: string): Promise<StatusOr<GraphNode>> {
    try { return new GraphNode(await this.binding.node(id)); }
    catch (error) { return statusFromBridgeFailure(error, 'Action node failed.'); }
  }
  async resolveNode(reference: NodeRef): Promise<StatusOr<GraphNode>> {
    const raw = toRaw(new NodeFragment({id: reference.id,
      data: reference}));
    if (!isOk(raw)) return raw;
    try {
      return new GraphNode(await this.binding.resolveNode(
        raw.data as Record<string, unknown>));
    } catch (error) {
      return statusFromBridgeFailure(error, 'Action node resolution failed.');
    }
  }
  async getStatus(): Promise<Status> {
    try { return statusFromBridgeValue((await this.binding.state()).status,
      'Action status is malformed.'); }
    catch (error) { return statusFromBridgeFailure(error, 'Action state failed.'); }
  }
  async isDone(): Promise<StatusOr<boolean>> {
    try { return (await this.binding.state()).done; }
    catch (error) { return statusFromBridgeFailure(error, 'Action state failed.'); }
  }
  async hasBeenRun(): Promise<StatusOr<boolean>> {
    try { return (await this.binding.state()).run; }
    catch (error) { return statusFromBridgeFailure(error, 'Action state failed.'); }
  }
  async hasBeenCalled(): Promise<StatusOr<boolean>> {
    try { return (await this.binding.state()).called; }
    catch (error) { return statusFromBridgeFailure(error, 'Action state failed.'); }
  }
  async getDispatchStatus(): Promise<Status | null> {
    try {
      const value = (await this.binding.state()).dispatchStatus;
      return value === undefined ? null : statusFromBridgeValue(value,
        'Action dispatch status is malformed.');
    } catch (error) {
      return statusFromBridgeFailure(error, 'Action state failed.');
    }
  }
  async getHeaders(): Promise<StatusOr<ByteMap>> {
    try {
      const values = await this.binding.headers();
      const headers: ByteMap = new Map();
      for (const [name, encoded] of Object.entries(values)) {
        const bytes = base64Decode(encoded);
        if (!isOk(bytes)) return bytes;
        headers.set(name, bytes);
      }
      return headers;
    } catch (error) { return statusFromBridgeFailure(error, 'Action headers failed.'); }
  }
  async getHeader(name: string): Promise<StatusOr<Uint8Array | null>> {
    try {
      const encoded = await this.binding.getHeader(name);
      return encoded === null ? null : base64Decode(encoded);
    } catch (error) { return statusFromBridgeFailure(error, 'Action header failed.'); }
  }
  async hasHeader(name: string): Promise<StatusOr<boolean>> {
    try { return await this.binding.hasHeader(name); }
    catch (error) { return statusFromBridgeFailure(error, 'Action header failed.'); }
  }
  async setHeader(name: string, value: ByteSource): Promise<Status> {
    const bytes = toBytes(value);
    if (!isOk(bytes)) return bytes;
    try { await this.binding.setHeader(name, base64Encode(bytes)); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error, 'Action header failed.'); }
  }
  async removeHeader(name: string): Promise<Status> {
    try { await this.binding.removeHeader(name); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error, 'Action header failed.'); }
  }
  async run(): Promise<StatusOr<GraphAction>> {
    try { await this.binding.run(); return this; }
    catch (error) { return statusFromBridgeFailure(error, 'Action run failed.'); }
  }
  async call(): Promise<StatusOr<GraphAction>> {
    try { await this.binding.call(); return this; }
    catch (error) { return statusFromBridgeFailure(error, 'Action call failed.'); }
  }
  async wait(timeoutMs?: number): Promise<StatusOr<GraphAction>> {
    try {
      await this.binding.wait({timeoutMs});
      const status = statusFromBridgeValue((await this.binding.state()).status,
        'Action status is malformed.');
      return isOk(status) ? this : status;
    } catch (error) {
      return statusFromBridgeFailure(error, 'Action wait failed.');
    }
  }
  async waitForDispatch(timeoutMs?: number): Promise<Status> {
    try { return statusFromBridgeValue(
      await this.binding.waitForDispatch({timeoutMs}),
      'Action dispatch status is malformed.'); }
    catch (error) { return statusFromBridgeFailure(error,
      'Action dispatch wait failed.'); }
  }
  async cancel(): Promise<Status> {
    try { await this.binding.cancel(); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error, 'Action cancel failed.'); }
  }
  async release(): Promise<Status> {
    try { await this.binding.release(); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error,
      'Action release failed.'); }
  }
}

/** One C++ Session with host WireStream attachment. */
export class GraphSession {
  private defaultStreamId: string | undefined;
  constructor(private readonly binding: SessionBinding,
      private readonly nodeMap: RuntimeGraph) {}
  static create(runtime: A11Runtime, options: SessionCreateOptions = {}):
      Promise<StatusOr<GraphSession>> {
    return runtime.createSession(options);
  }
  getId(): string { return this.binding.id; }
  getNodeMap(): RuntimeGraph { return this.nodeMap; }

  async isClosed(): Promise<StatusOr<boolean>> {
    try { return (await this.binding.state()).closed; }
    catch (error) { return statusFromBridgeFailure(error, 'Session state failed.'); }
  }
  async isDone(): Promise<StatusOr<boolean>> {
    try { return (await this.binding.state()).done; }
    catch (error) { return statusFromBridgeFailure(error, 'Session state failed.'); }
  }
  async getDeadline(): Promise<StatusOr<number | null>> {
    try { return (await this.binding.state()).deadlineMs; }
    catch (error) { return statusFromBridgeFailure(error, 'Session deadline failed.'); }
  }
  async setDeadline(deadline: Date | number | null): Promise<Status> {
    const deadlineMs = deadline instanceof Date ? deadline.getTime() : deadline;
    if (deadlineMs !== null && !Number.isSafeInteger(deadlineMs)) {
      return invalidArgumentError('Deadline must be a safe Unix millisecond timestamp.');
    }
    try { await this.binding.setDeadline(deadlineMs); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error, 'Session deadline failed.'); }
  }

  async getStatus(): Promise<Status> {
    try { return statusFromBridgeValue((await this.binding.state()).status,
      'Session status is malformed.'); }
    catch (error) { return statusFromBridgeFailure(error, 'Session state failed.'); }
  }

  async addStream(stream: WireStream, mode: 'start' | 'accept' = 'start'):
      Promise<Status> {
    try {
      const channel = 'nativeHandle' in stream &&
        typeof stream.nativeHandle === 'function'
        ? await stream.nativeHandle() : bridgeWireStream(stream);
      if ('__a11NativeChannelStream' in channel) channel.host = stream;
      this.defaultStreamId = await this.binding.addStream(channel, mode);
      return okStatus();
    }
    catch (error) { return statusFromBridgeFailure(error, 'Session stream failed.'); }
  }
  async createAction(schemaOrName: ActionSchema | string, id?: string,
      streamId?: string, settings?: ActionSettings):
      Promise<StatusOr<GraphAction>> {
    try {
      const name = schemaOrName instanceof ActionSchema ? schemaOrName.name :
        schemaOrName;
      if (schemaOrName instanceof ActionSchema) {
        const valid = schemaOrName.validate();
        if (!isOk(valid)) return valid;
      }
      const schema = schemaOrName instanceof ActionSchema ?
        actionSchemaToBridge(schemaOrName) : undefined;
      if (schema !== undefined && !isOk(schema)) return schema;
      const binding = this.binding.action(name, id,
        streamId ?? this.defaultStreamId, settings,
        schema);
      await binding.ready();
      return new GraphAction(binding);
    } catch (error) {
      return statusFromBridgeFailure(error, 'Session action failed.');
    }
  }
  async halfClose(): Promise<Status> {
    try { await this.binding.halfClose(); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error, 'Session close failed.'); }
  }
  async cancelActions(): Promise<Status> {
    try { await this.binding.cancelActions(); return okStatus(); }
    catch (error) {
      return statusFromBridgeFailure(error, 'Session action cancellation failed.');
    }
  }
  async cancelAction(actionId: string): Promise<Status> {
    try { await this.binding.cancelAction(actionId); return okStatus(); }
    catch (error) {
      return statusFromBridgeFailure(error, 'Session action cancellation failed.');
    }
  }
  async actions(): Promise<StatusOr<Array<[string, GraphAction]>>> {
    try {
      const found = await this.binding.actions();
      return found.map(item => [item.id, new GraphAction(item.action)]);
    } catch (error) {
      return statusFromBridgeFailure(error, 'Session actions failed.');
    }
  }
  async getAction(actionId: string): Promise<StatusOr<GraphAction>> {
    try { return new GraphAction(await this.binding.existingAction(actionId)); }
    catch (error) {
      return statusFromBridgeFailure(error, 'Session action lookup failed.');
    }
  }
  async awaitAllActions(timeoutMs?: number): Promise<Status> {
    try { await this.binding.awaitAllActions({timeoutMs}); return okStatus(); }
    catch (error) {
      return statusFromBridgeFailure(error, 'Session action wait failed.');
    }
  }
  async send(message: WireMessage, streamId?: string): Promise<Status> {
    const raw = wireMessageToJsonValue(message);
    if (!isOk(raw)) return raw;
    try { await this.binding.send(raw, {streamId}); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error, 'Session send failed.'); }
  }
  async dispatchWireMessage(message: WireMessage,
      streamId?: string): Promise<Status> {
    const raw = wireMessageToJsonValue(message);
    if (!isOk(raw)) return raw;
    try {
      await this.binding.dispatchWireMessage(raw, {streamId});
      return okStatus();
    } catch (error) {
      return statusFromBridgeFailure(error, 'Session dispatch failed.');
    }
  }
  async done(timeoutMs?: number): Promise<Status> {
    try { return statusFromBridgeValue(await this.binding.done({timeoutMs}),
      'Session status is malformed.'); }
    catch (error) { return statusFromBridgeFailure(error, 'Session wait failed.'); }
  }
  async abort(status?: Status): Promise<Status> {
    try { await this.binding.abort(status); return okStatus(); }
    catch (error) { return statusFromBridgeFailure(error, 'Session abort failed.'); }
  }
}

/** C++ SessionWithRecv with pull-style inbound message access. */
export class GraphSessionWithRecv extends GraphSession {
  static override create(runtime: A11Runtime,
      options: SessionCreateOptions = {}):
      Promise<StatusOr<GraphSessionWithRecv>> {
    return runtime.createSessionWithRecv(options);
  }
  constructor(private readonly receiver: SessionBinding,
      nodeMap: RuntimeGraph) {
    super(receiver, nodeMap);
  }

  async receiveWithStreamId(timeoutMs?: number, signal?: AbortSignal):
      Promise<StatusOr<{message: WireMessage; streamId: string} | null>> {
    try {
      const value = await this.receiver.receiveWithStreamId({timeoutMs, signal});
      if (value === null) return null;
      const message = wireMessageFromJsonValue(value.message);
      return isOk(message) ? {message, streamId: value.streamId} : message;
    } catch (error) {
      return statusFromBridgeFailure(error, 'Session receive failed.');
    }
  }

  async receive(timeoutMs?: number, signal?: AbortSignal):
      Promise<StatusOr<WireMessage | null>> {
    const received = await this.receiveWithStreamId(timeoutMs, signal);
    return isOk(received) && received !== null ? received.message : received;
  }
}

/** Create and register native graph objects without mirroring their state. */
export class RuntimeGraph {
  private readonly nodes = new Map<string, GraphNode>();
  private readonly pendingNodes = new Map<string, Promise<StatusOr<GraphNode>>>();
  constructor(private readonly binding: GraphBinding) {}
  static create(runtime: A11Runtime): StatusOr<RuntimeGraph> {
    return runtime.graph();
  }
  node(id: string): Promise<StatusOr<GraphNode>> {
    const existing = this.nodes.get(id);
    if (existing) {
      return this.binding.node(id).ready().then(() => existing,
        error => statusFromBridgeFailure(error, 'Node lookup failed.'));
    }
    const pending = this.pendingNodes.get(id);
    if (pending) return pending;
    const created = (async (): Promise<StatusOr<GraphNode>> => {
      try {
        const binding = this.binding.node(id);
        await binding.ready();
        const node = new GraphNode(binding);
        this.nodes.set(id, node);
        return node;
      } catch (error) {
        return statusFromBridgeFailure(error, 'Node creation failed.');
      } finally {
        this.pendingNodes.delete(id);
      }
    })();
    this.pendingNodes.set(id, created);
    return created;
  }
  get(id: string): Promise<StatusOr<GraphNode>> { return this.node(id); }
  async resolve(reference: NodeRef): Promise<StatusOr<GraphNode>> {
    const raw = toRaw(new NodeFragment({id: reference.id,
      data: reference}));
    if (!isOk(raw)) return raw;
    try {
      const resolved = await this.binding.resolveNode(
        raw.data as Record<string, unknown>);
      return this.node(resolved.id);
    } catch (error) {
      return statusFromBridgeFailure(error, 'Node reference resolution failed.');
    }
  }
  async getIfExists(id: string): Promise<StatusOr<GraphNode | null>> {
    try {
      if (!await this.binding.hasNode(id)) {
        this.nodes.delete(id);
        return null;
      }
      return this.node(id);
    } catch (error) {
      return statusFromBridgeFailure(error, 'Node lookup failed.');
    }
  }
  async entries(): Promise<StatusOr<Array<[string, GraphNode]>>> {
    try {
      const ids = await this.binding.nodeMapIds();
      const entries: Array<[string, GraphNode]> = [];
      for (const id of ids) {
        const node = await this.node(id);
        if (!isOk(node)) return node;
        entries.push([id, node]);
      }
      return entries;
    } catch (error) {
      return statusFromBridgeFailure(error, 'Node enumeration failed.');
    }
  }
  async size(): Promise<StatusOr<number>> {
    try { return await this.binding.nodeMapSize(); }
    catch (error) { return statusFromBridgeFailure(error, 'Node count failed.'); }
  }
  async discard(id: string): Promise<StatusOr<boolean>> {
    try {
      const removed = await this.binding.discardNode(id);
      this.nodes.delete(id);
      return removed;
    } catch (error) {
      return statusFromBridgeFailure(error, 'Node discard failed.');
    }
  }
  async action(name: string, id?: string,
      settings?: ActionSettings,
      schema?: ActionSchema): Promise<StatusOr<GraphAction>> {
    try {
      const description = schema === undefined ? undefined :
        actionSchemaToBridge(schema);
      if (description !== undefined && !isOk(description)) return description;
      const binding = this.binding.action(name, id, undefined, undefined,
        settings, description);
      await binding.ready();
      return new GraphAction(binding);
    } catch (error) { return statusFromBridgeFailure(error, 'Action creation failed.'); }
  }
  async session(id?: string, options: Record<string, unknown> = {},
      callbacks?: Pick<SessionCreateOptions, 'onStreamMessage' | 'onStreamDone'>):
      Promise<StatusOr<GraphSession>> {
    try {
      let session: GraphSession;
      const wrapped = callbacks?.onStreamMessage || callbacks?.onStreamDone ? {
        onMessage: callbacks.onStreamMessage ? async (
            raw: Record<string, unknown> | null, stream: WireStream) => {
          const message = raw === null ? null : wireMessageFromJsonValue(raw);
          if (message !== null && !isOk(message)) return message;
          return callbacks.onStreamMessage!(message, stream, session);
        } : undefined,
        onDone: callbacks.onStreamDone ? (stream: WireStream) =>
          Promise.resolve(callbacks.onStreamDone!(stream, session)) : undefined,
      } : undefined;
      const binding = this.binding.session(id, options, wrapped);
      await binding.ready();
      session = new GraphSession(binding, this);
      return session;
    } catch (error) { return statusFromBridgeFailure(error, 'Session creation failed.'); }
  }
  async sessionWithRecv(id?: string, options: Record<string, unknown> = {}):
      Promise<StatusOr<GraphSessionWithRecv>> {
    try {
      const binding = this.binding.session(id, {...options, withRecv: true});
      await binding.ready();
      return new GraphSessionWithRecv(binding, this);
    } catch (error) {
      return statusFromBridgeFailure(error, 'Receiving session creation failed.');
    }
  }
  async registerAction(schema: ActionSchema,
      handler: (action: GraphAction, context: {signal?: AbortSignal}) =>
        void | Status | Promise<void | Status>):
      Promise<StatusOr<() => Promise<Status>>> {
    const valid = schema.validate();
    if (!isOk(valid)) return valid;
    const description = actionSchemaToBridge(schema);
    if (!isOk(description)) return description;
    try {
      const unregister = await this.binding.registerRunnableAction(
        description,
        async (binding, context) => handler(new GraphAction(binding), context));
      return async () => {
        try { await unregister(); return okStatus(); }
        catch (error) {
          return statusFromBridgeFailure(error, 'Action unregister failed.');
        }
      };
    } catch (error) {
      return statusFromBridgeFailure(error, 'Action registration failed.');
    }
  }

  async registerSchema(schema: ActionSchema):
      Promise<StatusOr<() => Promise<Status>>> {
    const valid = schema.validate();
    if (!isOk(valid)) return valid;
    const description = actionSchemaToBridge(schema);
    if (!isOk(description)) return description;
    try {
      const unregister = await this.binding.registerSchema(
        description);
      return async () => {
        try { await unregister(); return okStatus(); }
        catch (error) {
          return statusFromBridgeFailure(error, 'Action unregister failed.');
        }
      };
    } catch (error) {
      return statusFromBridgeFailure(error, 'Action schema registration failed.');
    }
  }

  async isActionRegistered(name: string): Promise<StatusOr<boolean>> {
    try { return await this.binding.actionRegistryHas(name); }
    catch (error) {
      return statusFromBridgeFailure(error, 'Action lookup failed.');
    }
  }

  async getActionSchema(name: string): Promise<StatusOr<ActionSchema>> {
    try {
      return actionSchemaFromBridge(
        await this.binding.actionRegistrySchema(name));
    } catch (error) {
      return statusFromBridgeFailure(error, 'Action schema lookup failed.');
    }
  }

  async listRegisteredActions(): Promise<StatusOr<string[]>> {
    try { return await this.binding.actionRegistryNames(); }
    catch (error) {
      return statusFromBridgeFailure(error, 'Action listing failed.');
    }
  }

  async unregisterAction(name: string): Promise<Status> {
    try {
      await this.binding.unregisterRegisteredAction(name);
      return okStatus();
    } catch (error) {
      return statusFromBridgeFailure(error, 'Action unregister failed.');
    }
  }

  async makeActionMessage(name: string,
      actionId?: string): Promise<StatusOr<ActionMessage>> {
    try {
      const raw = await this.binding.actionRegistryMessage(name, actionId);
      const decoded = wireMessageFromJsonValue({actions: [raw]});
      return isOk(decoded) ? decoded.actions[0]! : decoded;
    } catch (error) {
      return statusFromBridgeFailure(error, 'Action message failed.');
    }
  }
}
