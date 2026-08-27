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

<link rel="stylesheet" href="assets/interface-map.css">
<link rel="stylesheet" href="assets/navigation-cards.css">

<nav class="a11-card-nav" aria-label="Problems A11 can help solve">
  <a href="#build-streaming-apis-and-throughput-sensitive-pipelines">
    <strong>Return useful output before work finishes</strong>
    <span>Stream tokens, audio, progress, and media through separate ports.</span>
  </a>
  <a href="#connect-models-without-rebuilding-the-application-boundary">
    <strong>Connect a model and its tools</strong>
    <span>Use included Claude, Gemini, and Ollama actions with portable state.</span>
  </a>
  <a href="#start-with-calls-and-streams-not-an-action-graph">
    <strong>Let a composition change at runtime</strong>
    <span>Pipe data through checked Flow source using registered actions.</span>
  </a>
  <a href="#build-a-capability-once">
    <strong>Move work to a GPU, browser, or service</strong>
    <span>Keep one action contract across local and remote execution.</span>
  </a>
  <a href="#discover-operations-at-runtime-and-reuse-connections">
    <strong>Add operations without regenerating clients</strong>
    <span>Discover schemas at runtime while reusing physical connections.</span>
  </a>
  <a href="#choose-where-stream-data-lives">
    <strong>Retain or share a live stream</strong>
    <span>Use memory, SQLite, Redis, or an application ChunkStore.</span>
  </a>
  <a href="#connect-peers-when-they-need-live-calls">
    <strong>Use the connection your product needs</strong>
    <span>Choose built-in browser and peer transports or provide one.</span>
  </a>
  <a href="#make-completion-and-failure-observable">
    <strong>Know whether partial work completed</strong>
    <span>Carry finality, failure, deadlines, and cancellation explicitly.</span>
  </a>
</nav>

## See how the interfaces fit together

Solid arrows show what an interface owns or creates; dashed arrows show a
dependency. The highlighted extension points have supported implementations in
A11 and can also be supplied by an application. Hover over or focus a box for
its role, then select it to open the corresponding reference.

