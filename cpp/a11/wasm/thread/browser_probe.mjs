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

import createProbe from "../build/threaded/a11_wasm_thread_probe.mjs";

try {
  await createProbe({ locateFile: name =>
    new URL(`../build/threaded/${name}`, import.meta.url).href });
  postMessage({ ok: true });
} catch (error) {
  postMessage({ ok: false, error: String(error?.stack ?? error) });
}
