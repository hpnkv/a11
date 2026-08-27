# Exchange durable streams through Redis

An [`AsyncNode`][a11.nodes.async_node.AsyncNode] does not have to keep its
chunks in the process that created it. Give two nodes Redis-backed stores and
two programs can use them as a small, durable chat: Alice writes one stream and
listens to the other, while Bob does the reverse.

This guide runs the same `chat.py` on two imaginary machines. Both machines can
reach the same Redis deployment, but they do not need a direct connection to
each other.

Use this pattern when named streams must outlive either process or accept data
before the reader starts. Use a [session](echo-session.md) when peers need to
discover and dispatch actions over a live connection.

| Redis-backed node | Writer | Reader |
| --- | --- | --- |
| `alice_to_bob_messages` | Alice | Bob |
| `bob_to_alice_messages` | Bob | Alice |

## 1. Point both machines at Redis

Set the same Redis URL and chunk-store key prefix on Alice's and Bob's
machines:

```console
export A11_REDIS_URL=redis://chat-redis.internal:6379/0
export A11_REDIS_CHUNK_STORE_KEY_PREFIX=a11:chat-demo-001:
```

The default Redis client reads this configuration. The key prefix gives this
conversation its own namespace; use a new prefix for another run. Redis chunk
stores are persistent, and graceful shutdown permanently seals both message
streams in this example.

## 2. Build every store with the default client

Both deployments define the same
[`ChunkStoreFactory`][a11.stores.chunk_store.ChunkStoreFactory]. It receives a
node ID and returns a
[`RedisChunkStore`][a11.stores.redis_chunk_store.RedisChunkStore] composed with
the process-global client:

```python
def redis_chunk_store_factory(node_id: str) -> a11.ChunkStore:
    return a11.RedisChunkStore(
        node_id,
        client=a11.default_redis_client(),
    )
```

`default_redis_client()` returns the same client on every call within one
process, so the two stores share its command and Pub/Sub connections. Alice and
Bob still have separate process-local clients; the environment makes both
clients connect to the same Redis deployment.

## 3. Create both directional nodes

Each process creates both named nodes with that factory:

```python
alice_to_bob_messages = a11.AsyncNode.create(
    "alice_to_bob_messages",
    chunk_store_factory=redis_chunk_store_factory,
)
bob_to_alice_messages = a11.AsyncNode.create(
    "bob_to_alice_messages",
    chunk_store_factory=redis_chunk_store_factory,
)
```

The `AsyncNode` objects themselves are local. Their IDs select the same Redis
streams on both machines, which is what joins the two deployments. Alice uses
the first node as her outgoing stream and the second as her incoming stream;
Bob swaps those roles.

## 4. Listen without blocking the sender

The listener consumes the other party's node with `async for`. When that node
receives its final marker, iteration ends and the listener announces that its
owner has left:

```python
async def listen(incoming: a11.AsyncNode, sender: str) -> None:
    async for message in incoming:
        print(f"\n{sender}> {message}", flush=True)
    print(f"\n[{sender}] has quit the chat", flush=True)
```

Start it with `asyncio.create_task()` before entering the input loop. Terminal
input is moved to a worker thread with `asyncio.to_thread()`, leaving the event
loop free to print incoming messages immediately:

```python
listener_task = asyncio.create_task(listen(incoming, peer_name))

while True:
    try:
        message = await asyncio.to_thread(input, f"{local_name}> ")
    except EOFError:
        break

    if message.strip() == "/quit":
        break
    if not message:
        continue

    await outgoing.put(message)
```

The `await` admits the value to the node's bounded writer queue, applying
backpressure. The returned store confirmation does not need to be awaited here:
the node is drained when this producer finishes.

## 5. Finalize

Leaving the input loop—by typing `/quit`, sending end-of-file, or unwinding
through an error—runs this cleanup in a `finally` block:

```python
async def finish(outgoing: a11.AsyncNode) -> None:
    await outgoing.finalize(wait=True)
```

