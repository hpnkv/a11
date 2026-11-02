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

import { createBridge } from "./bridge.mjs";
import {statusFromThrown} from './status.mjs';

let send;
let listen;
if (typeof process !== "undefined" && process.versions?.node) {
  const { parentPort } = await import("node:worker_threads");
  send = message => parentPort.postMessage(message);
  listen = callback => parentPort.on("message", callback);
} else {
  send = message => postMessage(message);
  listen = callback => addEventListener("message", event => callback(event.data));
}

async function initialize({ moduleUrl, wasmUrl, host }) {
try {
  const createModule = (await import(moduleUrl)).default;
  const module = await createModule({ locateFile: name =>
    name.endsWith(".wasm") ? wasmUrl : name });
  const { input, result, encoder, decoder } = createBridge(module);
  const running = new Map();
  const hostCalls = new Map();
  const wireCalls = new Map();
  const binaryCalls = new Map();
  let hostCallId = 0;
  globalThis.__a11WasmInvokeAction = (actionId, name) =>
    new Promise(resolve => {
      const id = ++hostCallId;
      hostCalls.set(id, resolve);
      send({invokeAction: {id, actionId, name}});
    });
  globalThis.__a11WasmInvokeSession = (sessionId, eventJson) =>
    new Promise(resolve => {
      const id = ++hostCallId;
      hostCalls.set(id, resolve);
      send({invokeSession: {id, sessionId, event: JSON.parse(eventJson)}});
    });
  globalThis.__a11WasmFlowOutput = (id, port, chunk) =>
    send({flowOutput: {id, port, chunk}});
  globalThis.__a11WasmActionCancelled = id =>
    send({actionCancelled: id});
  globalThis.__a11WasmWireEvent = (streamId, kind, payload) =>
    new Promise(resolve => {
      const id = ++hostCallId;
      wireCalls.set(id, resolve);
      send({wireEvent: {id, streamId, kind, payload}});
    });
  globalThis.__a11WasmBinaryEvent = (streamId, kind, payload) =>
    new Promise(resolve => {
      const id = ++hostCallId;
      binaryCalls.set(id, resolve);
      send({binaryEvent: {id, streamId, kind, payload}});
    });
  const objects = await module._a11_wasm_native_objects_new();
  if (!objects) throw new Error("native object state allocation failed");
  listen(async ({ id, request, command, cancel, callbackResult,
      wireResult, binaryResult }) => {
    if (callbackResult) {
      const resolve = hostCalls.get(callbackResult.id);
      if (resolve) {
        hostCalls.delete(callbackResult.id);
        resolve(callbackResult.status);
      }
      return;
    }
    if (wireResult) {
      const resolve = wireCalls.get(wireResult.id);
      if (resolve) {
        wireCalls.delete(wireResult.id);
        resolve(wireResult.status);
      }
      return;
    }
    if (binaryResult) {
      const resolve = binaryCalls.get(binaryResult.id);
      if (resolve) {
        binaryCalls.delete(binaryResult.id);
        resolve(binaryResult.answer);
      }
      return;
    }
    if (cancel) {
      const handle = running.get(id);
      if (handle) module._a11_wasm_task_cancel(handle);
      return;
    }
    let handle = 0;
    try {
      handle = command === undefined
        ? input(encoder.encode(JSON.stringify({ ...request, host })), (pointer, size) =>
          module._a11_wasm_native_flow_start(objects, pointer, size))
        : input(encoder.encode(JSON.stringify(command)), (pointer, size) =>
          module._a11_wasm_native_command_start(objects, pointer, size));
      if (!handle) throw new Error("WASM task allocation failed");
      running.set(id, handle);
      let launchError = null;
      setTimeout(() => {
        if (running.get(id) !== handle) return;
        try { module._a11_wasm_task_launch(handle); }
        catch (error) { launchError = error; }
      }, 0);
      let encoded = null;
      while (encoded === null) {
        if (launchError) throw launchError;
        encoded = result(module._a11_wasm_task_poll(handle));
        if (encoded === null) await new Promise(resolve => setTimeout(resolve, 8));
      }
      send({ id, output: JSON.parse(decoder.decode(encoded)) });
    } catch (error) {
      send({id, error: statusFromThrown(error)});
    } finally {
      running.delete(id);
      if (handle) module._a11_wasm_task_free(handle);
    }
  });
  send({ ready: true });
} catch (error) {
  send({ ready: false, error: String(error?.stack ?? error) });
}
}

listen(message => {
  if (message?.init) void initialize(message.init);
});
