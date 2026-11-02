# A11 WASM runtime

`createA11Runtime()` loads the C++ WASM runtime asynchronously. The runtime
uses native wire codecs, Flow tooling, fragment stores, nodes, actions,
sessions, and Flow execution. `backendFor(feature)` reports `wasm` for every
feature. Requests for the former TypeScript backend return `UNIMPLEMENTED`.
`backendFor()` describes ownership of the selected runtime feature. The
remaining host implementations are listed
in [the export audit](WASM_EXPORT_AUDIT.md).

Initialize the runtime before calling synchronous record, framing, status,
schema, authorization, stream-reference, or Flow tooling helpers. These
helpers return `FAILED_PRECONDITION` until their WASM bindings are ready.
Loading stays asynchronous and lazy at `createA11Runtime()`.

The package root exports the C++ owned stateful classes. Initialize one
`A11Runtime` and pass it to their factories. The previous TypeScript-only
runtime implementation has been removed.
The native classes expose the operations in their TypeScript declarations.
Private TypeScript↔WASM bindings live in `js/src/internal/wasm/`; application code
imports the public classes and helpers from the package entry points.

`@curiositystack/a11/runtime` exports the native classes under the familiar
`AsyncNode`, `Action`, `Session`, `SessionWithRecv`, and `NodeMap` names. For
example, `await Action.create(schema, {runtime})`,
`await AsyncNode.create(id, {runtime})`, and `await Session.create(runtime)`
create objects owned by that runtime. `instanceof` works with those names.
`new ActionRegistry(runtime)` registers TypeScript callbacks in the C++ action
registry; schema lookup, name listing, and action creation query that registry.
`LocalChunkStore.create(id, {runtime})` returns a C++ backed fragment store.
Registration and lookup methods are asynchronous because the registry lives in
the worker. The class-based example is `js/demo/native_runtime.mjs`.
The native registry does not return C++ handlers as JavaScript functions or
copy registrations into a second registry. `getHandler()` and `copy()` are
omitted deliberately. Use `makeAction()` and `run()` to invoke a registered
handler. Create another runtime when an independent registry is needed, then
register its actions explicitly. Each runtime owns one registry and node map;
sessions created by that runtime use both. Passing a separate `nodeMap` or
`actionRegistry` to `Session.create()` is rejected. This keeps session state in
one worker and prevents a session from retaining objects owned by another
runtime.
`FlowTooling` sends check, format, token, and other editor requests to the
native service. `FlowProgram` accepts typed `Chunk` inputs and returns typed
`Chunk` outputs while the C++ runtime executes the program. Its `onOutput`
callback receives chunks during execution.
The package root uses these same classes.

The native graph exposes reader and writer options, pull-style session
receive, and session stream callbacks. Native action and session creation is
asynchronous.

## Initialization

Node loads packaged assets with no options:

```ts
import {createA11Runtime, valueOrThrow} from '@curiositystack/a11';

const runtime = valueOrThrow(await createA11Runtime());
```

Bundled browser apps provide the served asset directory because bundling may
relocate `import.meta.url`:

```ts
const runtime = valueOrThrow(await createA11Runtime({
  wasm: {assetsBaseUrl: new URL('/a11-wasm/', location.href)},
}));
```

The browser needs cross-origin isolation for the pinned Emscripten worker.
Serve COOP and COEP headers with the page and keep the WASM assets on the
same origin. The runtime uses one scheduling worker and conservative browser
limits. The packaged WASM binaries have content-hashed names and immutable
cache headers; stable loaders are revalidated.
The guide demos register a service worker at the site root and reload once
to add these headers on hosts such as GitHub Pages. Applications outside the
guides must provide isolation through their own host or navigation worker.

`wasm.storageId` names the native storage namespace. `wasm.persistence` has
`load(): Promise<Uint8Array | null>` and `save(bytes): Promise<void>` methods.
The runtime loads a snapshot before returning. `flushPersistence()` saves a
quiescent graph, and `dispose()` flushes and returns a `Status` if saving
fails. A snapshot's storage identity must match the runtime's configured
identity. `GraphNode.ref({storage: 'include'})` calls C++ `AsyncNode::Ref` and
includes that identity.

