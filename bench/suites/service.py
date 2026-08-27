# Copyright 2026 The A11 Authors.

"""Service capacity: how many streams one service holds, and what it serves.

The `wire` suite counts what a transport costs. This one counts what a *served*
connection costs -- a stream with a session on it, a registry behind it and a
pump running -- because that is the thing a deployment actually has ten
thousand of.

* **accept rate** -- connections per second a service takes on. A service that
  holds many but accepts slowly fails differently from one that accepts fast
  and falls over.
* **resident bytes per live session** -- the capacity number. Multiply by the
  target connection count and compare against the host.
* **idle cost** -- whether holding N idle sessions costs CPU. A pump per
  session that wakes up for nothing is invisible at ten sessions and fatal at
  ten thousand, so this measures event-loop responsiveness with a large idle
  population.
* **aggregate throughput across connections** -- actions per second summed over
  N connections each doing work, which is the number a gateway is sized by.
"""

from __future__ import annotations

import asyncio
import contextlib
import time

import a11
from a11 import net, timing
from bench.harness import Result, benchmark, memory_slope, percentiles

SUITE = "service"

_WAIT = timing.Duration.seconds(30)

_ECHO = a11.ActionSchema(
    name="echo",
    description="Echo the input back.",
    inputs={
        "text": a11.ActionPortSchema(
            name="text", type="text/plain", unary=True, required=True
        )
    },
    outputs={"out": a11.ActionPortSchema(name="out", type="text/plain")},
)


async def _echo(action: a11.Action) -> None:
    text = await action["text"].consume(str)
    await action["out"].finalize(text)


def _registry() -> a11.ActionRegistry:
    registry = a11.ActionRegistry()
    registry.register(_ECHO.name, _ECHO, _echo)
    return registry


def _scaled(count: int, scale: float) -> int:
    return max(int(count * scale), 4)


class _Fleet:
    """A service and every client connection opened against it."""

    def __init__(self, transport: str = "in-process") -> None:
        self.service = a11.Service(action_registry=_registry())
        self.transport = transport
        self.clients: list[tuple[a11.Session, object]] = []
        self._serving: list[asyncio.Future] = []
        self._server = None
        if transport == "websocket":
            options = net.WebSocketServerOptions()
            options.path = "/bench"
            options.bind_address = "127.0.0.1"
            options.port = 0
            options.http2_options.enable_h2 = False
            options.http2_options.enable_h2c = False
            self._server = net.WebSocketWireServer.create(
                self.service.accept, options
            )

    async def connect(self) -> tuple[a11.Session, object]:
        if self.transport == "websocket":
            options = net.WebSocketClientOptions()
            options.http2_options.enable_h2 = False
            options.http2_options.enable_h2c = False
            stream = net.WebSocketWireStream.connect(
                f"ws://127.0.0.1:{self._server.port}/bench",
                websocket_options=options,
            )
        else:
            server_stream, stream = net.create_in_process_wire_stream_pair()
            self._serving.append(
                asyncio.ensure_future(self.service.accept(server_stream))
            )
        session = a11.Session(action_registry=a11.ActionRegistry())
        await session.add_stream(stream, mode="start")
        pair = (session, stream)
        self.clients.append(pair)
        return pair

    async def aclose(self) -> None:
        for _session, stream in self.clients:
            with contextlib.suppress(Exception):
                stream.half_close()
                await stream.drain_outgoing_messages()
        if self._server is not None:
            self._server.stop()
        self.service.abort(
            a11.Status(code=a11.StatusCode.CANCELLED, message="bench over")
        )
        for task in self._serving:
            task.cancel()
        await asyncio.gather(*self._serving, return_exceptions=True)


async def _echo_call(session, stream) -> str:
    call = (
        a11
        .Action(_ECHO)
        .bind_node_map(session.node_map)
        .bind_session(session)
        .bind_stream(stream)
    )
    await call.call()
    await call["text"].finalize("payload")
    result = await call["out"].consume(str)
    await call.wait(_WAIT)
    return result


