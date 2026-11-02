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

import {writeFile} from 'node:fs/promises';
import {relative, resolve} from 'node:path';
import ts from 'typescript';

const root = resolve(import.meta.dirname, '..');
const configPath = ts.findConfigFile(root, ts.sys.fileExists, 'tsconfig.json');
const config = ts.readConfigFile(configPath, ts.sys.readFile);
if (config.error) throw new Error(ts.flattenDiagnosticMessageText(config.error.messageText, '\n'));
const parsed = ts.parseJsonConfigFileContent(config.config, ts.sys, root);
const entries = {
  '.': 'src/index.ts',
  './runtime': 'src/runtime.ts',
  './runtime/llm': 'src/runtime_llm.ts',
  './presentation': 'src/presentation/index.ts',
  './presentation/browser': 'src/presentation/browser/index.ts',
};
const nativeWorker = new Set([
  'runtime_backend.ts', 'runtime_graph.ts', 'runtime_registry.ts',
  'runtime_flow.ts', 'worker_chunk_store.ts', 'flow_bridge.ts',
  'channel_wire_stream.ts',
]);
const nativeAfterInit = new Set([
  'data.ts', 'byte_chunking.ts', 'status_codec.ts', 'action_schema.ts',
  'authorization.ts', 'stream_reference.ts', 'wire_bridge.ts',
  'flow_highlight.ts', 'schema_json.ts',
]);
const tsProtocol = new Set([
  'serialization.ts', 'wire_values.ts',
  'wire_stream.ts', 'wire_stream_bridge.ts', 'stream_mode.ts',
  'action_names.ts', 'action_log.ts', 'sdk/usage.ts',
  'sdk/tool_adapter.ts', 'sdk/tool_runner.ts',
]);
const host = new Set([
  'bytes.ts', 'status.ts', 'status_bridge.ts', 'concurrency.ts',
  'chunk_clone.ts', 'in_process_wire_stream.ts',
  'websocket_wire_stream.ts', 'webrtc_wire_stream.ts',
  'http_sse_wire_stream.ts', 'signalling.ts', 'runtime_options.ts',
  'runtime_llm_policy.ts', 'runtime_tool_runner.ts',
  'runtime_authorization.ts',
  'sdk/llm.ts', 'sdk/interact_with_llm.ts',
  'sdk/config_schemas.ts', 'sdk/jsonschema.ts',
  'sdk/presentation.ts', 'sdk/gemma/interact_with_gemma.ts',
]);
function classify(source) {
  const name = source.replace(/^src\//, '');
  if (nativeWorker.has(name)) return 'native-worker';
  if (nativeAfterInit.has(name)) return 'native-after-init';
  if (tsProtocol.has(name)) return 'typescript-protocol';
  if (host.has(name) || name.startsWith('presentation/')) return 'host';
  if (name === 'runtime_usage.ts') return 'native-worker-facade';
  if (name.startsWith('native_')) return 'internal-bridge';
  if (name === 'flow_vocabulary.ts') return 'generated-native-data';
  if (['serial_tags.ts', 'sdk/index.ts', 'index.ts', 'runtime.ts',
       'runtime_llm.ts', 'action_schema_bridge.ts', 'fragment_bridge.ts'].includes(name)) {
    return 'value-conversion';
  }
  throw new Error(`Public source is unclassified: ${source}`);
}
const program = ts.createProgram(parsed.fileNames, parsed.options);
const checker = program.getTypeChecker();
const rows = [];
for (const [entry, path] of Object.entries(entries)) {
  const file = program.getSourceFile(resolve(root, path));
  if (!file) throw new Error(`Missing export source ${path}`);
  const module = checker.getSymbolAtLocation(file);
  for (const exported of checker.getExportsOfModule(module)) {
    const actual = exported.flags & ts.SymbolFlags.Alias
      ? checker.getAliasedSymbol(exported) : exported;
    const declaration = actual.declarations?.[0] ?? exported.declarations?.[0];
    const source = declaration
      ? relative(root, declaration.getSourceFile().fileName).replaceAll('\\', '/')
      : path;
    rows.push({entry, name: exported.name, source,
      route: (actual.flags & ts.SymbolFlags.Value)
        ? classify(source) : 'type-only'});
  }
}
rows.sort((a, b) => a.entry.localeCompare(b.entry) ||
  a.name.localeCompare(b.name));
const destination = resolve(root, 'WASM_EXPORT_AUDIT.json');
await writeFile(destination, `${JSON.stringify({format: 'a11.wasm-export-audit/v1',
  exportedSymbols: rows.length, exports: rows}, null, 2)}\n`);
console.log(`${destination}: ${rows.length} export paths`);
