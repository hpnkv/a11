# Nodes

An [`AsyncNode`][a11.nodes.async_node.AsyncNode] is A11's unit of streaming state:
an ordered, asynchronous sequence of chunks backed by a [`ChunkStore`][a11.stores.chunk_store.ChunkStore].
Nodes carry data between action ports and across network transports.

## Using AsyncNode

### Creating and Writing

Create a node with [`create`][a11.nodes.async_node.AsyncNode.create] and write items
sequentially with [`put`][a11.nodes.async_node.AsyncNode.put]. Each write returns
a future confirming storage acceptance. Mark the end of stream with
[`finalize`][a11.nodes.async_node.AsyncNode.finalize]:

```python
import a11

node = a11.AsyncNode.create("events")

await node.put({"event": "start"})
await node.put({"event": "progress", "percent": 50})
await node.finalize({"event": "complete", "percent": 100})
```

### Reading and Consuming

Consume items with `async for` or [`next`][a11.nodes.async_node.AsyncNode.next]. For
actions returning a single complete value, use [`consume`][a11.nodes.async_node.AsyncNode.consume]:

```python
async for event in node:
    print(event)

result = await unary_node.consume()
```

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
