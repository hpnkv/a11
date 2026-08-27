# Why A11

Agent applications spend much of their time waiting: for model output, tools,
people, storage, and other services. They also change shape as they grow. A
handler that begins in one process may later need a GPU service, a browser
client, durable state, or execution on another machine.

A11 keeps the operation's contract stable through those changes. The
application describes work as actions and moves data through streams; storage
and network placement remain deployment choices.

## Build a capability once

An [`Action`][a11.actions.action.Action] is a named operation with described
input and output ports. Its handler can run in the caller's process or be
registered on another peer. Callers feed and read the same ports in either
case.

This makes location a deployment decision:

- keep a formatter or parser beside its caller;
- put model access behind a service that owns credentials or a GPU;
- run a browser action in the page that owns the canvas or editor state;
- compose several registered actions into one workflow.

The [local-to-remote guide](guides/local-to-remote.md) follows one action across
that boundary. [Browser-hosted tools](guides/browser-tools.md) show the reverse
direction: a backend dispatches an action to the connected page.

## Return work as it is produced

Every action port is an [`AsyncNode`][a11.nodes.async_node.AsyncNode], an ordered
stream of values. A handler can write progress, tokens, audio frames, records,
or one complete object. The caller chooses whether to process each value or
collect a unary result.

- `put()` appends a value and exposes confirmation from the backing store.
- `async for` consumes values until the stream ends.
- `consume()` reads a port expected to contain one complete value.
- `finalize()` marks the logical end of the data and closes the writer.
- `abort_with_status()` ends the stream with a structured failure.

Because a consumer can start before the producer finishes, connected actions
can overlap their work. A text interface can display model output immediately;
a media action can report progress on one port while preparing an image on
another. See [streaming through a node](guides/streaming.md) and
[separate progress and result ports](guides/generative-media.md).

## Choose where stream data lives

A [`ChunkStore`][a11.stores.chunk_store.ChunkStore] holds a node's ordered
chunks. The default local store keeps them in memory. Redis and SQLite stores
support different application needs without changing the producer or consumer:

- SQLite can retain conversations and reopen them after a process restart;
- Redis can connect programs through durable, named streams even when their
  lifetimes do not overlap;
- a custom store can apply an application's retention, persistence, or testing
  policy.

The [persistent chat](guides/chat-sessions.md) and
[Redis stream](guides/going-distributed.md) guides demonstrate these choices.

## Connect peers when they need live calls

A [`Session`][a11.service.session.Session] dispatches actions and carries their
node data over one or more connections. It tracks in-flight work so callers and
services can drain and close cleanly.

The connection implements the
[`WireStream`][a11.net.wire_stream.WireStream] interface. Applications can use
an in-process pair, WebSocket, HTTP Server-Sent Events, or WebRTC while the
action layer remains unchanged. The [echo service](guides/echo-session.md)
shows the complete client and server lifecycle; the
[browser client](guides/browser-clients.md) uses an HTTP-compatible transport.

Use a session for live action dispatch between peers. Use a shared store when
the main requirement is durable stream data that either side may read later.

## Make completion and failure observable

Stream termination is part of the data contract. Finalization tells a consumer
that it received a complete result; an aborted stream carries a status instead
of appearing to be valid but truncated.

Actions, sessions, and connections also expose completion explicitly. A caller
can await the result it needs, wait for the full action, or let a context manager
drain a connection during shutdown. Deadlines and cancellation propagate
through nested action calls.

The lifecycle articles describe the exact transitions for
[nodes](lifecycles/async-node.md), [actions](lifecycles/action.md),
[sessions](lifecycles/session.md), and [connections](lifecycles/wire-stream.md).

## Use the language suited to each boundary

Python, TypeScript, and C++ expose the same action, node, session, storage, and
transport concepts. A Python service, TypeScript browser, and native component
can share action schemas and exchange the same wire messages while each uses
the conventions of its language.

Start with the [examples by task](examples.md), or go directly to the
[Python](api/nodes.md), [TypeScript](typescript.md), or [C++](cpp.md) reference.
