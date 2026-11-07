// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import { A11WasmError } from "./bridge.mjs";
import { createHttpClient } from "./http-client.mjs";
import {normalizeStatus, statusFromThrown, StatusCode} from './status.mjs';
import {createNativeNode} from './native-node.mjs';
import {createNativeStore} from './native-store.mjs';
import {createNativeAction} from './native-action.mjs';
import {createNativeSession} from './native-session.mjs';
import {registerHttpActions} from './native-http-actions.mjs';
import {decodeSnapshot, encodeSnapshot} from './snapshot-host.mjs';

export { createHttpClient } from "./http-client.mjs";

const DEFAULT_HOST_ACTION_TIMEOUT_MS = 30_000;

export async function createA11WasmRuntime(options = {}) {
  const WorkerClass = globalThis.Worker ??
    (await import("node:worker_threads")).Worker;
  const workerUrl = new URL(options.workerUrl ?? "./native_worker.mjs", import.meta.url);
  const moduleUrl = String(options.moduleUrl ??
    new URL("./build/threaded/a11-native.mjs", import.meta.url));
  const wasmUrl = String(options.wasmUrl ??
    new URL("./build/threaded/a11-native.wasm", import.meta.url));
  const allowedEnvironmentNames = options.allowedEnvironmentNames ??
    Object.keys(options.environment ?? {});
  const environment = {};
  for (const name of allowedEnvironmentNames) {
    if (typeof name !== "string") throw new TypeError("environment names must be strings");
    if (Object.hasOwn(options.environment ?? {}, name)) {
      environment[name] = options.environment[name];
    } else if (typeof process !== "undefined" && process.versions?.node &&
               Object.hasOwn(process.env, name)) {
      environment[name] = process.env[name];
    }
  }
  const host = {
    actions: {
      env_get: options.actions?.env_get !== false,
      make_http_request: options.actions?.make_http_request !== false,
      'web-fetch': options.actions?.['web-fetch'] !== false,
    },
    allowedEnvironmentNames: [...allowedEnvironmentNames],
    environment,
  };
  const storageId = options.storageId ?? 'local';
  if (typeof storageId !== 'string' || !storageId) {
    throw new TypeError('storageId must be a non-empty string');
  }
  let worker;
  let ready;
  let disposed = false;
  let terminalError = null;
  let serial = 0;
  const pending = new Map();
  const hostActions = new Map();
  const sessionCallbacks = new Map();
  const registryGenerations = new Map();
  const activeCallbacks = new Map();
  const hostStreams = new Map();
  const locallyDetached = new WeakSet();
  const hostChannels = new Map();
  const channelCallbacks = new Map();
  const attachedChannelStreams = new Map();
  const releasingHostStreams = new Map();
  let saveQueue = Promise.resolve();
  let workerTermination = Promise.resolve();

  function releaseHostStream(id) {
    if (releasingHostStreams.has(id)) return releasingHostStreams.get(id);
    const stream = hostStreams.get(id);
    if (!stream) return Promise.resolve();
    const release = command({operation: 'host_stream_release', id}).finally(() => {
      if (hostStreams.get(id) === stream) hostStreams.delete(id);
      if (releasingHostStreams.get(id) === release) {
        releasingHostStreams.delete(id);
      }
    });
    releasingHostStreams.set(id, release);
    return release;
  }

  function stop(error) {
    if (worker) {
      workerTermination = Promise.resolve(worker.terminate()).then(() => {});
    }
    worker = null;
    terminalError = error instanceof Error ? error : new Error(String(error));
    const terminal = {code: disposed ? StatusCode.CANCELLED :
      StatusCode.UNAVAILABLE, message: String(error?.message ?? error),
      details: []};
    for (const callback of activeCallbacks.values()) callback.abort();
    activeCallbacks.clear();
    sessionCallbacks.clear();
    for (const stream of hostStreams.values()) {
      void Promise.resolve().then(() => stream.abort(terminal)).catch(() => {});
    }
    hostStreams.clear();
    for (const channel of hostChannels.values()) {
      try { channel.close(); } catch {}
    }
    hostChannels.clear();
    channelCallbacks.clear();
    attachedChannelStreams.clear();
    releasingHostStreams.clear();
    for (const [id, item] of pending) {
      pending.delete(id);
      item.signal?.removeEventListener("abort", item.abort);
      if (item.abortTimer) clearTimeout(item.abortTimer);
      item.reject(error);
    }
  }

  function spawn() {
    return new Promise((resolve, reject) => {
      const current = new WorkerClass(workerUrl, { type: "module" });
      worker = current;
      function message(value) {
        const data = value?.data ?? value;
        if (data.binaryEvent) {
          const event = data.binaryEvent;
          Promise.resolve().then(async () => {
            const channel = hostChannels.get(event.streamId);
            if (!channel) return {status: {code: StatusCode.NOT_FOUND,
              message: 'Host binary channel is unknown', details: []}};
            let value;
            let status;
            if (event.kind === 'open') status = await channel.open();
            else if (event.kind === 'send') {
              status = channel.send(decodeSnapshot(event.payload));
            } else if (event.kind === 'is_open') {
              value = channel.isOpen();
            } else if (event.kind === 'buffered_amount') {
              value = channel.bufferedAmount();
              if (value && typeof value === 'object' &&
                  typeof value.code === 'number') {
                status = value;
                value = undefined;
              }
            } else if (event.kind === 'close') status = channel.close();
            else if (event.kind === 'abort') status = channel.abort
              ? channel.abort(event.payload) : channel.close();
            else if (event.kind === 'reset') status = channel.resetCallbacks();
            else return {status: {code: StatusCode.UNIMPLEMENTED,
              message: `Unknown binary event ${event.kind}`, details: []}};
            if (event.kind === 'is_open' && typeof value !== 'boolean') {
              throw new TypeError('BinaryChannel.isOpen() must return boolean');
            }
            if (event.kind === 'buffered_amount' &&
                (!Number.isSafeInteger(value) || value < 0)) {
              throw new TypeError('BinaryChannel.bufferedAmount() must be nonnegative');
            }
            return {status: normalizeStatus(status ?? {code: StatusCode.OK,
              message: '', details: []}, 'Invalid BinaryChannel status'), value};
          }).catch(error => ({status: statusFromThrown(error,
            'Host binary channel operation failed')})).then(answer => {
            current.postMessage({binaryResult: {id: event.id, answer}});
          });
          return;
        }
        if (data.wireEvent) {
          const event = data.wireEvent;
          Promise.resolve().then(async () => {
            const callbacks = channelCallbacks.get(event.streamId);
            if (callbacks) {
              if (event.kind === 'message') {
                return normalizeStatus(await callbacks.onMessage(event.payload),
                  'Invalid channel message callback status');
              }
              if (event.kind === 'half_close') {
                return normalizeStatus(await callbacks.onMessage(null,
                  event.payload),
                  'Invalid channel half-close callback status');
              }
              if (event.kind === 'done') {
                return normalizeStatus(await callbacks.onDone(event.payload),
                  'Invalid channel done callback status');
              }
            }
            const stream = hostStreams.get(event.streamId);
            if (event.kind === 'done' &&
                (!stream || locallyDetached.has(stream))) {
              return {code: StatusCode.OK, message: '', details: []};
            }
            if (!stream) return {code: StatusCode.NOT_FOUND,
              message: 'Host wire stream is not attached', details: []};
            let result;
            if (event.kind === 'message') result = await stream.send(event.payload);
            else if (event.kind === 'half_close') {
              result = await stream.halfClose(event.payload);
            } else if (event.kind === 'done') {
              if (event.payload.code !== StatusCode.OK) {
                result = await stream.abort(event.payload);
              }
            } else return {code: StatusCode.UNIMPLEMENTED,
              message: `Unknown host wire event ${event.kind}`, details: []};
            return result === undefined
              ? {code: StatusCode.OK, message: '', details: []}
              : normalizeStatus(result, 'Host wire stream returned an invalid status');
          }).catch(error => statusFromThrown(error,
            'Host wire stream operation failed')).then(status => {
            current.postMessage({wireResult: {id: event.id, status}});
            if (event.kind === 'done') {
              void releaseHostStream(event.streamId).catch(() => {});
            }
          });
          return;
        }
        if (data.actionCancelled) {
          activeCallbacks.get(data.actionCancelled)?.abort();
          return;
        }
        if (data.flowOutput) {
          const output = data.flowOutput;
          const id = Number(output.id);
          const item = pending.get(id);
          if (item?.onOutput) {
            try { item.onOutput(output.port, output.chunk); }
            catch (error) {
              pending.delete(id);
              item.signal?.removeEventListener('abort', item.abort);
              current.postMessage({id, cancel: true});
              item.reject(error);
            }
          }
          return;
        }
        if (data.invokeAction) {
          const invocation = data.invokeAction;
          const registered = hostActions.get(invocation.name);
          const flow = pending.get(Number(invocation.flowId));
          const signal = flow?.signal;
          const callbackController = new AbortController();
          activeCallbacks.set(invocation.actionId, callbackController);
          const remaining = Math.max(0,
            (flow?.deadlineAt ?? Date.now() + DEFAULT_HOST_ACTION_TIMEOUT_MS) -
              Date.now());
          let timer;
          let onAbort;
          const timed = new Promise(resolve => {
            timer = setTimeout(() => {
              resolve({code: StatusCode.DEADLINE_EXCEEDED,
                message: 'Host action deadline exceeded', details: []});
              callbackController.abort();
            }, remaining);
          });
          const cancelled = new Promise(resolve => {
            onAbort = () => callbackController.abort();
            callbackController.signal.addEventListener('abort', () => {
              resolve({code: StatusCode.CANCELLED,
                message: 'Host action cancelled', details: []});
            }, {once: true});
            if (signal?.aborted) onAbort();
            else signal?.addEventListener('abort', onAbort, {once: true});
          });
          const invoked = Promise.resolve().then(async () => {
            if (!registered) return {code: StatusCode.NOT_FOUND,
              message: `Host action ${invocation.name} is not registered`,
              details: []};
            const result = await registered.handler(
              api.action(invocation.name, invocation.actionId),
              {signal: callbackController.signal});
            return result === undefined
              ? {code: StatusCode.OK, message: '', details: []}
              : normalizeStatus(result,
                'Host action returned an invalid status');
          }).catch(error => statusFromThrown(error, 'Host action failed'));
          void Promise.race([invoked, timed, cancelled]).then(status => {
            clearTimeout(timer);
            signal?.removeEventListener('abort', onAbort);
            activeCallbacks.delete(invocation.actionId);
            current.postMessage({callbackResult: {id: invocation.id, status}});
          });
          return;
        }
        if (data.invokeSession) {
          const invocation = data.invokeSession;
          const callbacks = sessionCallbacks.get(invocation.sessionId);
          const event = invocation.event;
          const stream = hostStreams.get(event?.streamId) ??
            attachedChannelStreams.get(`${invocation.sessionId}\0${event?.streamId}`);
          const invoked = Promise.resolve().then(async () => {
            if (!callbacks || !stream) return {code: StatusCode.NOT_FOUND,
              message: 'Session callback or stream is unavailable',
              details: []};
            const callback = event.kind === 'message'
              ? callbacks.onMessage : event.kind === 'done'
                ? callbacks.onDone : null;
            if (callback === null) return {code: StatusCode.UNIMPLEMENTED,
              message: 'Unknown session callback event', details: []};
            if (!callback) return {code: StatusCode.OK, message: '', details: []};
            const result = event.kind === 'message'
              ? await callback(event.message, stream.host ?? stream)
              : await callback(stream.host ?? stream);
            return result === undefined
              ? {code: StatusCode.OK, message: '', details: []}
              : normalizeStatus(result, 'Session callback returned an invalid status');
          }).catch(error => statusFromThrown(error,
            'Session callback failed'));
          let timer;
          const timed = new Promise(resolve => {
            timer = setTimeout(() => resolve({
              code: StatusCode.DEADLINE_EXCEEDED,
              message: 'Session callback deadline exceeded', details: [],
            }), DEFAULT_HOST_ACTION_TIMEOUT_MS);
          });
          void Promise.race([invoked, timed]).then(status => {
            clearTimeout(timer);
            if (!disposed && worker === current) {
              current.postMessage({callbackResult: {id: invocation.id, status}});
            }
          });
          return;
        }
        if (data.ready !== undefined) {
          if (data.ready) resolve();
          else reject(new Error(data.error));
          return;
        }
        const item = pending.get(data.id);
        if (!item) return;
        pending.delete(data.id);
        item.signal?.removeEventListener("abort", item.abort);
        if (item.abortTimer) clearTimeout(item.abortTimer);
        if (item.signal?.aborted) {
          item.reject(new A11WasmError(StatusCode.CANCELLED, "native operation cancelled"));
        } else if (data.error) item.reject(new A11WasmError(data.error));
        else item.resolve(data.output);
      }
      function failure(error) {
        if (worker !== current) return;
        const reason = error instanceof Error ? error
          : new Error([error?.message, error?.filename,
            error?.lineno, error?.error?.stack].filter(Boolean).join(': ') ||
            String(error));
        stop(reason);
        reject(reason);
      }
      if (typeof current.addEventListener === "function") {
        current.addEventListener("message", message);
        current.addEventListener("error", failure);
      } else {
        current.on("message", message);
        current.on("error", failure);
        current.on("exit", code => {
          if (code !== 0) failure(new Error(`WASM worker exited: ${code}`));
        });
      }
      current.postMessage({ init: { moduleUrl, wasmUrl, host } });
    });
  }

  ready = spawn();
  await ready;
  function command(request, {signal} = {}) {
    if (disposed) return Promise.reject(new Error("native WASM runtime is disposed"));
    if (terminalError) return Promise.reject(terminalError);
    if (signal?.aborted) return Promise.reject(new A11WasmError(StatusCode.CANCELLED, "native operation cancelled"));
    return new Promise((resolve, reject) => {
      const id = ++serial;
      const abort = () => {
        if (!pending.has(id)) return;
        worker.postMessage({id, cancel: true});
      };
      pending.set(id, {resolve, reject, signal, abort});
      signal?.addEventListener("abort", abort, {once: true});
      worker.postMessage({id, command: request});
    });
  }
  const api = {
    http: createHttpClient({ transport: options.httpTransport }),
    command,
    registerAction(schema, handler) {
      if (!schema || typeof schema !== "object" || Array.isArray(schema) ||
          typeof schema.name !== "string" || !schema.name ||
          typeof handler !== "function") {
        throw new TypeError("registerAction requires an action schema and handler");
      }
      if (hostActions.has(schema.name)) {
        throw new Error(`Host action ${schema.name} is already registered`);
      }
      hostActions.set(schema.name, {schema, handler});
      return () => hostActions.delete(schema.name);
    },
    async registerRunnableAction(schema, handler) {
      if (!schema || typeof schema !== 'object' ||
          typeof schema.name !== 'string' || !schema.name ||
          typeof handler !== 'function') {
        throw new TypeError('registerRunnableAction requires a schema and handler');
      }
      const previous = hostActions.get(schema.name);
      const current = {schema, handler};
      hostActions.set(schema.name, current);
      try {
        await command({operation: 'action_register', id: schema.name, schema});
      } catch (error) {
        if (previous === undefined) hostActions.delete(schema.name);
        else hostActions.set(schema.name, previous);
        throw error;
      }
      const generation = Symbol(schema.name);
      registryGenerations.set(schema.name, generation);
      return async () => {
        if (registryGenerations.get(schema.name) !== generation) return;
        await command({operation: 'action_unregister', id: schema.name});
        registryGenerations.delete(schema.name);
        if (hostActions.get(schema.name) === current) {
          hostActions.delete(schema.name);
        }
      };
    },
    async registerSchema(schema) {
      if (!schema || typeof schema !== 'object' || Array.isArray(schema) ||
          typeof schema.name !== 'string' || !schema.name) {
        throw new TypeError('registerSchema requires an action schema');
      }
      await command({operation: 'action_register_schema', id: schema.name,
        schema});
      hostActions.delete(schema.name);
      const generation = Symbol(schema.name);
      registryGenerations.set(schema.name, generation);
      return async () => {
        if (registryGenerations.get(schema.name) !== generation) return;
        await command({operation: 'action_unregister', id: schema.name});
        registryGenerations.delete(schema.name);
      };
    },
    actionRegistryHas: name => command({operation: 'action_registry_has',
      id: name}),
    actionRegistrySchema: name => command({operation: 'action_registry_schema',
      id: name}),
    actionRegistryNames: () => command({operation: 'action_registry_names'}),
    actionRegistryMessage: (name, actionId) => command({operation:
      'action_registry_message', id: name, actionId}),
    async unregisterRegisteredAction(name) {
      await command({operation: 'action_unregister', id: name});
      registryGenerations.delete(name);
      hostActions.delete(name);
    },
    async createChannelStream(channel, id, role = 'either',
                              streamOptions = {}, framing = {}) {
      if (!channel || typeof channel.setCallbacks !== 'function' ||
          typeof id !== 'string' || !id) {
        throw new TypeError('A stream id and BinaryChannel are required');
      }
      const wireId = id;
      id = `channel-${++serial}-${wireId}`;
      hostChannels.set(id, channel);
      let prepared = false;
      try {
        await command({operation: 'channel_prepare', id, wireId, role,
          options: streamOptions, framing});
        prepared = true;
        const emit = (kind, payload = null) => {
          void command({operation: 'channel_event', id, kind, payload})
            .catch(() => {});
        };
        const status = normalizeStatus(channel.setCallbacks({
          onOpen: () => emit('open'),
          onMessage: packet => emit('message', encodeSnapshot(packet)),
          onError: error => emit('error', error),
          onClosed: () => emit('closed'),
          onBufferedAmountLow: () => emit('buffered_amount_low'),
        }), 'Invalid BinaryChannel callback status');
        if (status.code !== StatusCode.OK) throw new A11WasmError(status);
        return {
          __a11NativeChannelStream: true,
          ownerRuntime: api,
          id,
          wireId,
          send: message => command({operation: 'channel_send', id, message}),
          halfClose: message => command({operation: 'channel_half_close',
            id, message}),
          abort: status => command({operation: 'channel_abort', id, status}),
          drain: () => command({operation: 'channel_drain', id}),
          setDeadline: deadlineMs => command({operation: 'channel_deadline',
            id, deadlineMs}),
          status: () => command({operation: 'channel_status', id}),
          receiveStart(mode, onMessage, onDone) {
            channelCallbacks.set(id, {onMessage, onDone});
            return command({operation: mode === 'accept'
              ? 'channel_recv_accept' : 'channel_recv_start', id})
              .catch(error => { channelCallbacks.delete(id); throw error; });
          },
          receive: timeoutMs => command({operation: 'channel_recv_receive', id,
            ...(timeoutMs === undefined ? {} : {timeoutMs})}),
          start(onMessage, onDone, mode = 'start') {
            channelCallbacks.set(id, {onMessage, onDone});
            return command({operation: mode === 'accept'
              ? 'channel_accept' : 'channel_start', id});
          },
          async release() {
            channelCallbacks.delete(id);
            await command({operation: 'channel_release', id});
            hostChannels.delete(id);
          },
        };
      } catch (error) {
        hostChannels.delete(id);
        if (prepared) {
          void command({operation: 'channel_release', id}).catch(() => {});
        }
        throw error;
      }
    },
    async attachWireStream(sessionId, stream, mode = 'start') {
      if (stream?.__a11NativeChannelStream === true &&
          stream.ownerRuntime === api) {
        attachedChannelStreams.set(`${sessionId}\0${stream.wireId}`,
          stream.host ?? stream);
        try {
          await command({operation: 'session_add_channel_stream',
            id: stream.id, sessionId, mode});
        } catch (error) {
          attachedChannelStreams.delete(`${sessionId}\0${stream.wireId}`);
          throw error;
        }
        return stream.wireId;
      }
      if (stream?.__a11NativeChannelStream === true) {
        if (!stream.fallback) {
          throw new TypeError('Remote native channel has no host adapter');
        }
        stream = stream.fallback;
      }
      if (!stream || typeof stream !== 'object') {
        throw new TypeError('Wire stream is required');
      }
      const id = stream.id ?? stream.getId?.();
      if (typeof id !== 'string' || !id || hostStreams.has(id)) {
        throw new TypeError('Wire stream needs a unique id');
      }
      if (mode !== 'start' && mode !== 'accept') {
        throw new TypeError('Wire stream mode must be start or accept');
      }
      hostStreams.set(id, stream);
      let prepared = false;
      try {
        await command({operation: 'host_stream_prepare', id});
        prepared = true;
        const receive = async (message, trailers = {}) => {
          try {
            await command({operation: message === null
              ? 'host_stream_half_close' : 'host_stream_send', id,
              message: message ?? trailers});
            return {code: StatusCode.OK, message: '', details: []};
          } catch (error) {
            return statusFromThrown(error, 'Host wire receive failed');
          }
        };
        const done = async () => {
          const status = await stream.getStatus?.();
          try {
            await command({operation: status?.code !== undefined &&
              status.code !== StatusCode.OK ? 'host_stream_abort'
                : 'host_stream_half_close', id,
              ...(status?.code !== undefined && status.code !== StatusCode.OK
                ? {status} : {message: {}})});
            return {code: StatusCode.OK, message: '', details: []};
          } catch (error) {
            return statusFromThrown(error, 'Host wire completion failed');
          }
        };
        const started = Promise.resolve().then(() => stream[mode](receive, done));
        const attached = command({operation: 'session_add_host_stream', id,
          sessionId, mode});
        const [startStatus] = await Promise.all([started, attached]);
        const status = normalizeStatus(startStatus,
          'Host wire stream returned an invalid startup status');
        if (status.code !== StatusCode.OK) throw new A11WasmError(status);
        return id;
      } catch (error) {
        hostStreams.delete(id);
        void Promise.resolve().then(() => stream.abort(
          statusFromThrown(error, 'Host wire startup failed'))).catch(() => {});
        if (prepared) {
          void command({operation: 'host_stream_release', id}).catch(() => {});
        }
        throw error;
      }
    },
    async detachWireStream(id) {
      const stream = hostStreams.get(id);
      if (stream && !releasingHostStreams.has(id)) {
        locallyDetached.add(stream);
        const cancelled = {code: StatusCode.CANCELLED,
          message: 'Host wire stream detached', details: []};
        await command({operation: 'host_stream_abort', id,
          status: cancelled}).catch(() => {});
        await Promise.resolve(stream.abort(cancelled)).catch(() => {});
      }
      await releaseHostStream(id);
    },
    flushPersistence() {
      if (!options.persistence) return Promise.resolve();
      const save = saveQueue.then(async () => {
        const snapshot = await command({operation: 'snapshot_export',
          storageId});
        await options.persistence.save(decodeSnapshot(snapshot.data));
      });
      saveQueue = save.catch(() => {});
      return save;
    },
    node: (id, scopeId) => createNativeNode(command, id, scopeId),
    hasNode: id => command({operation: 'node_exists', id}),
    nodeMapSize: () => command({operation: 'node_map_size', id: ''}),
    nodeMapIds: () => command({operation: 'node_map_ids', id: ''}),
    discardNode: id => command({operation: 'node_map_discard', id}),
    resolveNode: reference => command({operation: 'node_map_resolve',
      id: '', reference}),
    store: (id, scopeId) => createNativeStore(command, id, scopeId),
    action: (name, id, sessionId, streamId, settings, schema) =>
      createNativeAction(command,
      nodeId => api.node(nodeId), name, id, sessionId, streamId,
      actionId => activeCallbacks.get(actionId)?.abort(), false, settings,
      schema),
    actionExisting: (name, id) => createNativeAction(command,
      nodeId => api.node(nodeId), name, id, undefined, undefined,
      actionId => activeCallbacks.get(actionId)?.abort(), true),
    session: (id, options = {}, callbacks) => {
      const sessionId = id ?? globalThis.crypto?.randomUUID?.();
      const session = createNativeSession(command,
      (name, actionId, sessionId, streamId, settings, schema) =>
        api.action(name, actionId, sessionId, streamId, settings, schema),
      (sessionId, stream, mode) => api.attachWireStream(sessionId, stream, mode),
      sessionId, {...options, withCallbacks: !!callbacks},
      (name, actionId) => api.actionExisting(name, actionId));
      return Object.freeze({...session, ready: async () => {
        const result = await session.ready();
        if (callbacks) sessionCallbacks.set(sessionId, callbacks);
        return result;
      }});
    },
    async runFlow(request, { signal, onOutput } = {}) {
      if (disposed) throw new Error("native WASM runtime is disposed");
      if (terminalError) throw terminalError;
      if (signal?.aborted) throw new A11WasmError(StatusCode.CANCELLED, "Flow run cancelled");
      await ready;
      if (signal?.aborted) throw new A11WasmError(StatusCode.CANCELLED, "Flow run cancelled");
      return new Promise((resolve, reject) => {
        const id = ++serial;
        const abort = () => {
          if (!pending.has(id)) return;
          worker.postMessage({ id, cancel: true });
          const item = pending.get(id);
          item.abortTimer = setTimeout(() => {
            if (!pending.has(id)) return;
            stop(new A11WasmError(StatusCode.CANCELLED, "Flow run cancelled"));
          }, 1000);
        };
        pending.set(id, { resolve, reject, signal, abort, onOutput,
          deadlineAt: Date.now() + (request.timeoutMs ?? 30_000) });
        signal?.addEventListener("abort", abort, { once: true });
        worker.postMessage({ id, request: {...request,
          hostFlowId: String(id),
          ...(onOutput ? {streamOutputs: true, streamId: String(id)} : {}),
          hostActions: [...hostActions.values()].flatMap(value =>
            value.schema ? [value.schema] : [])} });
      });
    },
    async runFlowWithNodes(request, options = {}) {
      const runId = globalThis.crypto?.randomUUID?.() ??
        (typeof process !== 'undefined' && process.versions?.node
          ? (await import('node:crypto')).randomUUID() : null);
      if (!runId) throw new Error('Secure UUID generation is unavailable');
      const outputs = await api.runFlow({...request, retainNodes: true, runId},
        options);
      let released = false;
      const liveScope = () => {
        if (released) throw new Error('Flow node scope is released');
      };
      return Object.freeze({outputs,
        node(id) { liveScope(); return api.node(id, runId); },
        store(id) { liveScope(); return api.store(id, runId); },
        async release() {
          if (released) return;
          await command({operation: 'flow_release', id: runId});
          released = true;
        },
      });
    },
    dispose() {
      if (disposed) return workerTermination;
      disposed = true;
      stop(new Error("native WASM runtime is disposed"));
      return workerTermination;
    },
  };
  registerHttpActions(hostActions, api, host.actions);
  try {
    await command({operation: 'configure', host, storageId});
  } catch (error) {
    api.dispose();
    throw error;
  }
  if (options.persistence) {
    try {
      const saved = await options.persistence.load();
      if (saved !== null && saved !== undefined) {
        await command({operation: 'snapshot_import', storageId,
          data: encodeSnapshot(saved)});
      }
    } catch (error) {
      api.dispose();
      throw error;
    }
  }
  return api;
}
