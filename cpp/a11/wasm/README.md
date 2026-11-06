# A11 WebAssembly module

Build the module with Emscripten and load the same ES module in Node or a
browser:

```sh
emcmake cmake -S cpp/a11/wasm -B cpp/a11/wasm/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/a11/wasm/build -j 8
node cpp/a11/wasm/test.mjs
```

`index.mjs` loads `build/a11.mjs` and its adjacent `a11.wasm`. The caller can
provide a module factory or Emscripten `locateFile` function when bundling.
Serve `cpp/a11/wasm/` over HTTP to run `test-browser.html`.

Release builds use `-Oz` and omit Emscripten's virtual filesystem. The public
module starts small, grows as needed, and defaults to a 512 MiB maximum;
`A11_WASM_MAXIMUM_MEMORY_MB` sets the cap when configuring CMake. The framing
API defaults to a 16 MiB message, 32 pending messages, and 32 MiB pending
bytes; callers can set larger limits for workloads that need them.
Package a built module with `node cpp/a11/wasm/package.mjs`. It writes
`build/dist/` with SHA-256-named `.mjs` and `.wasm` assets and a small stable
`index.mjs` loader. Serve the stable loader with revalidation and the hashed
assets with `Cache-Control: public, max-age=31536000, immutable`; the generated
`_headers` file supplies these rules on hosts that support that format. The
browser reuses unchanged binaries across deployments because their URLs stay
the same. Keep old hashed files available while pages using an older loader
remain open.

```js
import { createA11Wasm } from "./cpp/a11/wasm/index.mjs";

const a11 = await createA11Wasm();
const diagnostics = a11.flow({ method: "check", source: "flow main { }" });
const runtime = a11.runtime("local");
runtime.register("echo");
runtime.invoke({ id: "call1", name: "echo", outputs: [{ name: "out", id: "reply" }] });
await runtime.runPending({
  echo: async (_action, rt) => rt.append("reply", new TextEncoder().encode("ok")),
});
const bytes = runtime.readData("reply", 0);
runtime.dispose();
```

The module compiles the existing native Flow service, A11 data records,
MessagePack/JSON codecs, and byte packetisation/reassembly. Binary payloads
cross the JS/WASM boundary as `Uint8Array`; JSON is used at the public wire and
Flow request boundaries. `readData()` returns an inline chunk's raw bytes.
`readBytes()` returns a native MessagePack fragment, and `read()` decodes it
through the native fragment codec.

The default module keeps nodes in memory, validates fragments and action
messages with native A11 records, resolves local `NodeRef` chains, and queues
actions for asynchronous JavaScript handlers. JavaScript owns the event loop:
`readAsync()` and `stream()` wait for writes and support cancellation and
deadlines. A host supplies durable snapshots through `fileSnapshotStore()` or
`indexedDbSnapshotStore()`. A storage resolver can open references to another
namespace. This host-driven runtime does not yet execute Flow programs; the
Flow tooling API uses the native parser, formatter, and service.

`bridge.mjs`, `status.mjs`, and `bridge_core.{h,cc}` own memory, result, and
status transfer. Node, store, action, session, Flow, and HTTP wrappers live in
separate modules. The runtime owns the worker and routes commands to them. The
TypeScript SDK keeps wire conversion in `wire_bridge.ts`, Flow tooling and
host action adaptation in `flow_bridge.ts`, and store adaptation in
`worker_chunk_store.ts` and `fragment_bridge.ts`.

## Native Thread path

The optional `A11_WASM_BUILD_BOOST_PROBE` build compiles A11's existing
Boost.Context, Boost.Fiber, Thread, `LocalChunkStore`, and `AsyncNode` code.
`thread/ucontext_compat.cc` maps Boost.Context's ucontext calls to Emscripten
fibers. Each Boost-owned stack holds both a C stack and an Asyncify stack.
Emscripten fibers cannot migrate between workers, so the probes set
`A11_POOL_THREADS=1` and disable pool spinning. Browser pthread builds also
need `SharedArrayBuffer` and cross-origin isolation (COOP/COEP); the default
module remains a separate single-agent build. Run `node cpp/a11/wasm/serve.mjs`
and open `test-thread-browser.html` to exercise the Thread probe in a Web
Worker with those headers. The local server also sends ETags and immutable
cache headers for packaged binary assets.

The native store, node, action, session, and Flow APIs run in the worker-bound
module. Browser or Node cryptographic APIs provide secure entropy and SHA-256.
Native node handles accept and return typed fragment JSON at their boundary;
`putFragment` and `nextFragment` preserve both local and storage-qualified
`NodeRef` values. Their offsets and lengths count fragments.
HTTP/WebRTC servers, Redis, and SQLite are outside this build.
Client nodes, actions, and Flow do not require libuv; its native users are the
HTTP transport, Redis client, and filesystem-driven Flow actions.

Build the worker-bound Flow module in a second tree. The Boost include path
must contain the headers matching the Context and Fiber source directories.

```sh
emcmake cmake -S cpp/a11/wasm -B cpp/a11/wasm/build/threaded -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DA11_WASM_BUILD_BOOST_PROBE=ON \
  -DA11_WASM_BUILD_NATIVE_RUNTIME=ON \
  -DA11_WASM_BOOST_INCLUDE_DIR=/path/to/boost/include \
  -DA11_WASM_BOOST_CONTEXT_SOURCE=/path/to/boost-context \
  -DA11_WASM_BOOST_FIBER_SOURCE=/path/to/boost-fiber
cmake --build cpp/a11/wasm/build/threaded --target a11_wasm_native -j 8
node cpp/a11/wasm/package.mjs
```

