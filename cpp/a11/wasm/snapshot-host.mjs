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

const CHUNK_BYTES = 3 * 8192;

export function encodeSnapshot(bytes) {
  if (!(bytes instanceof Uint8Array)) {
    throw new TypeError('Snapshot load must return Uint8Array or null');
  }
  if (typeof Buffer !== 'undefined') return Buffer.from(bytes).toString('base64');
  const chunks = [];
  for (let offset = 0; offset < bytes.length; offset += CHUNK_BYTES) {
    chunks.push(btoa(String.fromCharCode(
      ...bytes.subarray(offset, offset + CHUNK_BYTES))));
  }
  return chunks.join('');
}

export function decodeSnapshot(encoded) {
  if (typeof encoded !== 'string') throw new TypeError('Snapshot is not base64');
  if (typeof Buffer !== 'undefined') return new Uint8Array(Buffer.from(encoded,
    'base64'));
  const padding = encoded.endsWith('==') ? 2 : encoded.endsWith('=') ? 1 : 0;
  const output = new Uint8Array((encoded.length / 4) * 3 - padding);
  let offset = 0;
  for (let index = 0; index < encoded.length; index += (CHUNK_BYTES / 3) * 4) {
    const binary = atob(encoded.slice(index,
      index + (CHUNK_BYTES / 3) * 4));
    for (let byte = 0; byte < binary.length; byte++) {
      output[offset++] = binary.charCodeAt(byte);
    }
  }
  return output;
}
