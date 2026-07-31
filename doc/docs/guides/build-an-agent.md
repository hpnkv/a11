# Build an agent

This guide walks from the smallest useful A11 program to a networked,
action-driven agent. Each step builds on the last, and the mental model
([Principles](../principles.md)) stays the same throughout: asynchronous work,
streaming data, pluggable storage and transport.

## 1. A stream you can write and read

The foundation is the [`AsyncNode`][a11.nodes.async_node.AsyncNode] — an ordered
stream. Produce into it, then consume it back:

```python
import asyncio
from a11.nodes.async_node import AsyncNode


async def main() -> None:
    async with AsyncNode.create("tokens") as node:
        for word in ["A11", "streams", "everything"]:
            await node.put(word)   # await for backpressure
        await node.put_final("!")

    async for token in node:       # ordered, to completion
        print(token)


asyncio.run(main())
```

`put()` accepts any object your node's
[`SerializationRegistry`][a11.data.serialization.SerializationRegistry] can
encode; `next()`/`consume()` give it back, optionally coerced to a type
(`await node.next(str)`). Staying at the transport level? Use the `*_chunk` and
`*_fragment` variants.

## 2. Serialize your own types

Objects cross the boundary as [`Chunk`][a11.data.types.Chunk] values. A11 ships
serializers for common types; register your own and they flow through nodes,
actions, and the network unchanged.

```python
--8<-- "examples/001-serialization/main.py"
```

## 3. Talk to a model

Agents are mostly conversations with models and tools. A11's SDK helpers drive
an interaction and **stream** the response through nodes, so your agent reacts
to partial output:

```python
--8<-- "examples/002-llm-interactions/main.py"
```

## 4. Go distributed: a networked echo agent

Nothing above changed the shape of your code — and neither does going over a
network. Pick a [`WireStream`][a11.net.wire_stream.WireStream] transport, hand
it to a [`Session`][a11.service.session.Session], and the session multiplexes
streams and dispatches messages for you.

The server accepts connections and echoes each message back:

```python
--8<-- "examples/000-websocket-echo/server.py"
```

And the client connects, sends, and reads the replies:

```python
--8<-- "examples/000-websocket-echo/main.py"
```

To run the same agent peer-to-peer instead of client/server, swap
[`WebSocketWireStream`][a11.net.websocket_wire_stream.WebSocketWireStream] for
[`WebRtcWireStream`][a11.net.webrtc_wire_stream.WebRtcWireStream] (with a
[signalling](../api/net.md) channel to establish the connection). The session
and action code are untouched.

## Where to customize

- **Storage:** pass a `ChunkStoreFactory` to
  [`AsyncNode.create`][a11.nodes.async_node.AsyncNode.create] or a
  [`NodeMap`][a11.nodes.async_node.NodeMap] to persist streams however you like
  — see [`ChunkStore`][a11.stores.chunk_store.ChunkStore].
- **Transport:** implement [`WireStream`][a11.net.wire_stream.WireStream] to
  carry A11 over a transport we don't ship.
- **Observability:** call `a11.observability.configure_otel(...)` (or
  `langfuse(...)`) to emit traces of every action and session.
