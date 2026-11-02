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

#include <cstddef>

#include <emscripten.h>

#include "openssl/rand.h"
#include "openssl/sha.h"

// The host owns both algorithms. Copy the input before awaiting the digest,
// since WASM memory may grow while the host crypto operation is pending.
EM_ASYNC_JS(int, HostSha256,
            (const unsigned char* data, size_t length, unsigned char* digest), {
  try {
    if (!globalThis.crypto?.subtle) return 0;
    const input = HEAPU8.slice(data, data + length);
    const output = await globalThis.crypto.subtle.digest("SHA-256", input);
    HEAPU8.set(new Uint8Array(output), digest);
    return 1;
  } catch { return 0; }
});

EM_JS(int, HostRandom, (unsigned char* data, int count), {
  try {
    if (!globalThis.crypto?.getRandomValues) return 0;
    for (let offset = 0; offset < count; offset += 65536) {
      const size = Math.min(65536, count - offset);
      const output = globalThis.crypto.getRandomValues(new Uint8Array(size));
      HEAPU8.set(output, data + offset);
    }
    return 1;
  } catch { return 0; }
});

extern "C" unsigned char* SHA256(const unsigned char* data, size_t length,
                                 unsigned char* digest) {
  if (digest == nullptr || (data == nullptr && length != 0)) return nullptr;
  return HostSha256(data, length, digest) == 1 ? digest : nullptr;
}

extern "C" int RAND_bytes(unsigned char* data, int count) {
  if (count < 0 || (data == nullptr && count != 0)) return 0;
  return HostRandom(data, count);
}
