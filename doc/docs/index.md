# A11

A11 is a concurrent **action** and **streaming** runtime for building AI agents
— including distributed ones that span processes and machines. The public API
is Python; the runtime underneath it is a C++20 engine bound with pybind11, so
you get native throughput and cooperative concurrency without leaving Python.

You describe your agent as a set of **Actions** — named, typed, asynchronous
operations — that read from and write to **Nodes**, A11's ordered streams of
data. Actions compose (an action's handler can call other actions, local or
remote), and their inputs and outputs stream: a handler can emit partial results
the moment it has them, and a caller can consume them incrementally.

```python
import asyncio
from a11.nodes.async_node import AsyncNode

async def main() -> None:
    # A node is an ordered, awaitable stream of values.
    async with AsyncNode.create("greeting") as node:
        await node.put("hello")
        await node.put_final("world")

    async for value in node:      # consume it back, in order
        print(value)

asyncio.run(main())
```

## Why A11

- **Streaming-first.** Everything is a stream of chunks. Tokens, audio frames,
  tool-call deltas, and whole objects all flow through the same
  [`AsyncNode`][a11.nodes.async_node.AsyncNode] interface, so partial output is
  the default.
- **Composable actions.** An [`Action`][a11.actions.action.Action] is a unit of
  work with a schema. Actions nest and call one another, which is how you build
  an agent out of smaller, testable pieces.
- **Distribution is a transport swap.** The same agent runs in one process or
  across a network by choosing a
  [`WireStream`][a11.net.wire_stream.WireStream] — in-process, WebSocket,
  HTTP SSE, or WebRTC. Your action code does not change.
- **Pluggable everywhere it matters.** Storage
  ([`ChunkStore`][a11.stores.chunk_store.ChunkStore]) and transport
  ([`WireStream`][a11.net.wire_stream.WireStream]) are explicit extension
  points — swap in your own without touching agent logic.
- **Observable.** Tracing is emitted natively over OTLP/HTTP; point it at
  Langfuse or any OpenTelemetry backend. Logs from the C++ runtime arrive as
  ordinary `logging` records, under whatever configuration the
  process already has.

## Where to go next

- [Why A11](principles.md) — the async, streaming model, and the extension
  points you will build on.
- [Examples](examples.md) — a hands-on tour from a single node to a
  networked, tool-using agent, one small step at a time.
- [Python API](api/nodes.md) — reference for every public type.
- [C++ internals](cpp.md) — the native runtime, for contributors.
