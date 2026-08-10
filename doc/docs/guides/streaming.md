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

`put()` admits a value into a bounded writer and returns its confirmation
future. Await the coroutine once for admission, then await that future when the
backing store must have accepted the value. Attached stream sends are attempted
or queued during the flush, but the confirmation is not an acknowledgement from
the remote reader. That second wait propagates storage and local-transport
**backpressure**:

```python
confirmation = await node.put("A11")
sequence = await confirmation
```

When you are the *authoritative* writer — the one who decides the stream is done
— finish in two steps: mark the last value **final**, then **seal** the store:

```python
await node.put("A11")
await node.put("streams")
await node.put_final("everything")  # mark where the data ends
await node.drain_and_close()  # flush, forbid further writes, record OK
```

These do different jobs, and a correct writer usually wants **both**:

- `put_final(value)` (equivalently `put(value)` then `put_null_final()`) marks
  the *end of the data*, so a reader knows the last value is whole.
- `drain_and_close()` waits for buffered writes to flush, then **closes the
  store with an OK status and refuses further writes**, and tells any attached
  stream so a peer's copy of the node closes as well. On its own, `put_final()`
  leaves the store open to more writes; without a final marker, a `consume()`
  reader cannot tell the value was complete.

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

Sealing by hand is easy to forget, and an error midway should not leave a reader
hanging on a half-written stream. As an async context manager a node
**`drain_and_close()`s on a clean exit** and **aborts with the error's
[`Status`][a11.status.Status] on an exception**, so a reader always observes a
definite end:

```python
async with a11.AsyncNode.create("tokens") as node:
    for word in ["A11", "streams", "everything"]:
        await node.put(word)
    # leaving the block seals the store (clean exit) or aborts it (on error)
```

The context manager handles the *sealing*, but it does not mark a final data
chunk — which is exactly what `async for` and `next()` want, since they stop the
moment the store closes. A `consume()` reader, which expects one whole value,
still needs an explicit terminator: end the block with `put_final(value)` (or a
`put_null_final()` after the last `put()`).

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

That is the whole model: produce into a node, read it back, and let the context
manager seal it. Next, [carry a node's data over a
network](echo-session.md) with a session.