`package.mjs` includes `native.mjs` when the native module exists in
`build/threaded`. That loader creates one Web Worker for native Flow execution;
the worker runs concurrent Flow tasks on A11 fibers pinned to one OS worker.
Each run keeps its own cancellation signal and host-action deadline. Browser
hosts serve the package from the same origin with COOP/COEP. Node uses
`worker_threads`.
Native Flow inputs and outputs are chunk JSON values. `runFlow` returns the
collected outputs, and its `onOutput(port, chunk)` option reports each chunk
while the Flow runs. Register a host action before running Flow to let native
Flow call a TypeScript handler. The handler receives a native action handle;
its input and output nodes access the same native node map as the Flow run.

```js
import {createA11WasmRuntime} from './cpp/a11/wasm/native-index.mjs';

const runtime = await createA11WasmRuntime();
runtime.registerAction({
  name: 'echo',
  inputs: [{name: 'text', type: 'string', required: true, unary: true}],
  outputs: [{name: 'result', type: 'string', unary: true}],
}, async action => {
  const input = await action.input('text');
  const output = await action.output('result');
  const chunk = await input.nextChunk();
  await output.putChunk(chunk);
  await output.finalize();
});
```

`registerAction` makes a host handler callable from Flow. Await
`registerRunnableAction` to install the same handler in the worker's C++
`ActionRegistry` for calls made with `runtime.action()` or
`runtime.session().action()`. The returned function unregisters it.

`runtime.session(id).addStream(bridgeWireStream(stream))` attaches a TypeScript
`WireStream` to a native Session. The bridge keeps the C++ session and node map
as the owners of session state and moves wire messages through a paired native
in-process stream. The TypeScript stream may be a WebSocket, SSE, WebRTC, or
in-process endpoint. The bridge releases its native pair at stream completion;
`runtime.detachWireStream(stream.getId())` releases it earlier.
`test-sdk-host-stream.mjs` checks delivery in both
directions with the SDK's in-process transport. An action created from a
session uses its sole attached stream for input mirroring. Pass a `streamId`
to `session.action(name, id, streamId)` when several streams are attached.
`test-sdk-host-action-call.mjs` runs a native caller against a TypeScript
server action through this bridge.
`runtime.session(id, options)` accepts native session limits in camel case.
Pass `{withRecv: true}` to create C++ `SessionWithRecv`, then call
`receiveWithStreamId({timeoutMs, signal})` to read inbound messages.
The worker defaults to four root and sixteen nested actions, 64 queued
messages, and 8 MiB of aggregate buffered message data. Hosts can raise these
limits for server workloads. `noStreamTimeoutMs: null` disables the idle
timeout; `deadlineMs` is an absolute Unix millisecond timestamp.

The worker can persist its native node graph through a host adapter:

```js
const runtime = await createA11WasmRuntime({
  storageId: 'browser-local',
  persistence: {
    load: async () => await readSnapshot(),
    save: async bytes => await writeSnapshot(bytes),
  },
});
await runtime.node('events').putChunk(chunk);
await runtime.node('events').finalize();
await runtime.flushPersistence();
runtime.dispose();
```

`load` returns `Uint8Array` or `null`. Restore completes before the factory
returns. Flush after producers finish; a running action, session, or Flow run
prevents export. Snapshots include native fragments, close statuses, and
storage-qualified references. The adapter has one writer per storage namespace.
The same worker owns stores made through `runtime.store()` and nodes made
through `runtime.node()`.
If a worker terminates, its runtime rejects further calls. Create a new runtime
to load the last saved snapshot.

The TypeScript SDK accepts an `ActionSchema` and a TypeScript callback through
`A11Runtime.registerAction`. The callback receives a `GraphAction` backed by
the C++ action and node graph. Callback failures become A11 statuses.
`test-native-host-action.mjs` and the TypeScript runtime test exercise this
route.
`A11Runtime.graph()` exposes `GraphNode`, `GraphAction`, and `GraphSession`
conveniences over the same worker. Their operations return SDK statuses, and
the graph shares stores with `A11Runtime.createChunkStore()`. Node finalization
accepts a value, reference, or empty terminator through the native C++
`AsyncNode::Finalize` overloads.

`make_http_request` and `web-fetch` are registered for native Flow by default.
Their schemas come from the native action catalogue. They use the same HTTP
client as `runtime.http.request()`, including its Node HTTP/2 transport and
browser Fetch transport. Set `actions.make_http_request` or
`actions['web-fetch']` to `false` when creating the runtime to omit either
registration. Browser Fetch does not expose an HTTP version, trailers, push
events, or connection details; those outputs remain empty or carry `null`
fields when the host cannot observe them. The HTTP client reports unsupported
requested controls as `UNIMPLEMENTED`.
The native graph registers the same configured actions, so direct
`runtime.action('env_get')`, `runtime.action('make_http_request')`, and
`runtime.action('web-fetch')` use the same host environment and HTTP client.
`runtime.registerSchema(schema)` installs a remote-only action contract without
a local handler.
HTTP/2 pushes appear on Flow's `pushes` output with a body node id. Flow code
can read `node(rec.body)` while it runs. `runFlowWithNodes` retains that node
map after completion and returns `node(id)` and `store(id)` accessors; call
`release()` when the pushed bodies have been read. Direct
`runtime.http.request()` can also receive pushes when its transport supports
them.

The WASM workflow builds each module once, packages it once, runs the native,
TypeScript, and browser tests, and uploads the assembled SDK. The npm release
workflow downloads that tested SDK package for publication.
