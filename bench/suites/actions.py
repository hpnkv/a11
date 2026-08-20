# Copyright 2026 The A11 Authors.

"""Actions: the unit A11 is counted in.

"Actions per second" is the number a person asks for first, and it has three
honest answers depending on where the action runs, so all three are here:

* **local** -- built, run and awaited in this process, no session and no
  stream. This is the composition path: a flow's `run` step, a tool a client
  invokes on itself. It is the ceiling for the other two.
* **dispatched over a session** -- the real remote path, over an in-process
  wire stream pair so the transport contributes as little as possible. The gap
  against local is the cost of the session, the envelope and the node
  mirroring.
* **concurrent** -- many calls in flight over one session, which is what a
  gateway serving several conversations at once actually sees. Sequential
  dispatch is bounded by the round-trip latency; concurrency is bounded by the
  session, and only the second number sizes a server.

Also measured: the cost of the *ports* rather than the call -- an action with
one input and one output against one with eight of each, since a wide schema
(`make_http_request` has eight outputs) creates a node per port per call.
"""

from __future__ import annotations

import asyncio
import contextlib
import time

import a11
from a11 import net, timing
from bench.harness import Result, benchmark, latency, memory_slope, percentiles

SUITE = "actions"

_WAIT = timing.Duration.seconds(30)


def _scaled(count: int, scale: float) -> int:
    return max(int(count * scale), 8)


def _echo_schema(inputs: int = 1, outputs: int = 1) -> a11.ActionSchema:
    """An action with `inputs` unary inputs and `outputs` output ports."""
    return a11.ActionSchema(
        name=f"echo-{inputs}-{outputs}",
        description="Echo the input back.",
        inputs={
            f"in{index}": a11.ActionPortSchema(
                name=f"in{index}",
                type="text/plain",
                unary=True,
                required=index == 0,
            )
            for index in range(inputs)
        },
        outputs={
            f"out{index}": a11.ActionPortSchema(
                name=f"out{index}", type="text/plain"
            )
            for index in range(outputs)
        },
    )


def _echo_handler(inputs: int, outputs: int):
    async def handle(action: a11.Action) -> None:
        text = await action["in0"].consume(str)
        for index in range(1, inputs):
            await action[f"in{index}"].consume(str)
        for index in range(outputs):
            await action[f"out{index}"].finalize(text)

    return handle


def _registry(schema: a11.ActionSchema, handler) -> a11.ActionRegistry:
    registry = a11.ActionRegistry()
    registry.register(schema.name, schema, handler)
    return registry


async def _call_local(schema, handler, inputs: int, outputs: int) -> str:
    call = a11.Action(schema, handler=handler)
    call.run()
    for index in range(inputs):
        await call[f"in{index}"].finalize("payload")
    result = await call["out0"].consume(str)
    for index in range(1, outputs):
        await call[f"out{index}"].consume(str)
    await call.wait(_WAIT)
    return result


async def _call_local_partial(schema, handler, inputs: int, outputs: int) -> str:
    """Feed one input and read one output of a wide schema."""
    call = a11.Action(schema, handler=handler)
    call.run()
    await call["in0"].finalize("payload")
    result = await call["out0"].consume(str)
    await call.wait(_WAIT)
    return result


def _one_port_handler():
    """Touch `in0` and `out0` only, leaving the rest of the schema unused."""

    async def handle(action: a11.Action) -> None:
        text = await action["in0"].consume(str)
        await action["out0"].finalize(text)

    return handle


@benchmark(SUITE, "partial_ports")
async def partial_ports(scale: float) -> list[Result]:
    """A wide schema where only one port each way is used.

    The shape `make_http_request` has: eight outputs, of which a caller reads
    one. A port costs a node, and a node costs a reader, a writer and a store,
    so what the unused ones cost is what a schema costs for being wide rather
    than for being used. Compare against `local_action[ports=8in/8out]`, which
    touches all sixteen.
    """
    results = []
    for inputs, outputs in ((1, 1), (8, 8)):
        schema = _echo_schema(inputs, outputs)
        iterations = _scaled(600, scale)
        metrics = await latency(
            lambda _index, s=schema: _call_local_partial(
                s, _one_port_handler(), inputs, outputs
            ),
            iterations=iterations,
            warmup=20,
        )
        results.append(
            Result(
                SUITE,
                "local_action",
                metrics,
                {"ports": f"{inputs}in/{outputs}out", "used": "1in/1out"},
            )
        )
    wide = results[-1].metrics["p50_us"]
    narrow = results[0].metrics["p50_us"]
    results[-1].note = (
        f"{wide / narrow:.2f}x the 1-in/1-out call for the same work through a"
        " schema 8x as wide"
    )
    return results


