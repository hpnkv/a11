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

import {rmSync} from 'node:fs';

const obsolete = ['legacy', 'action', 'action_builtins', 'action_registry',
  'async_node', 'chunk_store', 'chunk_store_reader', 'chunk_store_writer',
  'reference_mirror', 'session'];
for (const name of obsolete) {
  for (const suffix of ['.js', '.js.map', '.d.ts', '.d.ts.map']) {
    rmSync(new URL(`../dist/${name}${suffix}`, import.meta.url), {force: true});
  }
}
