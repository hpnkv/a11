# Copyright 2026 The A11 Authors.

"""The floor: what an `await` costs before A11 is involved at all.

Run this first and read it first. Every asynchronous number in every other
suite is bounded by these, and without them a reader will attribute the
event loop's cost to A11.

Three floors, in increasing order of what they include:

* **bare coroutine** -- awaiting a Python coroutine that returns immediately.
  Tens of nanoseconds; not a floor on anything, but it says how much of a
  measurement is the language.
* **event-loop turn** -- `asyncio.sleep(0)`, which is one full pass of the
  selector. On a selector loop that is a syscall, and it is the real floor on
  *sequential* async throughput: a caller awaiting one thing at a time cannot
  beat it no matter how fast the thing is.
* **native await** -- the cheapest asynchronous call that crosses into C++ and
  back (`ChunkStore.size`). Its excess over the loop turn is A11's
  marshalling: releasing the GIL, handing to a fiber, and marshalling the
  result back to the captured loop.

The last one is the number to watch. It is charged once per `await` on every
node read, every store write, and every action port operation in the system.
"""

from __future__ import annotations

import asyncio
import time

from a11.data import types
from a11.stores.local_chunk_store import LocalChunkStore
from bench.harness import (
    Result,
    benchmark,
    latency,
    percentiles,
    pipelined,
)

SUITE = "runtime"


def _scaled(count: int, scale: float) -> int:
    return max(int(count * scale), 100)


def _nothing() -> None:
    return None


@benchmark(SUITE, "await_floor")
async def await_floor(scale: float) -> list[Result]:
    """Bare coroutine, event-loop turn, and the cheapest native await."""
    iterations = _scaled(20_000, scale)
    store = LocalChunkStore("floor")
    await store.put(
        types.NodeFragment(
            data=types.Chunk(data=b"x"), seq=0, continued=True
        )
    )

    async def nothing() -> int:
        return 1

    cases = [
        ("bare_coroutine", lambda _i: nothing()),
        ("event_loop_turn", lambda _i: asyncio.sleep(0)),
        ("native_await", lambda _i: store.size()),
        # Not a floor -- an alternative A11 actually takes. `AsyncNode.put`
        # offloads serialization here, so anything written through a node pays
        # this before it pays the codec.
        ("thread_offload", lambda _i: asyncio.to_thread(_nothing)),
    ]
    results = []
    for name, operation in cases:
        results.append(
            Result(
                SUITE,
                name,
                await latency(
                    operation,
                    iterations=iterations,
                    warmup=iterations // 10,
                ),
                {},
            )
        )

    turn = results[1].metrics["p50_us"]
    native = results[2].metrics["p50_us"]
    results[2].note = (
        f"{native - turn:.1f}us above an event-loop turn -- that excess is"
        " A11's per-await marshalling"
    )
    return results


@benchmark(SUITE, "loop_turn_anatomy")
async def loop_turn_anatomy(scale: float) -> list[Result]:
    """Where an event-loop turn's microseconds actually go.

    This decides how to read every other Python number, and it is
    platform-specific enough that it must be measured rather than assumed.
    A turn is: run the ready callbacks, then poll for I/O. Awaiting a future
    that is *already* resolved skips the turn entirely and costs ~0.2us, so
    the turn is not Python's scheduling machinery. Polling is.

    On macOS the `kqueue` call with a zero timeout is expensive -- it has
    measured at ~13.6us of a ~17.7us turn on the reference machine -- and
    uvloop does not avoid it, because it polls too. On a platform where the
    poll is cheap (a typical Linux `epoll_wait` is a microsecond or two) the
    turn collapses and **A11's own ~5.4us of per-await marshalling becomes the
    majority of every await**.

    So: do not read "the event loop dominates" as a universal truth. It is
    true here. Where the poll is cheap, the marshalling dominates instead, and
    reducing awaits per operation is worth correspondingly more.
    """
    iterations = _scaled(20_000, scale)
    loop = asyncio.get_running_loop()
    results = []

    resolved = loop.create_future()
    resolved.set_result(1)

    async def await_resolved() -> None:
        await asyncio.shield(resolved)

    results.append(
        Result(
            SUITE,
            "await_already_resolved",
            await latency(
                lambda _i: await_resolved(),
                iterations=iterations,
                warmup=iterations // 10,
            ),
            {},
            note="no loop turn at all -- this is Python's scheduling cost",
        )
    )

    async def call_soon_round_trip() -> None:
        future = loop.create_future()
        loop.call_soon(future.set_result, None)
        await future

    results.append(
        Result(
            SUITE,
            "call_soon_round_trip",
            await latency(
                lambda _i: call_soon_round_trip(),
                iterations=iterations,
                warmup=iterations // 10,
            ),
            {},
            note="one full turn, scheduled the way A11's callbacks are",
        )
    )

    # The poll itself, with no Python scheduling around it. uvloop has no
    # equivalent handle to reach, so this is selector-loop only.
    selector = getattr(loop, "_selector", None)
    if selector is not None:
        samples = []
        for _ in range(min(iterations, 20_000)):
            started = time.perf_counter_ns()
            selector.select(0)
            samples.append(time.perf_counter_ns() - started)
        stats = percentiles(samples)
        stats["ops_per_s"] = 1e6 / stats["mean_us"]
        results.append(
            Result(
                SUITE,
                "io_poll_syscall",
                stats,
                {},
                note=(
                    "the kernel poll a turn ends with; on macOS this is most"
                    " of the turn, on Linux it is not"
                ),
            )
        )
    return results


@benchmark(SUITE, "await_pipelining")
async def await_pipelining(scale: float) -> list[Result]:
    """How much of the floor a caller wins back by having work in flight.

    The same native call, awaited one at a time and then with a window of them
    outstanding. The ratio is the argument for writing `gather` rather than a
    `for` loop at every A11 call site that has more than one thing to do.
    """
    iterations = _scaled(20_000, scale)
    store = LocalChunkStore("pipelining")
    results = []
    for window in (1, 8, 64, 512):
        metrics = await pipelined(
            lambda _i: store.size(),
            iterations=iterations,
            window=window,
            warmup=1000,
        )
        results.append(
            Result(SUITE, "native_await", metrics, {"in_flight": window})
        )
    sequential = results[0].metrics["ops_per_s"]
    best = max(result.metrics["ops_per_s"] for result in results)
    results[-1].note = f"{best / sequential:.1f}x over one-at-a-time"
    return results
