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

# Write values into the stream
await node.put({"event": "start"})
await node.put({"event": "progress", "percent": 50})

# Mark the stream complete and close the writer
await node.finalize({"event": "complete", "percent": 100})
```

### Reading and Consuming

Consume items with `async for` or [`next`][a11.nodes.async_node.AsyncNode.next]. For
actions returning a single complete value, use [`consume`][a11.nodes.async_node.AsyncNode.consume]:

```python
# Stream values as they arrive
async for event in node:
    print(event)

# Or consume a single complete value from a unary result port
result = await unary_node.consume()
```

::: a11.nodes.async_node.AsyncNode

## NodeMap

`NodeMap` coordinates shared named streams across multiple actions within a session:

```python
node_map = a11.NodeMap()
input_node = node_map.get("user_input")
output_node = node_map.get("agent_response")
```

::: a11.nodes.async_node.NodeMap