With no value, `finalize()` appends a final marker without adding a visible chat
message. The peer's `async for` sees that marker as end-of-stream and reaches
its quit announcement. The same call then flushes anything still queued, closes
the store with an OK status, and prevents later writes — in that order, which
guarantees the peer reads every accepted message before it reports the
departure. `wait=True` because this is a program on its way out: it should not
exit before the store has both.

## 6. Put the program together

Save this as `chat.py` on both machines. The command-line role changes only
which node is incoming and which is outgoing; both processes construct both
nodes.

```python
import argparse
import asyncio
from contextlib import suppress

import a11


def redis_chunk_store_factory(node_id: str) -> a11.ChunkStore:
    return a11.RedisChunkStore(
        node_id,
        client=a11.default_redis_client(),
    )


async def listen(incoming: a11.AsyncNode, sender: str) -> None:
    async for message in incoming:
        print(f"\n{sender}> {message}", flush=True)
    print(f"\n[{sender}] has quit the chat", flush=True)


async def send(outgoing: a11.AsyncNode, sender: str) -> None:
    while True:
        try:
            message = await asyncio.to_thread(input, f"{sender}> ")
        except EOFError:
            return

        if message.strip() == "/quit":
            return
        if not message:
            continue

        await outgoing.put(message)


async def finish(outgoing: a11.AsyncNode) -> None:
    await outgoing.finalize(wait=True)


async def chat(role: str) -> None:
    redis_client = a11.default_redis_client()
    await redis_client.ready()

    alice_to_bob_messages = a11.AsyncNode.create(
        "alice_to_bob_messages",
        chunk_store_factory=redis_chunk_store_factory,
    )
    bob_to_alice_messages = a11.AsyncNode.create(
        "bob_to_alice_messages",
        chunk_store_factory=redis_chunk_store_factory,
    )

    if role == "alice":
        local_name = "Alice"
        peer_name = "Bob"
        outgoing = alice_to_bob_messages
        incoming = bob_to_alice_messages
    else:
        local_name = "Bob"
        peer_name = "Alice"
        outgoing = bob_to_alice_messages
        incoming = alice_to_bob_messages

    print(f"[{local_name}] connected; type /quit to leave", flush=True)
    listener_task = asyncio.create_task(
        listen(incoming, peer_name),
        name=f"listen-for-{peer_name.lower()}",
    )

    try:
        await send(outgoing, local_name)
    finally:
        try:
            await finish(outgoing)
        finally:
            listener_task.cancel()
            try:
                with suppress(asyncio.CancelledError):
                    await listener_task
            finally:
                redis_client.close()


def parse_args() -> str:
    parser = argparse.ArgumentParser()
    parser.add_argument("role", choices=("alice", "bob"))
    return parser.parse_args().role


if __name__ == "__main__":
    asyncio.run(chat(parse_args()))
```

Alice starts her copy with:

```console
python chat.py alice
```

Bob starts his with:

```console
python chat.py bob
```

Either side may start first. A listener waiting on an empty Redis-backed node
sleeps until data arrives, and messages written before the other process starts
remain available when it connects.

## 7. See the conversation from both terminals

Suppose Alice starts first, they exchange three messages, and Alice types
`/quit`. Her terminal could look like this (prompts are shown without incidental
redraws from concurrent output):

```console
$ python chat.py alice
[Alice] connected; type /quit to leave
Alice> Hi Bob—did the deployment finish?
Bob> Yes. The new worker is healthy.
Alice> Great. I will check the traces.
Bob> Let me know if anything looks odd.
Alice> /quit
```

On Bob's machine the same conversation arrives through
`alice_to_bob_messages`. Alice's invisible final marker ends his listener and
produces the last line:

```console
$ python chat.py bob
[Bob] connected; type /quit to leave
Alice> Hi Bob—did the deployment finish?
Bob> Yes. The new worker is healthy.
Alice> Great. I will check the traces.
Bob> Let me know if anything looks odd.

[Alice] has quit the chat
Bob> /quit
```

If Bob leaves first, Alice sees the symmetric notification:

```text
[Bob] has quit the chat
```

The two stable node IDs, the shared Redis-backed factory, and normal
`AsyncNode` streaming and lifecycle operations are the whole distributed
boundary.
