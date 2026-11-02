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

import { A11WasmError, createBridge } from "./bridge.mjs";
import {StatusCode} from "./status.mjs";

export { A11WasmError } from "./bridge.mjs";

const PACKET_TYPE_SIZE = 1;
const PACKET_ID_OFFSET = PACKET_TYPE_SIZE;
const PACKET_SEQUENCE_OFFSET = PACKET_ID_OFFSET + BigUint64Array.BYTES_PER_ELEMENT;
const PACKET_COUNT_OFFSET = PACKET_SEQUENCE_OFFSET + Uint32Array.BYTES_PER_ELEMENT;
const PACKET_PAYLOAD_OFFSET = PACKET_COUNT_OFFSET + Uint32Array.BYTES_PER_ELEMENT;

export async function createA11Wasm(options = {}) {
  const factory = options.factory ??
    (await import(new URL("./build/a11.mjs", import.meta.url))).default;
  const module = await factory(options.locateFile ? { locateFile: options.locateFile } : {});

  const { input, string, result, jsonResult, encoder, decoder } = createBridge(module);

  const wire = {
    encode(value) {
      return input(encoder.encode(JSON.stringify(value)), (p, n) =>
        result(module._a11_wasm_wire_encode(p, n)));
    },
    decode(bytes) {
      return input(bytes, (p, n) => jsonResult(module._a11_wasm_wire_decode(p, n)));
    },
    decodeFragment(bytes) {
      return input(bytes, (p, n) => jsonResult(module._a11_wasm_fragment_decode(p, n)));
    },
  };

  const status = {
    pack(value) {
      return input(encoder.encode(JSON.stringify(value)), (p, n) =>
        result(module._a11_wasm_status_pack(p, n)));
    },
    unpack(bytes) {
      return input(bytes, (p, n) =>
        jsonResult(module._a11_wasm_status_unpack(p, n)));
    },
    chunk(value, closing = false) {
      return input(encoder.encode(JSON.stringify(value)), (p, n) =>
        result(module._a11_wasm_status_chunk(p, n, closing ? 1 : 0)));
    },
    fromChunk(bytes) {
      return input(bytes, (p, n) =>
        jsonResult(module._a11_wasm_status_from_chunk(p, n)));
    },
  };

  const data = {
    validateName(value) {
      return string(value, (p, n) =>
        result(module._a11_wasm_name_validate(p, n)));
    },
    encode(kind, value) {
      return string(kind, (kp, kn) =>
        input(encoder.encode(JSON.stringify(value)), (p, n) =>
          result(module._a11_wasm_data_encode(kp, kn, p, n))));
    },
    decode(kind, bytes) {
      return string(kind, (kp, kn) =>
        input(bytes, (p, n) =>
          jsonResult(module._a11_wasm_data_decode(kp, kn, p, n))));
    },
  };

  const schema = {
    validate(value) {
      return input(encoder.encode(JSON.stringify(value)), (p, n) =>
        result(module._a11_wasm_schema_validate(p, n)));
    },
    describe(value, runnable, allPorts) {
      return input(encoder.encode(JSON.stringify(value)), (p, n) =>
        jsonResult(module._a11_wasm_schema_describe(
          p, n, runnable ? 1 : 0, allPorts ? 1 : 0)));
    },
    parse(value) {
      return input(encoder.encode(JSON.stringify(value)), (p, n) =>
        jsonResult(module._a11_wasm_schema_parse(p, n)));
    },
    entries(value) {
      return input(encoder.encode(JSON.stringify(value)), (p, n) =>
        jsonResult(module._a11_wasm_schema_entries(p, n)));
    },
  };

  const streamReference = {
    toChunk(value) {
      return input(encoder.encode(JSON.stringify(value)), (p, n) =>
        result(module._a11_wasm_stream_reference_to_chunk(p, n)));
    },
    fromChunk(bytes) {
      return input(bytes, (p, n) =>
        jsonResult(module._a11_wasm_stream_reference_from_chunk(p, n)));
    },
  };

  const authorization = {
    encode(value) {
      return input(encoder.encode(JSON.stringify(value)), (p, n) =>
        result(module._a11_wasm_authorization_encode(p, n)));
    },
    decode(bytes) {
      return input(bytes, (p, n) =>
        jsonResult(module._a11_wasm_authorization_decode(p, n)));
    },
    toText(value) {
      return input(encoder.encode(JSON.stringify(value)), (p, n) =>
        decoder.decode(result(module._a11_wasm_authorization_to_text(p, n))));
    },
    fromText(value) {
      return string(value, (p, n) =>
        jsonResult(module._a11_wasm_authorization_from_text(p, n)));
    },
  };

  const serialization = {
    jsonToChunk(value, mimetype = '') {
      return string(mimetype, (mp, mn) =>
        input(encoder.encode(JSON.stringify(value)), (p, n) =>
          result(module._a11_wasm_json_to_chunk(p, n, mp, mn))));
    },
    jsonFromChunk(bytes, patterns = []) {
      return input(bytes, (p, n) =>
        input(encoder.encode(JSON.stringify(patterns)), (sp, sn) =>
          jsonResult(module._a11_wasm_json_from_chunk(p, n, sp, sn))));
    },
    bytesToChunk(bytes, mimetype) {
      return string(mimetype, (mp, mn) =>
        input(bytes, (p, n) =>
          result(module._a11_wasm_bytes_to_chunk(p, n, mp, mn))));
    },
    bytesFromChunk(bytes) {
      return input(bytes, (p, n) =>
        result(module._a11_wasm_bytes_from_chunk(p, n)));
    },
  };

  const framing = {
    validateOptions(options) {
      result(module._a11_wasm_frame_options_validate(options.packetSize,
        options.maxMessageSize, options.maxPendingMessages,
        options.maxPendingBytes));
    },
    parse(bytes) {
      const packed = input(bytes, (p, n) => result(
        module._a11_wasm_frame_parse(p, n)));
      const view = new DataView(packed.buffer, packed.byteOffset,
        packed.byteLength);
      return {
        type: view.getUint8(0),
        transientId: view.getBigUint64(PACKET_ID_OFFSET, true),
        sequence: view.getUint32(PACKET_SEQUENCE_OFFSET, true),
        packetCount: view.getUint32(PACKET_COUNT_OFFSET, true),
        payload: packed.subarray(PACKET_PAYLOAD_OFFSET),
      };
    },
    split(bytes, transientId, packetSize = 64 * 1024) {
      const packed = input(bytes, (p, n) => result(
        module._a11_wasm_frame_split(p, n, BigInt(transientId), packetSize)));
      const packets = [];
      const view = new DataView(packed.buffer, packed.byteOffset, packed.byteLength);
      for (let offset = 0; offset < packed.length;) {
        if (offset + 4 > packed.length) throw new Error("invalid packet list");
        const length = view.getUint32(offset, true);
        offset += 4;
        if (offset + length > packed.length) throw new Error("invalid packet length");
        packets.push(packed.subarray(offset, offset + length));
        offset += length;
      }
      return packets;
    },
    reassembler(options = {}) {
      const handle = module._a11_wasm_reassembler_new(
        options.packetSize ?? 64 * 1024,
        options.maxMessageSize ?? 16 * 1024 * 1024,
        options.maxPendingMessages ?? 32,
        options.maxPendingBytes ?? 32 * 1024 * 1024);
      if (!handle) throw new RangeError("invalid reassembler limits");
      let disposed = false;
      return {
        get pendingMessageCount() {
          if (disposed) throw new Error("reassembler is disposed");
          return module._a11_wasm_reassembler_pending_messages(handle);
        },
        get pendingByteCount() {
          if (disposed) throw new Error("reassembler is disposed");
          return module._a11_wasm_reassembler_pending_bytes(handle);
        },
        clear() {
          if (disposed) throw new Error("reassembler is disposed");
          module._a11_wasm_reassembler_clear(handle);
        },
        feed(packet) {
          if (disposed) throw new Error("reassembler is disposed");
          return input(packet, (p, n) => result(module._a11_wasm_reassembler_feed(handle, p, n)));
        },
        dispose() {
          if (!disposed) module._a11_wasm_reassembler_free(handle);
          disposed = true;
        },
      };
    },
  };

  function runtime(storageId = "local", options = {}) {
    const handle = string(storageId, (p, n) => module._a11_wasm_runtime_new(p, n));
    if (!handle) throw new Error("WASM runtime allocation failed");
    let disposed = false;
    const listeners = new Map();
    function live() {
      if (disposed) throw new Error("runtime is disposed");
    }
    function wake(id, error = null) {
      const waiting = listeners.get(id);
      if (!waiting) return;
      listeners.delete(id);
      for (const done of waiting) done(error);
    }
    function wakeAll(error = null) {
      for (const id of listeners.keys()) wake(id, error);
    }
    function waitForChange(id, signal, deadline) {
      return new Promise((resolve, reject) => {
        const end = deadline instanceof Date ? deadline.getTime() : (deadline ?? undefined);
        if (end !== undefined && (!Number.isFinite(end) || end < 0)) {
          reject(new RangeError("deadline must be an epoch millisecond value"));
          return;
        }
        const timeout = end === undefined ? null : Math.max(0, end - Date.now());
        if (signal?.aborted) {
          reject(new A11WasmError(StatusCode.CANCELLED, "node read cancelled"));
          return;
        }
        if (timeout === 0) {
          reject(new A11WasmError(StatusCode.DEADLINE_EXCEEDED, "node read deadline exceeded"));
          return;
        }
        const waiting = listeners.get(id) ?? new Set();
        listeners.set(id, waiting);
        let timer = null;
        let settled = false;
        const done = (error) => {
          if (settled) return;
          settled = true;
          waiting.delete(done);
          if (waiting.size === 0) listeners.delete(id);
          if (timer !== null) clearTimeout(timer);
          signal?.removeEventListener("abort", onAbort);
          if (error) reject(error);
          else resolve();
        };
        const onAbort = () => done(new A11WasmError(StatusCode.CANCELLED, "node read cancelled"));
        waiting.add(done);
        signal?.addEventListener("abort", onAbort, { once: true });
        if (timeout !== null) {
          timer = setTimeout(() => done(new A11WasmError(StatusCode.DEADLINE_EXCEEDED, "node read deadline exceeded")), timeout);
        }
      });
    }
    const api = {
      register(name) {
        live();
        string(name, (p, n) => result(module._a11_wasm_runtime_register(handle, p, n)));
      },
      invoke(action) {
        live();
        input(encoder.encode(JSON.stringify(action)), (p, n) =>
          result(module._a11_wasm_runtime_invoke(handle, p, n)));
        wakeAll();
      },
      nextAction() {
        live();
        return jsonResult(module._a11_wasm_runtime_next_action(handle));
      },
      finishAction(id, code = 0, message = "") {
        live();
        string(id, (ip, il) => string(message, (mp, ml) =>
          result(module._a11_wasm_runtime_finish_action(handle, ip, il, code, mp, ml))));
        wakeAll();
      },
      append(id, bytes, mimetype = "") {
        live();
        string(id, (ip, il) => input(bytes, (dp, dl) => string(mimetype, (mp, ml) =>
          result(module._a11_wasm_runtime_node_append(handle, ip, il, dp, dl, mp, ml)))));
        wake(id);
      },
      appendRef(id, ref) {
        live();
        string(id, (ip, il) => input(encoder.encode(JSON.stringify(ref)), (rp, rl) =>
          result(module._a11_wasm_runtime_node_append_ref(handle, ip, il, rp, rl))));
        wake(id);
      },
      putFragment(fragment) {
        live();
        const seq = Number(input(encoder.encode(JSON.stringify(fragment)), (p, n) =>
          decoder.decode(result(module._a11_wasm_runtime_node_put_fragment(handle, p, n)))));
        wake(fragment.id);
        return seq;
      },
      putMany(fragments) {
        live();
        const seqs = input(encoder.encode(JSON.stringify(fragments)), (p, n) =>
          jsonResult(module._a11_wasm_runtime_node_put_many(handle, p, n)));
        for (const fragment of fragments) wake(fragment.id);
        return seqs;
      },
      close(id, code = 0, message = "") {
        live();
        string(id, (ip, il) => string(message, (mp, ml) =>
          result(module._a11_wasm_runtime_node_close(handle, ip, il, code, mp, ml))));
        wake(id);
      },
      readBytes(id, index) {
        live();
        return string(id, (p, n) => result(module._a11_wasm_runtime_node_read(handle, p, n, index)));
      },
      readData(id, index) {
        live();
        return string(id, (p, n) => result(
          module._a11_wasm_runtime_node_read_data(handle, p, n, index)));
      },
      read(id, index) {
        return wire.decodeFragment(api.readBytes(id, index));
      },
      readByArrival(id, arrival) {
        live();
        const bytes = string(id, (p, n) => result(
          module._a11_wasm_runtime_node_read_arrival(handle, p, n, arrival)));
        return wire.decodeFragment(bytes);
      },
      clearData(id, seq) {
        live();
        const bytes = string(id, (p, n) => result(
          module._a11_wasm_runtime_node_clear_data(handle, p, n, seq)));
        return wire.decodeFragment(bytes);
      },
      arrivalSeq(id, arrival) {
        live();
        return Number(string(id, (p, n) => decoder.decode(result(
          module._a11_wasm_runtime_node_arrival_seq(handle, p, n, arrival)))));
      },
      finalSeq(id) {
        live();
        return JSON.parse(string(id, (p, n) => decoder.decode(result(
          module._a11_wasm_runtime_node_final_seq(handle, p, n)))));
      },
      async waitForFragment(id, index, wait = {}) {
        live();
        for (;;) {
          if (wait.signal?.aborted) throw new A11WasmError(StatusCode.CANCELLED, "node read cancelled");
          try {
            return api.read(id, index);
          } catch (error) {
            if (!(error instanceof A11WasmError) || (error.code !== 5 && error.code !== 14)) {
              throw error;
            }
          }
          await waitForChange(id, wait.signal, wait.deadline);
        }
      },
      waitForChange(id, wait = {}) {
        live();
        return waitForChange(id, wait.signal, wait.deadline);
      },
      async *stream(id, wait = {}) {
        let index = wait.from ?? 0;
        for (;;) {
          try {
            yield await api.waitForFragment(id, index++, wait);
          } catch (error) {
            if (error instanceof A11WasmError && error.code === 11) return;
            throw error;
          }
        }
      },
      count(id) {
        live();
        return Number(string(id, (p, n) => decoder.decode(
          result(module._a11_wasm_runtime_node_count(handle, p, n)))));
      },
      closed(id) {
        live();
        return string(id, (p, n) => decoder.decode(
          result(module._a11_wasm_runtime_node_closed(handle, p, n)))) === "true";
      },
      resolve(ref, limits = {}) {
        live();
        const bytes = input(encoder.encode(JSON.stringify(ref)), (p, n) =>
          result(module._a11_wasm_runtime_resolve(handle, p, n,
            limits.maxDepth ?? 8, limits.maxChunks ?? 1024)));
        return wire.decode(bytes).node_fragments ?? [];
      },
      async resolveAsync(ref, limits = {}) {
        if (ref.storage && ref.storage.id !== storageId) {
          if (!options.resolveStorage) {
            throw new A11WasmError(StatusCode.UNAVAILABLE, "reference storage needs a host resolver");
          }
          return options.resolveStorage(ref.storage, ref, limits);
        }
        return api.resolve(ref, limits);
      },
      exportSnapshot() {
        live();
        return result(module._a11_wasm_runtime_export_snapshot(handle));
      },
      importSnapshot(bytes) {
        live();
        input(bytes, (p, n) => result(module._a11_wasm_runtime_import_snapshot(handle, p, n)));
        wakeAll();
      },
      async runPending(handlers) {
        live();
        for (;;) {
          const action = api.nextAction();
          if (action === null) return;
          const handler = handlers[action.name];
          if (!handler) {
            api.finishAction(action.id, 5, `action ${action.name} has no handler`);
            continue;
          }
          try {
            await handler(action, api);
            api.finishAction(action.id);
          } catch (error) {
            api.finishAction(action.id, 13, String(error));
            throw error;
          }
        }
      },
      dispose() {
        wakeAll(new A11WasmError(StatusCode.CANCELLED, "runtime disposed"));
        if (!disposed) module._a11_wasm_runtime_free(handle);
        disposed = true;
      },
    };
    return api;
  }

  return {
    flow(request) {
      return input(encoder.encode(JSON.stringify(request)), (p, n) =>
        jsonResult(module._a11_wasm_flow_request(p, n)));
    },
    wire,
    data,
    schema,
    streamReference,
    authorization,
    serialization,
    status,
    framing,
    runtime,
    async openPersistentRuntime(adapter, storageId = "local", options = {}) {
      const rt = runtime(storageId, options);
      try {
        const saved = await adapter.load();
        if (saved !== null) rt.importSnapshot(saved);
      } catch (error) {
        rt.dispose();
        throw error;
      }
      let saveQueue = Promise.resolve();
      const save = () => {
        const snapshot = rt.exportSnapshot();
        saveQueue = saveQueue.catch(() => {}).then(() => adapter.save(snapshot));
        return saveQueue;
      };
      Object.assign(rt, {
        flush: save,
        async appendDurable(id, bytes, mimetype = "") {
          rt.append(id, bytes, mimetype);
          await save();
        },
        async appendRefDurable(id, ref) {
          rt.appendRef(id, ref);
          await save();
        },
        async closeDurable(id, code = 0, message = "") {
          rt.close(id, code, message);
          await save();
        },
        async finishActionDurable(id, code = 0, message = "") {
          rt.finishAction(id, code, message);
          await save();
        },
        async disposeDurable() {
          await saveQueue;
          rt.dispose();
        },
      });
      return rt;
    },
  };
}
