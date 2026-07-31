# Stream data through an AsyncNode

An [`AsyncNode`][a11.nodes.async_node.AsyncNode] is A11's unit of streaming
state: an **ordered** sequence of chunks that one side writes and another reads.
It is the building block everything else is made of — an action's inputs and
outputs are async nodes, and a model's tokens reach you through one. This page
builds up a producer/consumer from the smallest possible pieces.

Everything here is reachable straight off the top-level package, so a single
`import a11` is enough:

```python
import a11
```

## Create a node

`AsyncNode.create` builds a node and its backing store from a name. By default
the data lives in memory:

```python
node = a11.AsyncNode.create("tokens")
```

## Write to it

`put()` appends a value; `put_final()` appends the last one and closes the
stream. Both are coroutines that resolve once the value is durably stored, so
**awaiting them applies backpressure** — a fast producer waits for a slow
consumer instead of buffering without bound:

```python
await node.put("A11")
await node.put("streams")
await node.put_final("everything")   # closes the stream
```

A value can be anything the node's serialization registry can encode (strings,
dicts, dataclasses, Pydantic models, …); see
[serialization](../api/data.md). It comes back out as the same type.

## Read it back

Pull one value at a time with `next()` (which returns `None` at end of stream),
or iterate to completion with `async for`:

```python
async for token in node:
    print(token)
```

For a node that carries exactly one whole value — the common shape for a unary
action result — use `consume()`:

```python
result = await node.consume()
```

## Let the context manager finalize it

Writing `put_final()` by hand is easy to forget, and an error midway should not
leave a reader hanging on a half-written stream. Used as an async context
manager, a node **drains and closes on a clean exit** and **aborts with the
error's [`Status`][a11.status.Status] on an exception**, so the reader always
observes a definite end:

```python
async with a11.AsyncNode.create("tokens") as node:
    for word in ["A11", "streams", "everything"]:
        await node.put(word)
    # no explicit close: leaving the block finalizes the stream
```

## Putting it together

```python
import asyncio
import a11


async def main() -> None:
    node = a11.AsyncNode.create("tokens")

    async with node:
        for word in ["A11", "streams", "everything"]:
            await node.put(word)

    async for token in node:
        print(token)


asyncio.run(main())
```

That is the whole model: produce into a node, consume it back, and let the
context manager mark the end. Next, [carry a node's data over a
network](echo-session.md) with a session.