@benchmark(SUITE, "local_dispatch")
async def local_dispatch(scale: float) -> list[Result]:
    """One action built, run and awaited in this process, by port count."""
    results = []
    for inputs, outputs in ((1, 1), (2, 4), (8, 8)):
        schema = _echo_schema(inputs, outputs)
        handler = _echo_handler(inputs, outputs)
        iterations = _scaled(600, scale)
        metrics = await latency(
            lambda _index: _call_local(schema, handler, inputs, outputs),
            iterations=iterations,
            warmup=20,
        )
        metrics["ports_per_s"] = metrics["ops_per_s"] * (inputs + outputs)
        results.append(
            Result(
                SUITE,
                "local_action",
                metrics,
                {"ports": f"{inputs}in/{outputs}out"},
            )
        )
    widest = results[-1].metrics["p50_us"]
    narrowest = results[0].metrics["p50_us"]
    results[-1].note = (
        f"{widest / narrowest:.1f}x the 1-in/1-out call; 16 ports means 16"
        " nodes per call"
    )
    return results


@contextlib.asynccontextmanager
async def _session(schema, handler):
    """A client session talking to a service over an in-process stream pair."""
    service = a11.Service(action_registry=_registry(schema, handler))
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.ensure_future(service.accept(server_stream))
    client = a11.Session(action_registry=a11.ActionRegistry())
    await client.add_stream(client_stream, mode="start")
    try:
        yield client, client_stream
    finally:
        with contextlib.suppress(Exception):
            client_stream.half_close()
            await client_stream.drain_outgoing_messages()
        service.abort(
            a11.Status(code=a11.StatusCode.CANCELLED, message="bench over")
        )
        with contextlib.suppress(Exception):
            await asyncio.wait_for(serving, timeout=10)


async def _call_remote(session, stream, schema, inputs, outputs) -> str:
    call = (
        a11.Action(schema)
        .bind_node_map(session.node_map)
        .bind_session(session)
        .bind_stream(stream)
    )
    await call.call()
    for index in range(inputs):
        await call[f"in{index}"].finalize("payload")
    result = await call["out0"].consume(str)
    for index in range(1, outputs):
        await call[f"out{index}"].consume(str)
    await call.wait(_WAIT)
    return result


@benchmark(SUITE, "session_dispatch")
async def session_dispatch(scale: float) -> list[Result]:
    """Actions dispatched over a session, sequentially: the latency number."""
    results = []
    for inputs, outputs in ((1, 1), (8, 8)):
        schema = _echo_schema(inputs, outputs)
        handler = _echo_handler(inputs, outputs)
        iterations = _scaled(300, scale)
        async with _session(schema, handler) as (session, stream):
            metrics = await latency(
                lambda _index: _call_remote(
                    session, stream, schema, inputs, outputs
                ),
                iterations=iterations,
                warmup=10,
            )
        results.append(
            Result(
                SUITE,
                "dispatched_action",
                metrics,
                {"ports": f"{inputs}in/{outputs}out"},
                note="in-process transport, so this is the session's own cost",
            )
        )
    return results


@benchmark(SUITE, "session_concurrency")
async def session_concurrency(scale: float) -> list[Result]:
    """Actions per second with N in flight on one session.

    This is the number that sizes a gateway. `SessionOptions` caps concurrent
    root actions at 32 by default, so the curve is expected to flatten there --
    and confirming that it flattens *at* the cap rather than before it is the
    point.
    """
    schema = _echo_schema(1, 1)
    handler = _echo_handler(1, 1)
    results = []
    async with _session(schema, handler) as (session, stream):
        for in_flight in (1, 8, 32, 128):
            total = _scaled(400, scale)
            samples: list[float] = []

            async def one() -> None:
                started = time.perf_counter_ns()
                await _call_remote(session, stream, schema, 1, 1)
                samples.append(time.perf_counter_ns() - started)

            started = time.perf_counter_ns()
            remaining = total
            while remaining > 0:
                batch = min(in_flight, remaining)
                await asyncio.gather(*(one() for _ in range(batch)))
                remaining -= batch
            elapsed = (time.perf_counter_ns() - started) / 1e9
            metrics = percentiles(samples)
            metrics["ops_per_s"] = total / elapsed
            metrics["elapsed_s"] = elapsed
            results.append(
                Result(
                    SUITE,
                    "actions_in_flight",
                    metrics,
                    {"in_flight": in_flight},
                    note=(
                        "ops/s is aggregate; percentiles are per action, so"
                        " they grow with the queue"
                    ),
                )
            )
    best = max(result.metrics["ops_per_s"] for result in results)
    results[-1].note += f". peak {best:,.0f} actions/s"
    return results


