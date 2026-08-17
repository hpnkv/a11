# Copyright 2026 The A11 Authors.

"""ChunkStore: what a node's data actually costs to write down and read back.

The store is the one part of A11 a deployment genuinely chooses. In-memory is
free and forgets; SQLite survives a restart; Redis survives the machine. The
question this suite answers is *what each of those costs per fragment*, so that
"make node streams durable" can be priced rather than argued about.

Measured per backend:

* **put** -- one fragment at a time, latency percentiles and rate. This is the
  rate a producer can append at, and for the durable backends it is the number
  a commit-per-write design lives or dies by.
* **put_many** -- the same fragments in batches. The ratio against `put` is the
  whole argument for batching, in one number.
* **get / get_by_arrival_order** -- random and arrival-ordered reads of data
  already there, which is the replay path.
* **next drain** -- the sequential consumer, which is what a reader in a node
  does, in items/s.
* **fanout** -- several `next` consumers on one store, to see whether the
  contract's shared-cursor semantics scale or serialise.
* **memory** -- resident bytes per stored fragment, which is what says how much
  a process can hold before it is a problem.
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import shutil
import tempfile
import uuid
from collections.abc import AsyncIterator, Callable

from a11 import timing
from a11.data import types
from a11.stores.local_chunk_store import LocalChunkStore
from a11.stores.sqlite_chunk_store import SQLiteChunkStoreFactory
from bench.harness import (
    Result,
    benchmark,
    latency,
    memory_slope,
    pipelined,
    throughput,
)

SUITE = "stores"

#: Long enough that a slow durable backend does not fail the benchmark, short
#: enough that a genuine hang does.
_DEADLINE = timing.Duration.seconds(30)


def _deadline() -> timing.Time:
    return timing.now() + _DEADLINE


def _fragment(seq: int, payload: bytes, *, final: bool = False):
    return types.NodeFragment(
        data=types.Chunk(data=payload), seq=seq, continued=not final
    )


def _scaled(count: int, scale: float) -> int:
    return max(int(count * scale), 4)


# --------------------------------------------------------------------------
# Backends
# --------------------------------------------------------------------------


class _Backend:
    """A named way to make a store, plus whatever it needs torn down."""

    def __init__(self, name: str, open_store: Callable[[str], object]) -> None:
        self.name = name
        self.open = open_store

    async def aclose(self) -> None:
        return None


@contextlib.asynccontextmanager
async def _backends(include_slow: bool = True) -> AsyncIterator[list[_Backend]]:
    """Every backend this machine can offer, with temp roots cleaned up."""
    made: list[_Backend] = [_Backend("local", LocalChunkStore)]
    root = tempfile.mkdtemp(prefix="a11-bench-sqlite-")
    factory = SQLiteChunkStoreFactory(root)
    made.append(_Backend("sqlite", factory.open))

    redis_client = None
    if include_slow:
        redis_client = await _redis_client()
        if redis_client is not None:
            from a11.stores.redis_chunk_store import (
                RedisChunkStore,
                RedisChunkStoreOptions,
            )

            prefix = f"a11:bench:{uuid.uuid4().hex}:"

            def open_redis(node_id: str, client=redis_client, prefix=prefix):
                options = RedisChunkStoreOptions(key_prefix=prefix)
                return RedisChunkStore(node_id, client, options)

            made.append(_Backend("redis", open_redis))
    try:
        yield made
    finally:
        if redis_client is not None:
            redis_client.close()
        shutil.rmtree(root, ignore_errors=True)


async def _redis_client():
    """A live Redis, or None -- absence is a skip, never a failure."""
    from a11.redis.client import RedisClient, RedisClientOptions

    url = os.environ.get("A11_TEST_REDIS_URL")
    options = (
        RedisClientOptions.from_url(url) if url else RedisClientOptions()
    )
    options.connect_timeout = timing.Duration.milliseconds(250)
    options.command_timeout = timing.Duration.seconds(5)
    client = RedisClient(options)
    try:
        await client.ready()
    except Exception:  # noqa: BLE001 - any failure means "no Redis here"
        client.close()
        return None
    return client


async def _initialise(store) -> None:
    """Redis stores need one round trip before they will take a write."""
    initialize = getattr(store, "initialize", None)
    if initialize is not None:
        await initialize()


_STORE_COUNTER = iter(range(1 << 30))


def _node_id(backend: str, what: str) -> str:
    return f"bench-{backend}-{what}-{next(_STORE_COUNTER)}"


# --------------------------------------------------------------------------
# Benchmarks
# --------------------------------------------------------------------------


@benchmark(SUITE, "put_latency")
async def put_latency(scale: float) -> list[Result]:
    """Single-fragment append: the rate one producer can sustain."""
    results = []
    async with _backends() as backends:
        for backend in backends:
            for size in (64, 4096):
                iterations = _scaled(
                    2000 if backend.name == "local" else 400, scale
                )
                store = backend.open(_node_id(backend.name, "put"))
                await _initialise(store)
                payload = b"x" * size
                metrics = await latency(
                    lambda index, s=store, p=payload: s.put(
                        _fragment(index, p)
                    ),
                    iterations=iterations,
                    warmup=min(iterations // 10, 50),
                )
                metrics["mib_per_s"] = (
                    metrics["ops_per_s"] * size / (1024 * 1024)
                )
                results.append(
                    Result(
                        SUITE,
                        "put",
                        metrics,
                        {"backend": backend.name, "size": f"{size}B"},
                    )
                )
    return results


@benchmark(SUITE, "put_throughput")
async def put_throughput(scale: float) -> list[Result]:
    """Append rate with writes in flight, which is what a real producer does.

    `put_latency` charges every append an event-loop turn; see the `runtime`
    suite for how large that is. A producer with a window of appends
    outstanding pays it once for the window, and this is the rate it gets.
    """
    results = []
    async with _backends() as backends:
        for backend in backends:
            for window in (1, 32, 256):
                # Redis used to be skipped here because concurrent puts
                # deadlocked the process. That was the GIL/fibre deadlock, not
                # anything about Redis, and it is fixed; the smaller budget is
                # just because a network round trip per put is slow.
                budget = {"local": 4000, "sqlite": 800}.get(backend.name, 300)
                iterations = _scaled(budget, scale)
                store = backend.open(
                    _node_id(backend.name, f"putwin{window}")
                )
                await _initialise(store)
                payload = b"x" * 256
                metrics = await pipelined(
                    lambda index, s=store, p=payload: s.put(
                        _fragment(index, p)
                    ),
                    iterations=iterations,
                    window=window,
                    warmup=0,
                    per_op_bytes=256,
                )
                results.append(
                    Result(
                        SUITE,
                        "put_in_flight",
                        metrics,
                        {"backend": backend.name, "in_flight": window},
                    )
                )
    return results


@benchmark(SUITE, "put_many_batching")
async def put_many_batching(scale: float) -> list[Result]:
    """Batched append against single append -- the price of a commit."""
    results = []
    async with _backends() as backends:
        for backend in backends:
            for batch in (8, 64, 256):
                calls = _scaled(
                    400 if backend.name == "local" else 60, scale
                )
                store = backend.open(_node_id(backend.name, f"many{batch}"))
                await _initialise(store)
                payload = b"x" * 256

                def put_batch(index, s=store, b=batch, p=payload):
                    start = index * b
                    return s.put_many(
                        [_fragment(start + i, p) for i in range(b)]
                    )

                metrics = await throughput(
                    put_batch,
                    iterations=calls,
                    warmup=max(calls // 10, 1),
                    per_op_items=batch,
                    per_op_bytes=batch * 256,
                )
                results.append(
                    Result(
                        SUITE,
                        "put_many",
                        metrics,
                        {"backend": backend.name, "batch": batch},
                    )
                )
    return results


@benchmark(SUITE, "read_paths")
async def read_paths(scale: float) -> list[Result]:
    """get by sequence, get by arrival order, and the sequential drain."""
    results = []
    async with _backends() as backends:
        for backend in backends:
            count = _scaled(2000 if backend.name == "local" else 500, scale)
            store = backend.open(_node_id(backend.name, "read"))
            await _initialise(store)
            payload = b"x" * 256
            for start in range(0, count, 200):
                chunk = min(200, count - start)
                await store.put_many(
                    [
                        _fragment(
                            start + i, payload, final=start + i == count - 1
                        )
                        for i in range(chunk)
                    ]
                )

            deadline = _deadline()
            results.append(
                Result(
                    SUITE,
                    "get_by_seq",
                    await latency(
                        lambda index, s=store, c=count: s.get(
                            index % c, deadline=deadline
                        ),
                        iterations=count,
                        warmup=min(count // 10, 50),
                    ),
                    {"backend": backend.name},
                )
            )
            results.append(
                Result(
                    SUITE,
                    "get_by_arrival",
                    await latency(
                        lambda index, s=store, c=count: s.get_by_arrival_order(
                            index % c, deadline=deadline
                        ),
                        iterations=count,
                        warmup=min(count // 10, 50),
                    ),
                    {"backend": backend.name},
                )
            )

            # The drain is one pass over the whole store, so it is timed as a
            # whole rather than per call: `next` returns a batch whose size the
            # store chooses, and per-call latency would say nothing.
            drained = 0
            import time as _time

            started = _time.perf_counter_ns()
            while True:
                batch = await store.next(deadline=deadline, limit=64)
                drained += sum(1 for fragment in batch if fragment is not None)
                if batch and batch[-1] is None:
                    break
            elapsed = (_time.perf_counter_ns() - started) / 1e9
            results.append(
                Result(
                    SUITE,
                    "next_drain",
                    {
                        "items_per_s": drained / elapsed,
                        "ops_per_s": drained / elapsed,
                        "elapsed_s": elapsed,
                        "mib_per_s": drained * 256 / elapsed / (1024 * 1024),
                    },
                    {"backend": backend.name},
                    note=f"{drained} fragments, limit=64",
                )
            )
    return results


@benchmark(SUITE, "fanout")
async def fanout(scale: float) -> list[Result]:
    """Several `next` consumers on one store: shared cursor, shared cost.

    `next` hands each fragment to exactly one consumer, so this is not a
    replication benchmark -- it is the question of whether N consumers get
    through the log N times faster, or whether they queue behind one another.
    """
    results = []
    async with _backends(include_slow=False) as backends:
        for backend in backends:
            for consumers in (1, 4, 16):
                count = _scaled(4000, scale)
                store = backend.open(
                    _node_id(backend.name, f"fanout{consumers}")
                )
                await _initialise(store)
                payload = b"x" * 128
                for start in range(0, count, 250):
                    size = min(250, count - start)
                    await store.put_many(
                        [
                            _fragment(
                                start + i,
                                payload,
                                final=start + i == count - 1,
                            )
                            for i in range(size)
                        ]
                    )

                deadline = _deadline()

                async def consume(s=store, d=deadline) -> int:
                    seen = 0
                    while True:
                        batch = await s.next(deadline=d, limit=32)
                        seen += sum(1 for f in batch if f is not None)
                        if batch and batch[-1] is None:
                            return seen

                import time as _time

                started = _time.perf_counter_ns()
                counts = await asyncio.gather(
                    *(consume() for _ in range(consumers))
                )
                elapsed = (_time.perf_counter_ns() - started) / 1e9
                results.append(
                    Result(
                        SUITE,
                        "next_fanout",
                        {
                            "items_per_s": sum(counts) / elapsed,
                            "ops_per_s": sum(counts) / elapsed,
                            "elapsed_s": elapsed,
                        },
                        {"backend": backend.name, "consumers": consumers},
                    )
                )
    return results


@benchmark(SUITE, "resident_memory")
async def resident_memory(scale: float) -> list[Result]:
    """Bytes held per stored fragment, and per empty store.

    The empty-store number is the one that decides how many nodes a process can
    have open; the per-fragment number decides how long each may live.
    """
    results = []
    stage = _scaled(2000, scale)
    counts = [stage] * 6

    made = iter(range(1 << 30))
    slope, trail = await memory_slope(
        lambda count: [
            LocalChunkStore(f"empty-{next(made)}") for _ in range(count)
        ],
        counts=counts,
    )
    results.append(
        Result(
            SUITE,
            "empty_store",
            {"bytes_each": slope},
            {"backend": "local"},
            note=trail,
        )
    )

    payload = b"x" * 256
    store = LocalChunkStore("resident")
    written = iter(range(1 << 30))

    async def append(count: int, s=store) -> int:
        for start in range(0, count, 500):
            size = min(500, count - start)
            await s.put_many(
                [_fragment(next(written), payload) for _ in range(size)]
            )
        return count

    slope, trail = await memory_slope(
        append, counts=[_scaled(20_000, scale)] * 6
    )
    results.append(
        Result(
            SUITE,
            "stored_fragment",
            {"bytes_each": slope},
            {"backend": "local", "payload": "256B"},
            note=f"256 payload bytes; the rest is bookkeeping. {trail}",
        )
    )
    return results


@benchmark(SUITE, "waiter_wakeup", slow=False)
async def waiter_wakeup(scale: float) -> list[Result]:
    """How fast a blocked reader learns that its fragment arrived.

    This is the latency that shows up as "time to first token" in anything
    streaming: a consumer parked in `get`, a producer writing the fragment it
    is parked on, and the gap between the two.
    """
    results = []
    async with _backends(include_slow=False) as backends:
        for backend in backends:
            iterations = _scaled(400, scale)
            store = backend.open(_node_id(backend.name, "wakeup"))
            await _initialise(store)
            payload = b"x" * 64
            deadline = _deadline()

            async def one(index, s=store, p=payload, d=deadline):
                waiting = asyncio.ensure_future(s.get(index, deadline=d))
                await asyncio.sleep(0)
                await s.put(_fragment(index, p))
                await waiting

            metrics = await latency(
                one, iterations=iterations, warmup=min(iterations // 10, 20)
            )
            results.append(
                Result(
                    SUITE,
                    "blocked_get_wakeup",
                    metrics,
                    {"backend": backend.name},
                    note="park, write, wake -- includes one event-loop turn",
                )
            )
    return results
