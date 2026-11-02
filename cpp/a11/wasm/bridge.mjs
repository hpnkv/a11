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

const encoder = new TextEncoder();
const decoder = new TextDecoder();

import {A11WasmError, normalizeStatus} from './status.mjs';

export {A11WasmError} from './status.mjs';

export function createBridge(module) {
  function input(bytes, use) {
    if (!(bytes instanceof Uint8Array)) throw new TypeError("expected Uint8Array");
    const pointer = module._malloc(Math.max(1, bytes.length));
    if (!pointer) throw new Error("WASM allocation failed");
    try {
      module.HEAPU8.set(bytes, pointer);
      return use(pointer, bytes.length);
    } finally {
      module._free(pointer);
    }
  }

  function string(value, use) {
    return input(encoder.encode(value), use);
  }

  async function asyncInput(bytes, use) {
    if (!(bytes instanceof Uint8Array)) throw new TypeError("expected Uint8Array");
    const pointer = module._malloc(Math.max(1, bytes.length));
    if (!pointer) throw new Error("WASM allocation failed");
    try {
      module.HEAPU8.set(bytes, pointer);
      return await use(pointer, bytes.length);
    } finally {
      module._free(pointer);
    }
  }

  function result(pointer) {
    if (!pointer) throw new Error("WASM result allocation failed");
    try {
      const code = module._a11_wasm_result_code(pointer);
      const ready = !!module._a11_wasm_result_ready(pointer);
      const address = module._a11_wasm_result_data(pointer);
      const length = module._a11_wasm_result_size(pointer);
      const bytes = module.HEAPU8.slice(address, address + length);
      if (code) {
        const message = decoder.decode(bytes);
        let status;
        try { status = normalizeStatus(JSON.parse(message), message); }
        catch { status = {code, message, details: []}; }
        throw new A11WasmError(status);
      }
      return ready ? bytes : null;
    } finally {
      module._a11_wasm_result_free(pointer);
    }
  }

  function jsonResult(pointer) {
    return JSON.parse(decoder.decode(result(pointer)));
  }

  return { input, asyncInput, string, result, jsonResult, encoder, decoder };
}
