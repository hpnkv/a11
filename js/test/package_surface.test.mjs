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

import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {existsSync, mkdtempSync, rmSync, symlinkSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import test from 'node:test';
import {fileURLToPath, pathToFileURL} from 'node:url';

const root = fileURLToPath(new URL('..', import.meta.url));
const oldRuntime = ['legacy', 'action', 'action_registry',
  'action_builtins', 'async_node', 'chunk_store', 'chunk_store_reader',
  'chunk_store_writer', 'reference_mirror', 'session'];

test('npm package loads one WASM runtime without legacy modules',
  {skip: !existsSync(join(root, 'dist/wasm/native.mjs'))}, async () => {
    const directory = mkdtempSync(join(tmpdir(), 'a11-package-'));
    try {
      const packed = spawnSync('npm', ['pack', '--json', '--pack-destination',
        directory], {cwd: root, encoding: 'utf8'});
      assert.equal(packed.status, 0, packed.stderr);
      const [archive] = JSON.parse(packed.stdout);
      const files = new Set(archive.files.map(file => file.path));
      for (const name of oldRuntime) {
        for (const suffix of ['.js', '.d.ts']) {
          assert.equal(files.has(`dist/${name}${suffix}`), false,
            `${name} must not ship as a TypeScript runtime`);
        }
      }
      for (const name of ['index.js', 'runtime.js', 'runtime_graph.js',
        'worker_chunk_store.js', 'wasm/native.mjs']) {
        assert.equal(files.has(`dist/${name}`), true, `${name} is missing`);
      }
      const extracted = spawnSync('tar', ['-xzf',
        join(directory, archive.filename), '-C', directory],
      {encoding: 'utf8'});
      assert.equal(extracted.status, 0, extracted.stderr);
      const packageRoot = join(directory, 'package');
      symlinkSync(join(root, 'node_modules'),
        join(packageRoot, 'node_modules'), 'dir');
      const sdk = await import(pathToFileURL(join(packageRoot,
        'dist/index.js')).href);
      const runtime = await sdk.createA11Runtime();
      assert.equal(sdk.isOk(runtime), true);
      try {
        const store = sdk.LocalChunkStore.create('package-smoke', {runtime});
        assert.equal(sdk.isOk(store), true);
        assert.equal(runtime.backendOf(store), 'wasm');
        const node = await sdk.AsyncNode.create('package-node', {runtime});
        assert.ok(node instanceof sdk.AsyncNode);
        assert.equal(sdk.isOk(await node.finalize('from WASM')), true);
        assert.equal(await node.next(1000), 'from WASM');
        const action = await sdk.Action.create(new sdk.ActionSchema({
          name: 'package-action'}), {runtime});
        assert.ok(action instanceof sdk.Action);
        const session = await sdk.Session.create(runtime);
        assert.ok(session instanceof sdk.Session);
        const flow = await new sdk.FlowProgram(runtime,
          'flow packaged { out result: string\n "native" -> result\n }',
          'packaged').run();
        assert.equal(sdk.isOk(flow), true);
        assert.equal(flow.result.length, 1);
      } finally {
        assert.equal(sdk.isOk(await runtime.dispose()), true);
      }
    } finally {
      rmSync(directory, {recursive: true, force: true});
    }
  });