<div class="a11-interface-map" role="region" aria-label="A11 interface map">
<svg viewBox="0 0 1000 710" role="group"
     aria-labelledby="a11-map-title a11-map-description">
  <title id="a11-map-title">A11 interfaces and their relationships</title>
  <desc id="a11-map-description">
    A navigable map from Flow and model integrations through actions,
    sessions, nodes, storage, transport, and data types.
  </desc>
  <defs>
    <marker id="a11-map-owned" markerWidth="9" markerHeight="9"
            refX="8" refY="4.5" orient="auto">
      <path d="M0,0 L9,4.5 L0,9 Z" class="a11-map-arrow-owned"/>
    </marker>
    <marker id="a11-map-used" markerWidth="9" markerHeight="9"
            refX="8" refY="4.5" orient="auto">
      <path d="M0,0 L9,4.5 L0,9 Z" class="a11-map-arrow-used"/>
    </marker>
  </defs>

  <g class="a11-map-edges" aria-hidden="true">
    <path class="used" d="M230 60 C265 60 265 160 300 160"/>
    <text x="248" y="102">resolves</text>
    <path class="used" d="M230 160 H580"/>
    <text x="385" y="150">adapts to</text>
    <path class="owned" d="M130 270 C130 220 300 225 330 190"/>
    <text x="174" y="225">owns</text>
    <path class="owned" d="M230 300 H300"/>
    <text x="242" y="290">creates</text>
    <path class="owned" d="M500 160 H580"/>
    <text x="511" y="150">creates</text>
    <path class="used" d="M400 270 V190"/>
    <text x="410" y="235">dispatches</text>
    <path class="owned" d="M500 300 H560"/>
    <text x="508" y="290">owns</text>
    <path class="used" d="M350 330 C310 370 225 420 230 460"/>
    <text x="252" y="390">multiplexes over</text>
    <path class="owned" d="M680 190 C720 215 775 235 835 270"/>
    <text x="730" y="223">ports</text>
    <path class="owned" d="M740 300 H800"/>
    <text x="746" y="290">indexes</text>
    <path class="used" d="M890 330 V430"/>
    <text x="900" y="385">backed by</text>
    <path class="used" d="M800 315 C700 350 580 410 500 450"/>
    <text x="625" y="383">encodes values as</text>
  </g>

  <g class="a11-map-help" aria-hidden="true">
    <rect x="30" y="600" width="940" height="80" rx="9"/>
    <text x="55" y="632">Hover over or focus an interface to see its role.</text>
    <text x="55" y="657">Select the interface to open its documentation.</text>
  </g>

  <a class="a11-map-node" href="api/flow.html" tabindex="0"
     aria-label="Flow. Resolves runtime compositions through an ActionRegistry.">
    <rect x="30" y="30" width="200" height="60" rx="9"/>
    <text x="130" y="66">Flow</text>
    <g class="a11-map-tip">
      <rect x="30" y="600" width="940" height="80" rx="9"/>
      <text x="55" y="632">Flow checks and executes runtime compositions against the host's</text>
      <text x="55" y="657">current ActionRegistry; every flow is also an Action.</text>
    </g>
  </a>

  <a class="a11-map-node" href="llm-sdk/interactions.html" tabindex="0"
     aria-label="LLM SDK. Adapts model interactions and tool calls to Actions.">
    <rect x="30" y="130" width="200" height="60" rx="9"/>
    <text x="130" y="166">LLM SDK</text>
    <g class="a11-map-tip">
      <rect x="30" y="600" width="940" height="80" rx="9"/>
      <text x="55" y="632">Provider adapters expose model requests, streamed outputs, and tool</text>
      <text x="55" y="657">calls through Actions and AsyncNodes.</text>
    </g>
  </a>

  <a class="a11-map-node" href="api/service.html#service" tabindex="0"
     aria-label="Service. Owns an ActionRegistry and creates Sessions.">
    <rect x="30" y="270" width="200" height="60" rx="9"/>
    <text x="130" y="306">Service</text>
    <g class="a11-map-tip">
      <rect x="30" y="600" width="940" height="80" rx="9"/>
      <text x="55" y="632">Service shares one action registry across listeners and creates a</text>
      <text x="55" y="657">Session for each connected peer.</text>
    </g>
  </a>

  <a class="a11-map-node extensible" href="api/net.html" tabindex="0"
     aria-label="WireStream. Extensible transport interface with built-in implementations.">
    <rect x="30" y="430" width="200" height="60" rx="9"/>
    <text x="130" y="454">WireStream</text>
    <text class="a11-map-badge" x="130" y="478">EXTENSIBLE</text>
    <g class="a11-map-tip">
      <rect x="30" y="600" width="940" height="80" rx="9"/>
      <text x="55" y="632">Use the in-process, WebSocket, HTTP SSE, or WebRTC implementations,</text>
      <text x="55" y="657">or implement WireStream for another bidirectional transport.</text>
    </g>
  </a>

  <a class="a11-map-node" href="api/actions.html#actionregistry" tabindex="0"
     aria-label="ActionRegistry. Holds schemas and handlers and creates Actions.">
    <rect x="300" y="130" width="200" height="60" rx="9"/>
    <text x="400" y="166">ActionRegistry</text>
    <g class="a11-map-tip">
      <rect x="30" y="600" width="940" height="80" rx="9"/>
      <text x="55" y="632">ActionRegistry is the runtime catalogue of schemas and handlers used</text>
      <text x="55" y="657">for discovery, local calls, remote dispatch, tools, and Flow.</text>
    </g>
  </a>

  <a class="a11-map-node" href="api/service.html" tabindex="0"
     aria-label="Session. Owns a NodeMap and multiplexes calls over WireStreams.">
    <rect x="300" y="270" width="200" height="60" rx="9"/>
    <text x="400" y="306">Session</text>
    <g class="a11-map-tip">
      <rect x="30" y="600" width="940" height="80" rx="9"/>
      <text x="55" y="632">Session resolves inbound actions, owns the peer's NodeMap, and routes</text>
      <text x="55" y="657">many action and node streams over a bounded set of transports.</text>
    </g>
  </a>

  <a class="a11-map-node" href="api/data.html" tabindex="0"
     aria-label="Data and serialization. Encodes values as typed chunks and wire messages.">
    <rect x="300" y="430" width="200" height="60" rx="9"/>
    <text x="400" y="466">Data &amp; serialization</text>
    <g class="a11-map-tip">
      <rect x="30" y="600" width="940" height="80" rx="9"/>
      <text x="55" y="632">Chunks carry bytes, media metadata, and status through stores and</text>
      <text x="55" y="657">transports; the serialization registry maps application values to them.</text>
    </g>
  </a>

  <a class="a11-map-node" href="api/actions.html" tabindex="0"
     aria-label="Action. A schema-described operation whose ports are AsyncNodes.">
    <rect x="580" y="130" width="200" height="60" rx="9"/>
    <text x="680" y="166">Action</text>
    <g class="a11-map-tip">
      <rect x="30" y="600" width="940" height="80" rx="9"/>
      <text x="55" y="632">Action binds an ActionSchema to named input and output AsyncNodes,</text>
      <text x="55" y="657">then runs a local handler or dispatches the call through a Session.</text>
    </g>
  </a>

  <a class="a11-map-node" href="api/nodes.html#nodemap" tabindex="0"
     aria-label="NodeMap. Indexes the AsyncNodes shared within a Session.">
    <rect x="560" y="270" width="180" height="60" rx="9"/>
    <text x="650" y="306">NodeMap</text>
    <g class="a11-map-tip">
      <rect x="30" y="600" width="940" height="80" rx="9"/>
      <text x="55" y="632">NodeMap gives actions and incoming fragments a shared node namespace</text>
      <text x="55" y="657">inside a Session.</text>
    </g>
  </a>

  <a class="a11-map-node" href="api/nodes.html#using-asyncnode" tabindex="0"
     aria-label="AsyncNode. Streams ordered values through a ChunkStore.">
    <rect x="800" y="270" width="170" height="60" rx="9"/>
    <text x="885" y="306">AsyncNode</text>
    <g class="a11-map-tip">
      <rect x="30" y="600" width="940" height="80" rx="9"/>
      <text x="55" y="632">AsyncNode is every action port: producers append values, consumers</text>
      <text x="55" y="657">iterate or consume them, and final status makes completion observable.</text>
    </g>
  </a>

  <a class="a11-map-node extensible" href="api/stores.html" tabindex="0"
     aria-label="ChunkStore. Extensible storage interface with built-in implementations.">
    <rect x="800" y="430" width="170" height="60" rx="9"/>
    <text x="885" y="454">ChunkStore</text>
    <text class="a11-map-badge" x="885" y="478">EXTENSIBLE</text>
    <g class="a11-map-tip">
      <rect x="30" y="600" width="940" height="80" rx="9"/>
      <text x="55" y="632">Use local memory, embedded SQLite, or shared Redis, or implement</text>
      <text x="55" y="657">ChunkStore for application-specific retention and placement.</text>
    </g>
  </a>

  <g class="a11-map-legend" aria-hidden="true">
    <path class="owned" d="M35 555 H90"/>
    <text x="100" y="560">owns or creates</text>
    <path class="used" d="M245 555 H300"/>
    <text x="310" y="560">uses or requires</text>
  </g>
