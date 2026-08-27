# Why A11

A live-captioning service sends transcript fragments while audio is still
arriving. An image-generation API reports denoising progress before returning a
PNG. A document pipeline fetches pages, decodes records, and feeds several
workers without collecting the corpus at every boundary. A model gateway serves
tokens from a GPU process to browser, Python, and native clients.

These systems have the same runtime needs as an agent: they wait on independent
producers, exchange structured and multimodal data, and often move components
between processes as they grow. An agent adds a model/tool loop, but A11 does
not require one.

A11 keeps the operation's contract stable through those changes. The
application describes work as actions and moves data through streams; storage
and network placement remain deployment choices.

## Build streaming APIs and throughput-sensitive pipelines

An action is a useful API boundary whenever inputs or results have independent
lifecycles. A hosted model can accept configuration and prompt content on
separate inputs, then expose text, reasoning, usage, and the completed response
on separate outputs. A speech service can stream audio into recognition and
start translation on the first transcript fragment. A media endpoint can keep
progress as small JSON records while its final image remains encoded bytes with
an `image/png` media type.

These contracts remain modular because a caller routes each port to the
consumer that needs it and explicitly drains or omits the rest. Adding progress
or diagnostics does not turn every result into a larger event union. The schema
also remains the same for an in-process call, a model tool, and a service
reached through a session.

A11 supports throughput by controlling data movement:

- streaming stages can begin before an upstream producer completes;
- backpressure propagates through bounded writers and stores;
- Flow places an explicit bound on concurrent work with `parallel N`;
- local node maps keep high-volume intermediates off the network;
- media types and serialization tags describe bytes without placing type names
  inside the payload;
- the native implementation supplies scheduling, storage, framing, and
  transport for Python and C++ services, while TypeScript shares the same wire
  and action contracts.

This does not prescribe one server topology. Keep decoding beside its caller,
place inference on a GPU host, persist selected streams in SQLite or Redis, and
move only the action ports that cross those boundaries. The
[generative-media](guides/generative-media.md),
[HTTP actions](api/http-actions.md), and
[local-to-remote](guides/local-to-remote.md) guides show these choices in
complete APIs.

## Start with calls and streams, not an action graph

A11 does not require a graph object, scheduler, or checkpoint format to connect
operations. Starting an action creates its input and output streams immediately.
The action may produce output as soon as its inputs arrive, and a consumer may
start before the producer finishes. Waiting on data supplies the usual
dependency ordering.

This is close to calling asynchronous functions, with the arguments and results
represented as named streams. It supports linear pipelines, fan-out, loops, and
parallel tool calls without making every application adopt a graph runtime.

[Flow](guides/flow.md) uses the same rule. Statements start concurrently unless
an explicit ordering constraint says otherwise. A graph-shaped declaration is
still possible: create the calls first, then connect their ports.

Flow source can arrive at runtime from an application, user, or model. The host
resolves it against its current action registry, reports syntax and contract
errors before dispatch, and restricts model-authored calls with the same
per-turn allow-list as direct tool calls. A flow cannot import code or discover
capabilities outside the registry. This provides dynamic composition within an
explicit set of operations and types.

```a11flow
search = run web-search(query: question, limit: 3)
brief  = run llm-summarize(question: question)

for hit in search.hits parallel 3 {
  page = run web-fetch(url: hit.url)
  page.text | truncate 2000 -> brief.pages
}

brief.summary -> answer
```

`brief` is dispatched with its `pages` port open. Fetches feed that port as
results arrive, and the runtime closes it when the loop finishes. The source
describes a familiar search-fetch-summarize dependency, but execution is
coordinated by the data already moving between actions.

Intermediate results travel directly between ports. They need not be copied by
a model, added to its context, collected into a graph state object, or sent to
the dispatching peer. Runtime composition therefore retains the streaming and
placement choices of the underlying actions.

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

The same [`ActionSchema`][a11.actions.action.ActionSchema] also supplies the
function definition shown to a model and the contract advertised to a remote
peer. Tool adapters and MCP integrations translate at this boundary, so the
application does not maintain separate schemas for local calls, model tool
calling, and service dispatch.

The [local-to-remote guide](guides/local-to-remote.md) follows one action across
that boundary. [Browser-hosted tools](guides/browser-tools.md) show the reverse
direction: a backend dispatches an action to the connected page.

## Discover operations at runtime and reuse connections

[gRPC](https://grpc.io/docs/what-is-grpc/core-concepts/) commonly starts with a
service definition and generated client and server code. Its unary and
streaming RPCs run through reusable channels. This model, descended from
Google's [Stubby](https://grpc.io/blog/principles/), is effective when services
have a stable interface and every client can build against it.

A11 retains efficient connection reuse but makes the operation layer dynamic.
A registry exposes its current `ActionSchema` values at runtime, and callers
bind named node streams without generating a client stub. This fits model
tools, plugins, browser capabilities, and tenant-specific services whose
available operations can change while the application is running.

The dynamic contract does not require a physical connection for every call or
port. Attaching a `WireStream` starts or accepts that transport immediately.
The `Session` then multiplexes action control, input fragments, output
fragments, and shutdown messages over each active stream. Many logical calls
and nodes can therefore share a small number of network connections, with
explicit limits, backpressure, deadlines, cancellation, and status handling.
See the [Session lifecycle](lifecycles/session.md) for the exact routing and
completion rules.

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

A11 implements node ordering, cursors, and final status over established
storage systems. SQLite supplies embedded transactions and local durability;
Redis supplies shared streams, atomic operations, deployment tooling, and its
own replication and scaling options. Applications can use infrastructure they
already operate instead of adopting a dedicated A11 storage service. The
backend's operational limits still apply: SQLite is primarily a one-machine
choice, while Redis Cluster needs the routing setup described in the
[Redis reference](api/redis.md).

The [persistent chat](guides/chat-sessions.md) and
[Redis stream](guides/going-distributed.md) guides demonstrate these choices.

This covers many cases for which an agent framework would introduce a separate
conversation-memory or stream-checkpoint layer. Store the completed
`Interaction` values when conversation history is the requirement; choose a
durable node store when live stream data must survive process boundaries. These
are explicit data-retention choices, not hidden state in an agent runner.

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
