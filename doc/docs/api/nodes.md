# Nodes

An [`AsyncNode`][a11.nodes.async_node.AsyncNode] is A11's unit of streaming state:
an ordered, asynchronous sequence of chunks backed by a [`ChunkStore`][a11.stores.chunk_store.ChunkStore].
Nodes carry data between action ports and across network transports.

## Using AsyncNode

### Creating and writing

Create a node with [`create`][a11.nodes.async_node.AsyncNode.create] and write items
sequentially with [`put`][a11.nodes.async_node.AsyncNode.put]. A write has two
asynchronous stages: admission to the bounded writer, then acceptance by the
backing store. Await both when later work depends on durable acceptance:

```python
import a11

node = a11.AsyncNode.create("events")

confirmation = await node.put({"event": "start"})
sequence = await confirmation
```

The first await applies local backpressure. The confirmation reports store
acceptance and any local transport failure; it is not an acknowledgement from
a remote reader. Code that will drain the node before shutdown can omit the
second await.

[`finalize`][a11.nodes.async_node.AsyncNode.finalize] records the logical end
of the data and closes the writer:

```python
await node.put({"event": "progress", "percent": 50})
await node.finalize({"event": "complete", "percent": 100})
```

`finalize(value)` makes `value` the final visible record. `finalize()` writes
an invisible final marker after previously admitted records. Pass `wait=True`
when the process must remain alive until finality and closure reach the store.
Use `abort_with_status()` when partial output must be reported as a failure.

### Iterating streams and reading unary values

Read items one-by-one with `async for` or
[`next`][a11.nodes.async_node.AsyncNode.next]. For an action returning one
complete value, use
[`consume`][a11.nodes.async_node.AsyncNode.consume]:

```python
async for event in node:
    print(event)

result = await unary_node.consume()
```

`next()` returns one independent value and `None` at the end of a successful
stream. `consume()` requires one logically complete value, including the final
marker, and rejects an incomplete unary result. Use `next_chunk()` or
`next_fragment()` when code needs media metadata, serialization tags, sequence
numbers, or routing fields.

The [AsyncNode lifecycle](../lifecycles/async-node.md) defines finality,
closure, reset, replay, and failure in detail.

## Values and encoded chunks

`put()` serializes an application value through the node's registry.
`put_chunk()` writes bytes that already have their final representation, such
as a PNG or an HTTP body. The chunk metadata must state the media type because
readers use that metadata to interpret the bytes. The
[data reference](data.md) describes representations and type tags.

Pre-encoded binary assets use chunks directly. The reader uses
`next_chunk()` and an explicit size limit.

::: a11.nodes.async_node.AsyncNode

## AsyncNode and durable stream services

An `AsyncNode` backed by Redis or SQLite combines an ordered record stream with
an action port's lifecycle. Producers append chunks; readers can follow new
data or read stored data; finalization and failure are part of the same
contract. Media metadata and serialization tags remain attached to the data,
and changing the store does not change the action schema.

Modern stream services expose related facilities. For example,
[S2](https://s2.dev/docs/intro) provides managed, durable, ordered streams with
append sessions, live tailing, and replay from retained positions. Its
[agent patterns](https://s2.dev/docs/use-cases/agents) include resumable token
delivery, event sourcing, and coordination through a stream per run.

The scopes differ. S2 is a hosted stream storage API with service-specific
positions, access controls, reconnection, and scaling. `AsyncNode` is an A11
runtime primitive connected directly to action inputs and outputs. Its storage
is selectable: memory for local work, SQLite for embedded durability, or Redis
for readers and writers in independent processes. This is useful when the
application needs streamed action ports and already operates Redis, or when a
single-machine service can keep its state in SQLite.

## NodeMap

`NodeMap` coordinates named streams shared by actions within a session:

```python
node_map = a11.NodeMap()
input_node = node_map.get("user_input")
output_node = node_map.get("agent_response")
```

::: a11.nodes.async_node.NodeMap
