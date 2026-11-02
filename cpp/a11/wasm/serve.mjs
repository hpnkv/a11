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
import { readFile } from "node:fs/promises";
import { createServer } from "node:http";
import { resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(fileURLToPath(new URL("./", import.meta.url)));
const port = Number(process.argv[2] ?? 8732);
const types = new Map([
  [".html", "text/html; charset=utf-8"],
  [".mjs", "text/javascript; charset=utf-8"],
  [".js", "text/javascript; charset=utf-8"],
  [".json", "application/json"],
  [".wasm", "application/wasm"],
]);

createServer(async (request, response) => {
  const headers = {
    "Cross-Origin-Resource-Policy": "same-origin",
  };
  if (!request.url?.startsWith('/guides/')) {
    headers['Cross-Origin-Opener-Policy'] = 'same-origin';
    headers['Cross-Origin-Embedder-Policy'] = 'require-corp';
  }
  if (request.url?.startsWith('/__http_test/')) {
    headers['Content-Type'] = 'text/plain; charset=utf-8';
    if (request.url === '/__http_test/echo' && request.method === 'POST') {
      response.writeHead(200, headers);
      for await (const chunk of request) response.write(chunk);
      response.end();
      return;
    }
    if (request.url === '/__http_test/redirect') {
      response.writeHead(302, {...headers,
        Location: '/__http_test/stream'}).end();
      return;
    }
    if (request.url === '/__http_test/status') {
      response.writeHead(418, headers).end('teapot');
      return;
    }
    if (request.url === '/__http_test/stream') {
      response.writeHead(200, headers);
      response.write('hello');
      setTimeout(() => response.end(' world'), 20);
      return;
    }
    response.writeHead(404, headers).end();
    return;
  }
  if (request.method !== "GET" && request.method !== "HEAD") {
    response.writeHead(405, headers).end();
    return;
  }
  let path;
  try {
    const pathname = new URL(request.url, "http://localhost").pathname;
    if (pathname === '/guides/isolation-test/') {
      path = resolve(root, 'test-guide-isolation.html');
    } else if (pathname === '/guides/wasm-isolation-sw.js') {
      path = resolve(root, '../../../doc/docs/guides/wasm-isolation-sw.js');
    } else if (pathname === '/assets/guide-isolation-test.mjs') {
      path = resolve(root, 'build/guide-isolation-test.mjs');
    } else if (pathname.startsWith('/assets/wasm/')) {
      path = resolve(root, '../../../js/dist/wasm',
        decodeURIComponent(pathname.slice('/assets/wasm/'.length)));
    } else {
      path = resolve(root, "." + decodeURIComponent(pathname));
    }
  } catch {
    response.writeHead(400, headers).end();
    return;
  }
  const docGuide = resolve(root, '../../../doc/docs/guides');
  const packageAssets = resolve(root, '../../../js/dist/wasm');
  if (![root, docGuide, packageAssets].some(base =>
    path.startsWith(base + sep))) {
    response.writeHead(403, headers).end();
    return;
  }
  try {
    const bytes = await readFile(path);
    const extension = path.slice(path.lastIndexOf("."));
    const etag = `"${createHash("sha256").update(bytes).digest("hex")}"`;
    headers["Content-Type"] = types.get(extension) ?? "application/octet-stream";
    headers["Cache-Control"] = /\/a11-[0-9a-f]{20}\.(mjs|wasm)$/.test(path)
      ? "public, max-age=31536000, immutable" : "no-cache";
    headers.ETag = etag;
    if (request.headers["if-none-match"] === etag) {
      response.writeHead(304, headers).end();
      return;
    }
    response.writeHead(200, headers).end(request.method === "HEAD" ? null : bytes);
  } catch {
    response.writeHead(404, headers).end();
  }
}).listen(port, "127.0.0.1", () => {
  process.stdout.write(`A11 WASM browser server: http://127.0.0.1:${port}/\n`);
});