</svg>
</div>

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

## Connect models without rebuilding the application boundary

The included `interact_with_llm` action routes an `Interaction` to Claude,
Gemini, or Ollama. Text and reasoning stream on named output ports, while
completed interactions carry usage and conversation state. Application code
does not have to adopt each provider's event types or conversation envelope.

An `Interaction` retains structured text and images, tool calls, tool results,
token usage, and provider-native values needed to continue a turn. Actions
become model tools through their existing schemas, and the shared tool runner
turns requested calls back into ordinary A11 action calls. Switching an
included backend therefore does not require a second tool registry or
conversation data model.

Use a backend-specific action when the provider's controls are part of the
product contract, or `interact_with_llm` when the application chooses the
backend at runtime. [Talk to a model](guides/llm.md) starts with a streamed
turn; the [LLM SDK articles](llm-sdk/interactions.md) cover portable state,
actions as tools, MCP, and tool execution.

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

A [`ChunkStore`][a11.stores.chunk_store.ChunkStore] is the extensible storage
interface behind a node's ordered chunks. A11 includes an in-memory store,
embedded SQLite persistence, and Redis-backed streams. Applications can provide
a `ChunkStore` and factory for another database, object store, retention policy,
or test environment without changing the producer, consumer, or action schema:

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

The connection implements the extensible
[`WireStream`][a11.net.wire_stream.WireStream] interface. A11 includes
in-process, WebSocket, HTTP Server-Sent Events, and WebRTC implementations.
Applications can implement the same interface for another bidirectional
transport while sessions, actions, and nodes remain unchanged. The
[echo service](guides/echo-session.md) shows the complete client and server
lifecycle; the [browser client](guides/browser-clients.md) uses an
HTTP-compatible transport.

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