@benchmark(SUITE, "session_capacity")
async def session_capacity(scale: float) -> list[Result]:
    """Live sessions per service: resident bytes each and the accept rate."""
    results = []
    for transport in ("in-process", "websocket"):
        fleet = _Fleet(transport)
        stage = _scaled(100 if transport == "in-process" else 60, scale)
        try:
            started = time.perf_counter_ns()

            async def open_many(count: int, f=fleet) -> int:
                for _ in range(count):
                    await f.connect()
                return count

            slope, trail = await memory_slope(open_many, counts=[stage] * 6)
            elapsed = (time.perf_counter_ns() - started) / 1e9
            total = stage * 6
            results.append(
                Result(
                    SUITE,
                    "live_session",
                    {
                        "bytes_each": slope,
                        "ops_per_s": total / elapsed,
                        "elapsed_s": elapsed,
                    },
                    {"transport": transport},
                    note=(
                        f"{total} sessions served at once ("
                        f"service reports {fleet.service.session_count});"
                        f" ops/s is the accept rate. {trail}"
                    ),
                )
            )
        finally:
            await fleet.aclose()
    return results


@benchmark(SUITE, "idle_session_cost")
async def idle_session_cost(scale: float) -> list[Result]:
    """Does holding idle sessions cost the event loop anything?

    An idle connection should be free. If a per-session pump wakes for nothing,
    the loop's own turn time grows with the population, and that shows up as
    latency on every unrelated piece of work in the process. Measured by
    timing bare `asyncio.sleep(0)` -- work that touches no A11 object at all --
    with 0, N and 4N sessions held open.
    """
    fleet = _Fleet("in-process")
    stage = _scaled(100, scale)
    results = []
    try:
        for population in (0, stage, stage * 4):
            while len(fleet.clients) < population:
                await fleet.connect()
            samples = []
            for _ in range(2000):
                started = time.perf_counter_ns()
                await asyncio.sleep(0)
                samples.append(time.perf_counter_ns() - started)
            metrics = percentiles(samples)
            metrics["ops_per_s"] = 1e9 / metrics["mean_us"] / 1000
            results.append(
                Result(
                    SUITE,
                    "loop_turn_under_load",
                    metrics,
                    {"idle_sessions": population},
                )
            )
    finally:
        await fleet.aclose()
    base = results[0].metrics["p50_us"]
    worst = results[-1].metrics["p50_us"]
    results[-1].note = (
        f"{worst / base:.2f}x the empty-process loop turn -- anything near"
        " 1.0 means idle sessions are genuinely idle"
    )
    return results


@benchmark(SUITE, "aggregate_throughput", slow=True)
async def aggregate_throughput(scale: float) -> list[Result]:
    """Actions per second summed across N connections working at once.

    **Marked slow because it hangs**, not because it is long. Above roughly a
    dozen connections it wedges with the Python main thread parked in
    `std::condition_variable::wait` inside the extension -- the same stack, at
    the same offsets, as the Redis deadlock in `stores/put_throughput`. So it
    is one native blocking call reachable from two unrelated suites, and while
    the harness timeout cannot fire because the blocked thread runs the event
    loop. Run this benchmark only with `--slow` while investigating the issue.

    The gateway-sizing number. Each connection runs its own sequence of echo
    calls; the total is what the service delivers. Compare against
    `actions/session_concurrency`, which puts the same load down one
    connection: if spreading it over connections does not help, the ceiling is
    not the session.
    """
    results = []
    for connections in (1, 4, 16):
        fleet = _Fleet("in-process")
        try:
            pairs = [await fleet.connect() for _ in range(connections)]
            # One call per connection before the clock starts: the first call
            # on a session pays for whatever the session sets up lazily, and
            # charging that to the measurement makes wide fleets look slow for
            # a reason that has nothing to do with throughput.
            for pair in pairs:
                await _echo_call(*pair)
            per_connection = max(_scaled(400, scale) // connections, 8)
            samples: list[float] = []

            async def drive(pair, count=per_connection) -> None:
                session, stream = pair
                for _ in range(count):
                    started = time.perf_counter_ns()
                    await _echo_call(session, stream)
                    samples.append(time.perf_counter_ns() - started)

            started = time.perf_counter_ns()
            await asyncio.gather(*(drive(pair) for pair in pairs))
            elapsed = (time.perf_counter_ns() - started) / 1e9
            metrics = percentiles(samples)
            metrics["ops_per_s"] = len(samples) / elapsed
            metrics["elapsed_s"] = elapsed
            results.append(
                Result(
                    SUITE,
                    "actions_across_connections",
                    metrics,
                    {"connections": connections},
                )
            )
        finally:
            await fleet.aclose()
    first = results[0].metrics["ops_per_s"]
    best = max(result.metrics["ops_per_s"] for result in results)
    results[-1].note = (
        f"{best / first:.2f}x one connection -- linear scaling would be"
        f" {results[-1].params['connections']}x"
    )
    return results