## Native objects

`runtime.graph()` returns the native node namespace. `runtime.createNode()`,
`createAction()`, `createSession()`, and `createSessionWithRecv()` use that same
graph. `GraphNode` supports typed reads and writes, references, iteration,
finalization, reader and writer options, cancellation, and state queries.
`RuntimeGraph.getIfExists()`
consults C++ and returns asynchronously. `entries()`, `size()`, and
`discard()` also query the native node map. `discard()` returns whether a node
was removed; a retained `GraphNode` handle is id-based and must not be reused
after discard. `GraphAction` supports local execution, remote calls, port
binding, log nodes, headers, dispatch waits,
status, settings, and nested actions. Action schemas retain binary header
defaults and input autofills across the worker boundary. An action can be
created from a schema without registering it for remote dispatch.
`GraphAction.release()` removes a finished action from the worker's action
index after callers have read its outputs.
`getStreamSourcesNode()`, `registerStream()`, and `resolveNode()` use the
native action graph. `UsageAccounting.create(action)` supports native actions
asynchronously and preserves opt-in tracking and an explicit inherited off.
The collector reads native fragment streams through their `NodeRef` values.
`getToolDefinitions()` and `executeActionsFromInteraction()` also accept the
native registry and action. Nested tool actions receive the model's call id
before C++ indexes them, and their output fragments retain the SDK's mapping.
`GraphSessionWithRecv` reads inbound `WireMessage` values through
native `SessionWithRecv`. Sessions can cancel individual or all actions and
await active actions, and inspect active actions by id.
`dispatchWireMessage()` applies a received message to native session state
when the application uses pull-style receive.

`createSession()` and `createSessionWithRecv()` accept binary session headers
alongside native session limits and deadlines.
`getDeadline()` and `setDeadline()` use Unix milliseconds or `null`.

Register a TypeScript callback as a C++ action with
`await runtime.registerAction(schema, handler)`. Its callback receives a
`GraphAction`, including native input and output nodes. Register a remote
contract without a handler through `await runtime.registerSchema(schema)`.
Both registration methods return asynchronous unregister functions. A native
session can attach the SDK's `WireStream` adapters, including in-process,
WebSocket, SSE, and WebRTC streams. The host bridge forwards session trailers
on half-close so native receivers see clean EOF. Channel lifecycle and
pull-style reads run in C++; packet I/O stays in browser or Node APIs.

`runtime.runFlow()` executes native Flow and returns output chunks by port.
`onOutput` observes chunks during execution; an `AbortSignal` cancels a run.
`runtime.flowTool()` sends editor requests to the native Flow service.

## Host actions and HTTP

`ticker`, `sleep_for`, `random_bytes`, and `new_uuid` are available by
default. `env_get`, `make_http_request`, and `web-fetch` are also registered
by default and can be disabled with `wasm.actions`. Browser environment
values come from `wasm.environment`. Node reads only names listed in
`wasm.allowedEnvironmentNames`. `wasm.httpTransport` may provide a custom
HTTP transport. The direct HTTP client is available from
`@curiositystack/a11/wasm/http`; it reports its transport capabilities and
rejects unsupported requested controls.

## Build and verification

Build the compact and threaded native modules as described in
`cpp/a11/wasm/README.md`, then package the content-hashed assets:

```sh
node cpp/a11/wasm/package.mjs cpp/a11/wasm/build cpp/a11/wasm/build/threaded
npm --prefix js run build
rm -rf js/dist/wasm
mkdir -p js/dist/wasm
cp -a cpp/a11/wasm/build/dist/. js/dist/wasm/
node js/demo/native_runtime.mjs
npm --prefix js test
```

The demo uses build-tree asset URLs. The npm package assembled in CI includes
the tested WASM assets. The test workflow runs native C++ tests under
Emscripten, Node bridge tests, browser tests, and the TypeScript SDK suite.
The documentation workflow reuses that tested artifact and installs it under
`assets/wasm/` before publishing the guide pages.