@benchmark(SUITE, "concurrency_attribution")
async def concurrency_attribution(scale: float) -> list[Result]:
    """Where the ceiling on actions/second actually is.

    `session_dispatch` flattening under concurrency has two possible causes and
    they call for opposite work: either the session serialises the calls, or
    the single Python event loop driving *both* ends is saturated and the
    session never gets the chance. Three configurations tell them apart:

    * local calls, no session at all, run concurrently;
    * one session, calls concurrent;
    * several sessions, calls concurrent across all of them.

    If local calls do not scale either, the loop is the ceiling and the fix is
    in the Python surface (fewer awaits per call), not in the session. If local
    scales and one session does not, the session is the ceiling. If one session
    does not scale but several do, the limit is per-session.
    """
    schema = _echo_schema(1, 1)
    handler = _echo_handler(1, 1)
    results = []
    total = _scaled(300, scale)

    for in_flight in (1, 32):
        started = time.perf_counter_ns()
        remaining = total
        while remaining > 0:
            batch = min(in_flight, remaining)
            await asyncio.gather(
                *(
                    _call_local(schema, handler, 1, 1)
                    for _ in range(batch)
                )
            )
            remaining -= batch
        elapsed = (time.perf_counter_ns() - started) / 1e9
        results.append(
            Result(
                SUITE,
                "local_in_flight",
                {"ops_per_s": total / elapsed, "elapsed_s": elapsed},
                {"in_flight": in_flight},
            )
        )

    for sessions in (1, 4):
        opened = []
        stack = contextlib.AsyncExitStack()
        async with stack:
            for _ in range(sessions):
                opened.append(
                    await stack.enter_async_context(_session(schema, handler))
                )
            per_session = max(total // sessions, 1)

            async def drive(pair) -> None:
                session, stream = pair
                remaining = per_session
                while remaining > 0:
                    batch = min(32, remaining)
                    await asyncio.gather(
                        *(
                            _call_remote(session, stream, schema, 1, 1)
                            for _ in range(batch)
                        )
                    )
                    remaining -= batch

            started = time.perf_counter_ns()
            await asyncio.gather(*(drive(pair) for pair in opened))
            elapsed = (time.perf_counter_ns() - started) / 1e9
        results.append(
            Result(
                SUITE,
                "sessions_in_flight",
                {
                    "ops_per_s": per_session * sessions / elapsed,
                    "elapsed_s": elapsed,
                },
                {"sessions": sessions, "in_flight": 32},
            )
        )
    return results


@benchmark(SUITE, "action_memory")
async def action_memory(scale: float) -> list[Result]:
    """Resident bytes per live action, by port count.

    An action holds a node per port and a node is not free (see the `nodes`
    suite), so a wide schema is a memory decision as much as an API one.
    """
    results = []
    for inputs, outputs in ((1, 1), (8, 8)):
        schema = _echo_schema(inputs, outputs)
        handler = _echo_handler(inputs, outputs)
        stage = _scaled(300, scale)

        def build(count, s=schema, h=handler, i=inputs, o=outputs):
            made = []
            for _ in range(count):
                call = a11.Action(s, handler=h)
                # Realising the ports is what costs; an un-run action has not
                # made its nodes yet, and a benchmark of that would flatter.
                made.append(
                    (
                        call,
                        [
                            call.get_input(f"in{n}", bind_stream=False)
                            for n in range(i)
                        ],
                        [
                            call.get_output(f"out{n}", bind_stream=False)
                            for n in range(o)
                        ],
                    )
                )
            return made

        slope, trail = await memory_slope(build, counts=[stage] * 6)
        results.append(
            Result(
                SUITE,
                "live_action",
                {"bytes_each": slope},
                {"ports": f"{inputs}in/{outputs}out"},
                note=trail,
            )
        )
    return results
