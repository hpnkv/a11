# Copyright 2026 The A11 Authors.

"""AsyncNode: the stream everything above the store is written against.

A node is a store plus a reader, a writer, and the serialization registry, and
each of those adds cost the store alone does not have. The point of this suite
is to say *how much*, so that a slow port can be attributed to the right layer.

Measured:

* **create** -- nodes per second, and resident bytes each. Actions create two
  nodes per port, so this is a per-call cost, not a startup cost.
* **put / next** -- the object path (`put(value)`, `next()`), which pays the
  chunk codec, against the chunk path (`put_chunk`, `next_chunk`), which does
  not. The gap is exactly the codec tax from the `data` suite, arriving where
  it is actually paid.
* **round trip** -- write then read one value, sequentially and pipelined.
  This is the unit a streaming producer/consumer pair moves in.
* **consume** -- draining a whole node with `iter_chunks`/`async for`, which is
  what an action's output port reader does.
* **fanout** -- one node, several readers, since a port mirrored to several
  places is the common shape in a flow.
"""

from __future__ import annotations

import asyncio

from a11 import timing
from a11.data import types
from a11.nodes.async_node import AsyncNode
from bench.harness import (
    Result,
    benchmark,
    latency,
    memory_slope,
    pipelined,
    throughput,
)

SUITE = "nodes"

_DEADLINE = timing.Duration.seconds(30)


def _scaled(count: int, scale: float) -> int:
    return max(int(count * scale), 8)


def _node(name: str) -> AsyncNode:
    return AsyncNode.create(name)


_IDS = iter(range(1 << 30))


def _fresh(prefix: str) -> AsyncNode:
    return _node(f"bench-{prefix}-{next(_IDS)}")


@benchmark(SUITE, "create")
async def create(scale: float) -> list[Result]:
    """Nodes per second, and resident bytes per live node."""
    iterations = _scaled(20_000, scale)
    metrics = await throughput(
        lambda index: _node(f"created-{index}"), iterations=iterations
    )
    slope, trail = await memory_slope(
        lambda count: [_fresh("resident") for _ in range(count)],
        counts=[_scaled(2000, scale)] * 6,
    )
    return [
        Result(SUITE, "node_create", metrics, {}),
        Result(
            SUITE,
            "node_resident",
            {"bytes_each": slope},
            {},
            note=f"idle node, in-memory store. {trail}",
        ),
    ]


#: A chunk with a mimetype, so the object reader can decode it too. A raw
#: `Chunk(data=...)` has no metadata and is undecodable -- see the
#: `text/plain has no deserializer` rule.
_TOKEN_CHUNK = types.Chunk(
    data=b'{"seq":0,"text":"a token"}',
    metadata=types.ChunkMetadata(mimetype="application/json"),
)
_TOKEN_VALUE = {"seq": 0, "text": "a token"}


@benchmark(SUITE, "write_paths")
async def write_paths(scale: float) -> list[Result]:
    """The object path against the chunk path, admitted and confirmed.

    Two independent axes, and both matter:

    * `put(value)` serializes through the registry, `put_chunk(chunk)` does
      not. The gap is what handing a node a Python object costs over handing
      it bytes.
    * A `put` is two awaits. The first returns when the writer's bounded
      admission buffer has taken the value; the second, on the future it
      returns, when the backing store has actually accepted it. A producer
      that only awaits the first is running ahead of its store on purpose, and
      the two numbers are that different on purpose.
    """
    iterations = _scaled(3000, scale)
    results = []

    async def confirmed(call) -> None:
        await (await call)

    for path, write in (
        ("object", lambda n: n.put(_TOKEN_VALUE)),
        ("chunk", lambda n: n.put_chunk(_TOKEN_CHUNK)),
    ):
        for stage, wrap in (
            ("admitted", lambda call: call),
            ("confirmed", confirmed),
        ):
            node = _fresh(f"write-{path}-{stage}")
            results.append(
                Result(
                    SUITE,
                    "put",
                    await latency(
                        lambda _index, n=node, w=write, wr=wrap: wr(w(n)),
                        iterations=iterations,
                        warmup=200,
                    ),
                    {"path": path, "stage": stage},
                )
            )
        # Several windows, not just a deep one.
        #
        # A deep window amortises whatever a single put pays, so measuring only
        # 256 says nothing about the shape in between -- and the shape is not
        # monotonic. Two writes in flight is the worst case and the most common
        # one: a handler writing to a couple of ports does exactly that.
        for window in (2, 8, 32, 256):
            node = _fresh(f"write-{path}-pipelined-{window}")
            results.append(
                Result(
                    SUITE,
                    "put_in_flight",
                    await pipelined(
                        lambda _index, n=node, w=write: confirmed(w(n)),
                        iterations=iterations,
                        window=window,
                        warmup=100,
                    ),
                    {"path": path, "in_flight": window},
                )
            )
    return results


