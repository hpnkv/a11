# Principles

A11 is small at its core: a handful of ideas compose into everything from a
one-process helper to a fleet of networked agents. This page is the mental model
to hold while you read the rest of the docs.

## Everything is asynchronous

A11 is built for work that waits — on a model, a tool, a peer, a human. Every
operation that could block is a coroutine you `await`, and the runtime schedules
thousands of them cooperatively. You write straight-line `async def` code;
A11 keeps the machine busy.

Two conveniences make this pleasant:

- **Completion is an event.** An [`Action`][a11.actions.action.Action] and a
  [`Session`][a11.service.session.Session] each expose a `done` you can
  `await action.done.wait()`, shaped like an `asyncio.Event`.
- **Lifecycles are context managers.** `async with` a node, wire stream, or
  session and it is finalised for you — drained and closed on success, aborted
  with the right [`Status`][a11.status.Status] on error, so the other side of a
  stream always learns how it ended.

## Everything is a stream

The unit of data in A11 is the **chunk**, and the unit of state is the
**node** — an [`AsyncNode`][a11.nodes.async_node.AsyncNode], which is a single
ordered sequence of chunks with a writer end and a reader end.

This is deliberate. An agent rarely has its whole answer at once; it has the
*next* token, the *next* audio frame, the *next* tool call. Nodes make that the
natural shape:

- **Produce incrementally** with `put()` / `put_final()`. Each write returns a
  future that completes when the chunk is durably stored (and sent, if a
  transport is attached), so awaiting it applies backpressure.
- **Consume in whatever shape fits** — `await node.next()` for the next value,
  `async for value in node` to drain, or `await node.consume()` for a single
  whole result. Objects are serialized on the way in and deserialized on the way
  out via the node's
  [`SerializationRegistry`][a11.data.serialization.SerializationRegistry].

Actions are built from nodes: an [`Action`][a11.actions.action.Action]'s typed
input and output **ports** are nodes, so calling an action is really wiring
streams together. A handler can start emitting into an output port before it has
finished reading its inputs — which is exactly what streaming an LLM response
through a pipeline of actions looks like.

## Two extension points: storage and transport

Most of A11 is fixed machinery. Two interfaces are deliberately left open,
because they are where real deployments differ.

### ChunkStore — where stream data lives

A [`ChunkStore`][a11.stores.chunk_store.ChunkStore] is the ordered log behind a
node. The default
[`LocalChunkStore`][a11.stores.local_chunk_store.LocalChunkStore] keeps chunks in
memory, but the interface is yours to implement: persist a stream to disk or a
database, enforce a retention policy, or inject faults in tests — without
changing any action or node code. You choose an implementation by passing a
`ChunkStoreFactory` (a `node_id -> ChunkStore` callable) to a
[`NodeMap`][a11.nodes.async_node.NodeMap] or
[`AsyncNode.create`][a11.nodes.async_node.AsyncNode.create].

### WireStream — how bytes move between peers

A [`WireStream`][a11.net.wire_stream.WireStream] is a bidirectional, ordered
channel carrying [`WireMessage`][a11.data.types.WireMessage] values between two
endpoints. Everything above it — node mirroring, session multiplexing, remote
action dispatch — is written against this one interface, so the concrete
transport is a detail you pick at the edge:

- [`InProcessWireStream`][a11.net.in_process_wire_stream.InProcessWireStream] —
  two endpoints in one process (great for tests and in-process composition);
- [`WebSocketWireStream`][a11.net.websocket_wire_stream.WebSocketWireStream] —
  the default network transport, over A11's nghttp2/HTTP2 stack;
- [`HttpSseWireStream`][a11.net.http_sse_wire_stream.HttpSseWireStream] — an
  HTTP Server-Sent-Events channel for firewall-friendly, HTTP-only paths;
- [`WebRtcWireStream`][a11.net.webrtc_wire_stream.WebRtcWireStream] —
  peer-to-peer data channels, with NAT traversal via
  [signalling](api/net.md).

Because the interface is stable, making an agent distributed is a transport
swap, not a rewrite — and you can implement `WireStream` yourself to carry A11
traffic over a transport we don't ship.

## Sessions tie it together

A [`Session`][a11.service.session.Session] is the connection-scoped runtime: add
one or more wire streams, and it multiplexes them, dispatches inbound action
calls against a registry, and tracks their lifetimes so the whole connection can
be drained and closed cleanly. It is the object you build a server or client
agent around.
