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

import { createHash } from "node:crypto";
import { cp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { resolve, sep } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = new URL("./", import.meta.url);
const build = pathToFileURL(resolve(process.argv[2] ??
  fileURLToPath(new URL("./build/", root))) + sep);
const nativeBuild = pathToFileURL(resolve(process.argv[3] ??
  fileURLToPath(new URL("./build/threaded/", root))) + sep);
const output = new URL("./build/dist/", root);
const js = await readFile(new URL("a11.mjs", build));
const wasm = await readFile(new URL("a11.wasm", build));
const name = (kind, bytes) => {
  const hash = createHash("sha256").update(bytes).digest("hex");
  return { file: `a11-${hash.slice(0, 20)}.${kind}`, sha256: hash,
    bytes: bytes.length };
};
const assets = { module: name("mjs", js), wasm: name("wasm", wasm) };
let native = null;
try {
  let nativeJs = (await readFile(new URL("a11-native.mjs", nativeBuild))).toString();
  const workerPath = /new Worker\(new URL\("a11-native\.mjs",\s*import\.meta\.url\)/;
  if (!workerPath.test(nativeJs)) throw new Error("native pthread worker path changed");
  nativeJs = nativeJs.replace(workerPath,
    'new Worker(new URL(import.meta.url)');
  const nativeWasm = await readFile(new URL("a11-native.wasm", nativeBuild));
  const nativeModule = name("mjs", Buffer.from(nativeJs));
  const nativeBinary = name("wasm", nativeWasm);
  native = { module: nativeModule, wasm: nativeBinary,
    js: Buffer.from(nativeJs), binary: nativeWasm };
  assets.native = { module: nativeModule, wasm: nativeBinary };
} catch (error) {
  if (error?.code !== "ENOENT") throw error;
}

await rm(output, { recursive: true, force: true });
await mkdir(output, { recursive: true });
await writeFile(new URL(assets.module.file, output), js);
await writeFile(new URL(assets.wasm.file, output), wasm);
if (native) {
  await writeFile(new URL(native.module.file, output), native.js);
  await writeFile(new URL(native.wasm.file, output), native.binary);
}
for (const file of ["index.mjs", "bridge.mjs", "status.mjs", "index.d.ts",
                    "persistence-node.mjs", "persistence-node.d.mts",
                    "persistence-browser.mjs", "persistence-browser.d.mts",
                    "package.json"]) {
  const destination = file === "index.mjs" ? "runtime.mjs" : file;
  await cp(new URL(file, root), new URL(destination, output));
}
if (native) {
  for (const file of ["native-index.mjs", "native-index.d.mts",
                      "native-node.mjs", "native-store.mjs",
                      "native-action.mjs", "native-session.mjs",
                      "snapshot-host.mjs",
                      "native-http-actions.mjs",
                      "native_worker.mjs", "http-client.mjs",
                      "http-client.d.mts"]) {
    await cp(new URL(file, root), new URL(file, output));
  }
  await writeFile(new URL("native.mjs", output),
    `import { createA11WasmRuntime as createRuntime } from "./native-index.mjs";\n` +
    `export { createHttpClient } from "./http-client.mjs";\n` +
    `export function createA11WasmRuntime(options = {}) {\n` +
    `  return createRuntime({ workerUrl: new URL("./native_worker.mjs", import.meta.url),\n` +
    `    moduleUrl: new URL("./${native.module.file}", import.meta.url),\n` +
    `    wasmUrl: new URL("./${native.wasm.file}", import.meta.url), ...options });\n}\n`);
}
const wrapper = `import { createA11Wasm as createRuntime } from "./runtime.mjs";
export { A11WasmError } from "./runtime.mjs";
export async function createA11Wasm(options = {}) {
  const factory = options.factory ??
    (await import("./${assets.module.file}")).default;
  const locateFile = options.locateFile ??
    (name => name.endsWith(".wasm")
      ? new URL("./${assets.wasm.file}", import.meta.url).href : name);
  return createRuntime({ ...options, factory, locateFile });
}
`;
await writeFile(new URL("index.mjs", output), wrapper);
await writeFile(new URL("asset-manifest.json", output),
  JSON.stringify(assets, null, 2) + "\n");
await writeFile(new URL("_headers", output),
  `/${assets.module.file}\n  Cache-Control: public, max-age=31536000, immutable\n` +
  `/${assets.wasm.file}\n  Cache-Control: public, max-age=31536000, immutable\n` +
  `/index.mjs\n  Cache-Control: no-cache\n` +
  `/runtime.mjs\n  Cache-Control: no-cache\n` +
  `/bridge.mjs\n  Cache-Control: no-cache\n` +
  (native ? `/${native.module.file}\n  Cache-Control: public, max-age=31536000, immutable\n` +
    `/${native.wasm.file}\n  Cache-Control: public, max-age=31536000, immutable\n` +
    `/native.mjs\n  Cache-Control: no-cache\n` +
    `/native-index.mjs\n  Cache-Control: no-cache\n` +
    `/native_worker.mjs\n  Cache-Control: no-cache\n` : ""));
process.stdout.write(`${fileURLToPath(output)}\n`);
