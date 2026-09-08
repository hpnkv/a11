# Connect TypeScript sessions and transports

A `Session` routes action messages and node fragments across one or more
`WireStream` connections. Use it for remote calls, browser-hosted handlers, and
connections that may change transport without changing action code.

## Attach a transport

The browser and Node package supports WebSocket, HTTP SSE, WebRTC, and
in-process streams. A client starts a transport; a service accepts it.

```ts
import {
    ActionRegistry,
    Session,
    StreamMode,
    WebSocketWireStream,
    valueOrThrow,
} from '@curiositystack/a11';

const registry = new ActionRegistry();
valueOrThrow(registry.register('embed', embedSchema));

const session = valueOrThrow(Session.create({
    actionRegistry: registry,
    noStreamTimeoutMs: 30_000,
}));
const stream = valueOrThrow(WebSocketWireStream.createClient(
    'wss://models.example/a11',
));
valueOrThrow(await session.addStream(stream, StreamMode.START));
```

Use `HttpSseClientWireStream` when browser infrastructure already exposes an
HTTP endpoint and server-to-client delivery fits SSE. Use `WebRtcWireStream`
for direct browser peers. `InProcessWireStream.createPair()` provides the same
lifecycle for tests and two endpoints in one JavaScript runtime.

## Call a remote action

Bind the action to the session's node map, transport, and session. The dispatch
status confirms that the peer accepted the contract. The completion status
covers the remote handler and its output cleanup.

```ts
const action = valueOrThrow(registry.makeAction('embed', {
    nodeMap: session.getNodeMap(),
    stream,
    session,
}));

valueOrThrow(await action.call());
const text = valueOrThrow(await action.getInput('text'));
valueOrThrow(await text.finalize('cooperative cancellation'));
valueOrThrow(await action.waitForDispatch(10_000));

const vectors = valueOrThrow(await action.getOutput('vectors', false));
for await (const vector of vectors) index.add(vector);
valueOrThrow(await action.wait(60_000));
```

## End a connection

`halfClose()` stops new outgoing work and drains messages already queued.
Incoming work may continue until the peer also closes. `abort(status)` cancels
active actions and carries the failure to every stream.

```ts
valueOrThrow(session.halfClose());
valueOrThrow(await session.done());
```

Session limits bound queued messages, bytes, and concurrent root and nested
actions. Configure them in `SessionCreateOptions`; the defaults suit an
interactive agent connection. A service that fans out heavily should set
`maxConcurrentNestedActions` from its provider and memory limits.

## Route cancellation

`action.cancel()` sends the reserved cancellation action for a remote call.
The receiving session resolves the target action and aborts its signal. Each
runtime then recursively cancels that action's active children. Session abort
and `cancelAllActions()` cover every tracked root and nested action.

See [Run a WebSocket echo session](echo-session.md) for a two-process example
and [Call an A11 service from a browser](browser-clients.md) for HTTP SSE.
