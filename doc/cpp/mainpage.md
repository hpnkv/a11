# A11 C++ API

Use the C++20 libraries to build actions, stream data through nodes, connect
peers, and embed A11 directly in a native application. The components under
`cpp/a11/` are independently linkable, so an application can depend on the
layers it uses. Python binds the same runtime objects with pybind11 and follows
the same behavioural contract.

## How the pieces fit together

A11 is a stack of independently linkable components:

- **concurrency** — the fiber-aware task/future primitives (`a11::Task`,
  executors, schedulers) every asynchronous operation is built on. Awaitables
  returned across the runtime "resolve when ..." some work completes.
- **data** — the wire value types (`a11::data::Chunk`, `a11::data::NodeFragment`,
  `a11::data::WireMessage`) and the `a11::data::SerializationRegistry` that maps
  application objects to and from chunks.
- **stores** — `a11::stores::ChunkStore`, the ordered, appendable log that holds
  a stream's data, with `ChunkStoreReader`/`ChunkStoreWriter` cursors over it.
  The default `LocalChunkStore` keeps data in memory; the interface is a
  pluggable extension point.
- **nodes** — `a11::nodes::AsyncNode`, the unit of streaming state (an *ordered*
  sequence of chunks, keyed by sequence number) that one side writes and another
  reads, optionally mirrored to a peer over a wire stream. `NodeMap` groups the
  nodes of a peer.
- **net** — `a11::net::WireStream`, the transport abstraction, and its
  implementations (in-process, WebSocket/HTTP2, HTTP SSE, WebRTC) plus the
  signalling used to establish peer connections. A wire stream's delivery is
  **unordered** but **synchronised on closure**.
- **actions** — `a11::actions::Action`, the schema-described unit of work whose
  typed input/output ports are async nodes, together with `ActionRegistry` and
  the `ActionSchema` family that describe an action's interface.
- **service** — `a11::service::Session`, the connection-scoped runtime that
  attaches wire streams, dispatches incoming action calls against a registry,
  and tracks their lifetimes so a connection can drain and close cleanly.
- **obs** — native OpenTelemetry tracing emitted directly from C++.

## Where to start

- Streaming and storage: `a11::nodes::AsyncNode`, `a11::stores::ChunkStore`.
- Actions and dispatch: `a11::actions::Action`, `a11::actions::ActionRegistry`.
- Networking: `a11::net::WireStream`, `a11::service::Session`.

For task-oriented walkthroughs shared by the language APIs, see the
[main A11 documentation](../index.html). Build and linking instructions are in
[`BUILDING.md`](https://github.com/hpnkv/a11/blob/main/BUILDING.md).