@benchmark(SUITE, "read_paths")
async def read_paths(scale: float) -> list[Result]:
    """Draining a filled node, object path against chunk path."""
    count = _scaled(20_000, scale)
    results = []
    chunk = _TOKEN_CHUNK

    for label in ("object", "chunk"):
        node = _fresh(f"read-{label}")
        async with node as writer:
            for _index in range(count - 1):
                await writer.put_chunk(chunk)
            await (await writer.put_chunk(chunk, final=True))

        import time as _time

        seen = 0
        started = _time.perf_counter_ns()
        if label == "object":
            async for _value in node:
                seen += 1
        else:
            async for _piece in node.iter_chunks():
                seen += 1
        elapsed = (_time.perf_counter_ns() - started) / 1e9
        results.append(
            Result(
                SUITE,
                "drain",
                {
                    "ops_per_s": seen / elapsed,
                    "items_per_s": seen / elapsed,
                    "elapsed_s": elapsed,
                    "mib_per_s": seen * len(chunk.data) / elapsed / 1048576,
                },
                {"path": label},
                note=f"{seen} values",
            )
        )
    return results


@benchmark(SUITE, "read_batching_headroom")
async def read_batching_headroom(scale: float) -> list[Result]:
    """The node reader against the store underneath it, on the same data.

    `ChunkStore.next` takes a `limit` and returns a batch, so a consumer can
    pay one await for many fragments. `AsyncNode` has no batched read: every
    value costs an await, and an await costs an event-loop turn. This measures
    both on identical data so the gap is the headroom a batched node read
    would recover.
    """
    count = _scaled(20_000, scale)
    chunk = types.Chunk(
        data=b"x" * 128,
        metadata=types.ChunkMetadata(mimetype="application/octet-stream"),
    )
    import time as _time

    from a11.stores.local_chunk_store import LocalChunkStore

    node = _fresh("headroom-node")
    async with node as writer:
        for _index in range(count - 1):
            await writer.put_chunk(chunk)
        await (await writer.put_chunk(chunk, final=True))

    seen = 0
    started = _time.perf_counter_ns()
    while await node.next_fragment() is not None:
        seen += 1
    node_elapsed = (_time.perf_counter_ns() - started) / 1e9

    results = [
        Result(
            SUITE,
            "read_one_at_a_time",
            {
                "ops_per_s": seen / node_elapsed,
                "items_per_s": seen / node_elapsed,
                "p50_us": node_elapsed / seen * 1e6,
                "elapsed_s": node_elapsed,
            },
            {"via": "AsyncNode.next_fragment"},
        )
    ]

    for limit in (1, 64):
        store = LocalChunkStore(f"headroom-store-{limit}")
        for start in range(0, count, 500):
            size = min(500, count - start)
            await store.put_many(
                [
                    types.NodeFragment(
                        data=chunk,
                        seq=start + offset,
                        continued=start + offset < count - 1,
                    )
                    for offset in range(size)
                ]
            )
        deadline = timing.now() + _DEADLINE
        drained = 0
        started = _time.perf_counter_ns()
        while True:
            batch = await store.next(deadline=deadline, limit=limit)
            drained += sum(1 for fragment in batch if fragment is not None)
            if batch and batch[-1] is None:
                break
        elapsed = (_time.perf_counter_ns() - started) / 1e9
        results.append(
            Result(
                SUITE,
                "read_one_at_a_time" if limit == 1 else "read_batched",
                {
                    "ops_per_s": drained / elapsed,
                    "items_per_s": drained / elapsed,
                    "p50_us": elapsed / drained * 1e6,
                    "elapsed_s": elapsed,
                },
                {"via": f"ChunkStore.next(limit={limit})"},
            )
        )

    batched = results[-1].metrics["items_per_s"]
    results[-1].note = (
        f"{batched / results[0].metrics['items_per_s']:.0f}x the node's"
        " one-at-a-time read, on the same data"
    )
    return results


