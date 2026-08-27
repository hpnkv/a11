# A11

A11 is a concurrent **action** and **streaming** runtime for AI agents that run
in one process or span multiple machines. Applications can use its Python API,
TypeScript client, or C++20 libraries.

Build an agent from **Actions**: named, typed, asynchronous operations that read
from and write to **Nodes**, A11's ordered data streams. Action handlers can call
other local or remote actions. Streaming inputs and outputs let handlers emit
partial results and callers consume them incrementally.

```python
import asyncio
from a11.nodes.async_node import AsyncNode

async def main() -> None:
    # A node is an ordered, awaitable stream of values.
    node = AsyncNode.create("greeting")
    await node.put("hello")
    await node.finalize("world")  # marks the end of the data, and closes

    async for value in node:      # consume it back, in order
        print(value)

asyncio.run(main())
```

## What A11 provides

- **Streaming-first.** Everything is a stream of chunks. Tokens, audio frames,
  tool-call deltas, and whole objects all flow through the same
  [`AsyncNode`][a11.nodes.async_node.AsyncNode] interface, so partial output is
  the default.
- **Composable actions.** An [`Action`][a11.actions.action.Action] is a unit of
  work with a schema. Actions can call one another, supporting smaller,
  independently testable components.
- **Stable transport interface.** Run the same action and node code in one
  process or across a network by choosing a
  [`WireStream`][a11.net.wire_stream.WireStream] — in-process, WebSocket,
  HTTP SSE, or WebRTC without changing action code.
- **Explicit extension points.** Storage
  ([`ChunkStore`][a11.stores.chunk_store.ChunkStore]) and transport
  ([`WireStream`][a11.net.wire_stream.WireStream]) are explicit extension
  points that applications can replace without changing agent logic.
- **Observable.** Tracing is emitted natively over OTLP/HTTP; point it at
  Langfuse or any OpenTelemetry backend. Logs from the C++ runtime arrive as
  ordinary `logging` records, under whatever configuration the
  process already has.

## Where to go next

- [Why A11](principles.md) — understand the asynchronous streaming model and
  extension points.
- [Examples](examples.md) — build a single stream, a networked service, or a
  tool-using agent.
- [Python API](api/nodes.md) — reference for every public type.
- [C++ API](cpp.md) — reference for the native libraries.
