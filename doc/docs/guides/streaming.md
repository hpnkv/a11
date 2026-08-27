# Stream data through an AsyncNode

An [`AsyncNode`][a11.nodes.async_node.AsyncNode] is A11's unit of streaming
state: an **ordered** sequence of chunks that one side writes and another reads.
Action inputs and outputs are async nodes, and model tokens can stream through
them. This guide builds a minimal producer and consumer.

All required types are available from the top-level package:

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
— end it with **`finalize()`**:

```python
await node.put("A11")
await node.put("streams")
await node.finalize("everything")  # mark where the data ends, and seal
```

One call, because a writer almost always wants the two things it does:

- it marks the **end of the data**, so a reader knows the last value is whole.
  `finalize(value)` writes that value as the final one; `finalize()` with no
  value writes an invisible terminator after whatever `put()` already sent.
- it then **closes the store with an OK status and refuses further writes**, and
  tells any attached stream so a peer's copy of the node closes as well.

`finalize()` does not wait for the store. Its writer pump completes the write
and closure asynchronously, allowing a handler to finish after ending its
outputs. Pass `wait=True` to wait for storage, and `close=False` to mark the end
without closing yet.

A value can be anything the node's serialization registry can encode (strings,
dicts, dataclasses, Pydantic models, ...); see
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

## When the end is not a value

A streaming producer usually does not know which token was the last one until it
has already sent it. Then the terminator carries no value of its own:

```python
node = a11.AsyncNode.create("tokens")
for word in ["A11", "streams", "everything"]:
    await node.put(word)
await node.finalize()
```

`async for` and `next()` would stop on the closure alone; the null terminator is
what a `consume()` reader — one expecting a single whole value — needs to know
the value was complete.

Use `abort_with_status(status)` for a failed stream. Readers then receive the
error instead of treating a truncated stream as complete. When an action handler
raises, its outputs are aborted with the corresponding status.

For the rare producer that can say "no more are coming" but cannot say which
chunk was last — a log, say — `close()` is closure without finality.

## Putting it together

```python
import asyncio
import a11


async def main() -> None:
    node = a11.AsyncNode.create("tokens")

    for word in ["A11", "streams", "everything"]:
        await node.put(word)
    await node.finalize()

    async for token in node:
        print(token)


asyncio.run(main())
```

Next, [carry a node's data over a network](echo-session.md) with a session.