@benchmark(SUITE, "round_trip")
async def round_trip(scale: float) -> list[Result]:
    """Write one value, read it back: the streaming unit, both pacings.

    Sequential is the honest number for a producer that waits for its consumer.
    Pipelined is what a producer running ahead of its consumer gets, which is
    the normal shape for token streaming.
    """
    iterations = _scaled(2000, scale)
    results = []

    node = _fresh("rt-sequential")
    reader = node

    async def one(_index, n=node, r=reader):
        await n.put("token")
        await r.next()

    results.append(
        Result(
            SUITE,
            "put_then_read",
            await latency(one, iterations=iterations, warmup=100),
            {"pacing": "sequential"},
        )
    )

    node = _fresh("rt-pipelined")
    count = iterations

    async def produce(n=node, c=count):
        for index in range(c - 1):
            await n.put("token")
        await n.put_final("token")

    async def consume(n=node) -> int:
        seen = 0
        async for _value in n:
            seen += 1
        return seen

    import time as _time

    started = _time.perf_counter_ns()
    _, seen = await asyncio.gather(produce(), consume())
    elapsed = (_time.perf_counter_ns() - started) / 1e9
    results.append(
        Result(
            SUITE,
            "producer_consumer",
            {
                "ops_per_s": seen / elapsed,
                "items_per_s": seen / elapsed,
                "elapsed_s": elapsed,
            },
            {"pacing": "concurrent"},
            note=f"{seen} values through one node, writer and reader in flight",
        )
    )
    return results


@benchmark(SUITE, "reader_fanout")
async def reader_fanout(scale: float) -> list[Result]:
    """One filled node, N independent readers, each reading everything.

    A node's reader is per-reader, so this is genuine replication rather than
    the store's shared cursor. It is the shape of an output port that several
    steps of a flow are watching.
    """
    count = _scaled(5000, scale)
    chunk = types.Chunk(
        data=b"x" * 128,
        metadata=types.ChunkMetadata(mimetype="application/octet-stream"),
    )
    results = []
    for readers in (1, 4, 16):
        node = _fresh(f"fanout-{readers}")
        async with node as writer:
            for _index in range(count - 1):
                await writer.put_chunk(chunk)
            await (await writer.put_chunk(chunk, final=True))

        async def drain(n=node) -> int:
            seen = 0
            async for _piece in n.iter_chunks():
                seen += 1
            return seen

        import time as _time

        started = _time.perf_counter_ns()
        seen = await asyncio.gather(*(drain() for _ in range(readers)))
        elapsed = (_time.perf_counter_ns() - started) / 1e9
        results.append(
            Result(
                SUITE,
                "replicated_read",
                {
                    "ops_per_s": sum(seen) / elapsed,
                    "items_per_s": sum(seen) / elapsed,
                    "elapsed_s": elapsed,
                },
                {"readers": readers},
                note=f"{sum(seen)} reads total",
            )
        )
    return results
