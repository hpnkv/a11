# Copyright 2026 The A11 Authors.

"""WireStream: what the transport costs, and how many of them there can be.

`WireStream` is the seam A11 is pluggable at, so the useful thing to measure is
not one transport but the *same* traffic on each of them, side by side. Three
questions, each with a number:

* **How fast is a round trip?** Send a message, get one back. This is the floor
  under every remote action dispatch, and the percentiles matter more than the
  mean -- a p99 of 40ms on a transport with a 2ms p50 is a queueing problem
  somebody has to know about.
* **How many messages per second, and how many bytes?** Measured against
  payload size, because the two curves cross: small messages are bounded by the
  per-message tax (framing, envelope codec, a loop turn), large ones by
  bandwidth. Where they cross tells you whether to batch.
* **How many streams can one process hold, and what does each cost?** Open N
  connections, keep them alive, measure resident bytes each and the rate they
  can be opened at. This is the "how many streams can a service maintain"
  question, from the transport's side; the `service` suite asks it again with
  sessions attached.

Transports measured: in-process (the floor -- no syscalls, no framing), and
WebSocket and HTTP SSE over loopback. Loopback is not a network, and these
numbers are the *protocol* cost with the network taken out; a real link adds
its own latency on top and does not remove any of this.
"""

from __future__ import annotations

import asyncio
import contextlib
import time
from collections.abc import AsyncIterator, Callable

from a11 import net
from a11.data import types
from a11.net.wire_stream import WireStreamWithRecv
from bench.harness import Result, benchmark, latency, memory_slope, percentiles

SUITE = "wire"

_TIMEOUT = 20.0


def _scaled(count: int, scale: float) -> int:
    return max(int(count * scale), 8)


def _message(payload: bytes) -> types.WireMessage:
    return types.WireMessage(
        node_fragments=[
            types.NodeFragment(id="bench", data=types.Chunk(data=payload))
        ]
    )


def _client_options() -> net.WebSocketClientOptions:
    """The gateway serves RFC 6455 over HTTP/1.1, so a client must say so."""
    options = net.WebSocketClientOptions()
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False
    return options


# --------------------------------------------------------------------------
# Transports
# --------------------------------------------------------------------------


class _Pair:
    """Two connected, started endpoints, and how to shut them down."""

    def __init__(self, client, server, close) -> None:
        self.client = client
        self.server = server
        self._close = close

    async def aclose(self, timeout: float = 5.0) -> None:
        """Close, but never wait forever for it.

        `drain_outgoing_messages` waits for the peer's transport buffer to
        take the half-close, and a peer that is not reading never makes room
        -- so closing a stream that was deliberately overrun blocks
        indefinitely. That is worth knowing (see `send_backpressure`) but it
        must not be the way a benchmark run ends, so the wait is bounded and a
        stuck close is reported rather than waited on.
        """
        try:
            await asyncio.wait_for(self._close(), timeout=timeout)
        except asyncio.TimeoutError:
            print(
                "    note: a stream close did not complete within"
                f" {timeout:.0f}s (peer not reading)",
                flush=True,
            )
        except Exception:  # noqa: BLE001 - teardown is best effort
            pass


@contextlib.asynccontextmanager
async def _in_process() -> AsyncIterator[Callable[[], object]]:
    async def connect() -> _Pair:
        raw_client, raw_server = net.create_in_process_wire_stream_pair()
        client = WireStreamWithRecv(raw_client)
        server = WireStreamWithRecv(raw_server)
        await client.start()
        await server.accept()

        async def close() -> None:
            with contextlib.suppress(Exception):
                client.half_close()
                await client.drain_outgoing_messages()

        return _Pair(client, server, close)

    yield connect


@contextlib.asynccontextmanager
async def _websocket() -> AsyncIterator[Callable[[], object]]:
    accepted: asyncio.Queue = asyncio.Queue()

    async def on_stream(stream) -> None:
        endpoint = WireStreamWithRecv(stream)
        await endpoint.accept()
        accepted.put_nowait(endpoint)

    options = net.WebSocketServerOptions()
    options.path = "/bench"
    options.bind_address = "127.0.0.1"
    options.port = 0
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False
    server = net.WebSocketWireServer.create(on_stream, options)

    async def connect() -> _Pair:
        raw = net.WebSocketWireStream.connect(
            f"ws://127.0.0.1:{server.port}/bench",
            websocket_options=_client_options(),
        )
        client = WireStreamWithRecv(raw)
        await asyncio.wait_for(client.start(), timeout=_TIMEOUT)
        peer = await asyncio.wait_for(accepted.get(), timeout=_TIMEOUT)

        async def close() -> None:
            with contextlib.suppress(Exception):
                client.half_close()
                await client.drain_outgoing_messages()

        return _Pair(client, peer, close)

    try:
        yield connect
    finally:
        server.stop()


@contextlib.asynccontextmanager
async def _http_sse() -> AsyncIterator[Callable[[], object]]:
    accepted: asyncio.Queue = asyncio.Queue()

    async def on_stream(stream) -> None:
        endpoint = WireStreamWithRecv(stream)
        await endpoint.accept()
        accepted.put_nowait(endpoint)

    server = net.HttpSseServer.create(
        "127.0.0.1", 0, on_stream, net.HttpSseOptions()
    )

    async def connect() -> _Pair:
        raw = net.HttpSseClientWireStream(f"http://127.0.0.1:{server.port}")
        client = WireStreamWithRecv(raw)
        await asyncio.wait_for(client.start(), timeout=_TIMEOUT)
        peer = await asyncio.wait_for(accepted.get(), timeout=_TIMEOUT)

        async def close() -> None:
            with contextlib.suppress(Exception):
                client.half_close()
                await client.drain_outgoing_messages()

        return _Pair(client, peer, close)

    try:
        yield connect
    finally:
        server.stop()


#: Name -> async context manager yielding a `connect()` coroutine factory.
TRANSPORTS = {
    "in-process": _in_process,
    "websocket": _websocket,
    "sse": _http_sse,
}


# --------------------------------------------------------------------------
# Benchmarks
# --------------------------------------------------------------------------


@benchmark(SUITE, "round_trip")
async def round_trip(scale: float) -> list[Result]:
    """Send a message, have the peer send one back, per transport and size."""
    results = []
    for name, transport in TRANSPORTS.items():
        async with transport() as connect:
            pair = await connect()
            try:
                for size in (64, 4096, 65536):
                    payload = _message(b"x" * size)
                    reply = _message(b"y" * 16)
                    iterations = _scaled(
                        800 if name == "in-process" else 300, scale
                    )

                    async def one(_index, p=pair, m=payload, r=reply):
                        p.client.send(m)
                        await asyncio.wait_for(
                            p.server.receive(), timeout=_TIMEOUT
                        )
                        p.server.send(r)
                        await asyncio.wait_for(
                            p.client.receive(), timeout=_TIMEOUT
                        )

                    metrics = await latency(
                        one, iterations=iterations, warmup=20
                    )
                    metrics["mib_per_s"] = (
                        metrics["ops_per_s"] * size / (1024 * 1024)
                    )
                    results.append(
                        Result(
                            SUITE,
                            "message_round_trip",
                            metrics,
                            {"transport": name, "size": _human(size)},
                        )
                    )
            finally:
                await pair.aclose()
    return results


@benchmark(SUITE, "one_way_throughput")
async def one_way_throughput(scale: float) -> list[Result]:
    """Streaming one direction, which is what a node mirror does.

    The sender does not wait for the receiver, so this measures the transport's
    sustained rate rather than its round-trip latency. The receiver counting
    every message is what makes it honest -- a benchmark that only measures
    `send` is measuring a queue.
    """
    results = []
    for name, transport in TRANSPORTS.items():
        async with transport() as connect:
            pair = await connect()
            try:
                for size in (64, 4096, 65536):
                    count = _scaled(
                        4000 if size <= 4096 else 400,
                        scale * (1.0 if name == "in-process" else 0.5),
                    )
                    payload = _message(b"x" * size)

                    # The sender is held to a bounded number of *unread*
                    # messages, not merely told to yield now and then.
                    #
                    # Yield-pacing was tried and is not enough: yielding lets
                    # the loop run but does not make the receiver keep up, and
                    # over WebSocket the run still died with RESOURCE_EXHAUSTED
                    # -- with a live reader attached, on loopback. The peer's
                    # budget is in *unread* messages, so the only thing that
                    # bounds it is counting what has actually been read. That
                    # `send` gives the caller nothing to count with, and that
                    # this benchmark has to reconstruct it from the receive
                    # side, is the finding in `send_backpressure`.
                    # A semaphore, not an Event, and that is not a style
                    # choice. The Event version had a lost wakeup: the sender
                    # checked its budget, the drain read a message and set the
                    # event, and only *then* did the sender clear it -- wiping
                    # the wakeup it was about to wait for. It survived until
                    # receives started resolving synchronously, which let the
                    # drain run ahead in bursts and made the interleaving
                    # common; the suite then hung. A credit per unread slot
                    # cannot lose one.
                    credit = asyncio.Semaphore(16)

                    async def drain(p=pair, c=count) -> int:
                        for _ in range(c):
                            await asyncio.wait_for(
                                p.server.receive(), timeout=_TIMEOUT
                            )
                            credit.release()
                        return c

                    started = time.perf_counter_ns()
                    receiver = asyncio.ensure_future(drain())
                    for _index in range(count):
                        await asyncio.wait_for(
                            credit.acquire(), timeout=_TIMEOUT
                        )
                        pair.client.send(payload)
                    seen = await receiver
                    elapsed = (time.perf_counter_ns() - started) / 1e9
                    results.append(
                        Result(
                            SUITE,
                            "one_way",
                            {
                                "ops_per_s": seen / elapsed,
                                "items_per_s": seen / elapsed,
                                "mib_per_s": seen * size / elapsed / 1048576,
                                "elapsed_s": elapsed,
                            },
                            {"transport": name, "size": _human(size)},
                        )
                    )
            finally:
                await pair.aclose()
    return results


@benchmark(SUITE, "send_backpressure")
async def send_backpressure(scale: float) -> list[Result]:
    """How far a sender may run ahead of its reader before the stream dies.

    `WireStream.send` is fire-and-forget: it does not block, does not return a
    future, and has no way to say "not yet". The peer's incoming buffer is
    bounded (`max_buffered_incoming_messages`, `max_buffered_incoming_bytes`),
    and when it fills, the stream is **aborted** with RESOURCE_EXHAUSTED
    rather than the sender being slowed down. That makes the number below a
    correctness boundary, not a tuning knob: a producer streaming faster than
    its consumer reads loses the connection.

    The transports do **not** agree about this, which is the finding. The same
    producer that kills a WebSocket connection is fine on SSE and fine
    in-process, so a composition that works on one transport can fail on
    another with no change to the code. `WireStream` is the interface this is
    meant to be pluggable behind, and overrun behaviour is part of a
    transport's contract whether or not it is written down.

    Reported per transport and payload size: how many unread messages went out
    before the stream died, and what killed it. Note that `send` returns
    successfully either way -- the abort surfaces asynchronously, after a
    settle, which is why the sender has no chance to react.
    """
    results = []
    #: Two ceilings, because a probe looking for a cliff must stop whether or
    #: not it finds one. Without the byte ceiling a transport with generous
    #: limits has the benchmark allocate until the machine swaps, and then it
    #: measures the machine; without the time budget the same run simply never
    #: returns. Reaching either is a result -- "no cliff within this much" --
    #: not a failure.
    ceiling_bytes = 64 * 1024 * 1024
    budget_s = 20.0

    for name, transport in TRANSPORTS.items():
        for size in (64, 65536):
            payload = _message(b"x" * size)
            async with transport() as connect:
                pair = await connect()
                sent = 0
                outcome = "survived the probe's ceiling"
                limit = min(
                    int(_scaled(20_000, scale)), ceiling_bytes // size
                )
                deadline = time.perf_counter() + budget_s
                try:
                    while sent < limit:
                        pair.client.send(payload)
                        sent += 1
                        # Yield periodically. `send` is synchronous and the
                        # transport's pump is not: a tight loop with no awaits
                        # never lets the peer's buffer accounting run, so the
                        # sender would never learn it had overrun. The yield is
                        # also what a real producer inside a coroutine does.
                        if sent % 64 == 0:
                            await asyncio.sleep(0)
                            if not pair.client.get_status().is_ok():
                                break
                            if time.perf_counter() > deadline:
                                outcome = (
                                    f"survived {budget_s:.0f}s of sending"
                                )
                                break
                except Exception as failure:  # noqa: BLE001 - the abort is the result
                    outcome = f"send raised {type(failure).__name__}"

                # The abort is asynchronous: `send` has already returned by the
                # time the peer decides. Settle before reading the verdict,
                # because that delay is exactly why a producer cannot react.
                await asyncio.sleep(0.5)
                status = pair.client.get_status()
                if not status.is_ok():
                    outcome = f"aborted {status.code.name}"
                results.append(
                    Result(
                        SUITE,
                        "unread_messages_before_abort",
                        {
                            "ops_per_s": float(sent),
                            "bytes_each": float(sent * size),
                        },
                        {"transport": name, "size": _human(size)},
                        note=(
                            f"{sent} unread messages"
                            f" ({sent * size / 1048576:.1f} MiB) -> {outcome}"
                        ),
                    )
                )
                await pair.aclose()
    return results


@benchmark(SUITE, "stream_capacity")
async def stream_capacity(scale: float) -> list[Result]:
    """How many concurrent streams a process holds, and what each one costs.

    Connections are opened and *kept* -- the resident-memory fit is over live
    streams, not over streams that have been and gone. The open rate is
    reported beside it because a service that can hold ten thousand streams but
    only accept forty a second has a different problem.
    """
    results = []
    for name, transport in TRANSPORTS.items():
        stage = _scaled(200 if name == "in-process" else 100, scale)
        async with transport() as connect:
            held: list[object] = []

            async def open_many(count: int, c=connect, h=held) -> int:
                for _ in range(count):
                    h.append(await c())
                return count

            started = time.perf_counter_ns()
            slope, trail = await memory_slope(
                open_many, counts=[stage] * 6
            )
            elapsed = (time.perf_counter_ns() - started) / 1e9
            total = stage * 6
            results.append(
                Result(
                    SUITE,
                    "live_stream",
                    {
                        "bytes_each": slope,
                        "ops_per_s": total / elapsed,
                        "elapsed_s": elapsed,
                    },
                    {"transport": name},
                    note=(
                        f"{total} live connections, both endpoints in this"
                        f" process -- halve for a server's own share."
                        f" ops/s is the open rate. {trail}"
                    ),
                )
            )
            for pair in held:
                await pair.aclose()
            held.clear()
    return results


@benchmark(SUITE, "concurrent_stream_traffic", slow=True)
async def concurrent_stream_traffic(scale: float) -> list[Result]:
    """Aggregate round trips per second with N streams working at once.

    One stream's round-trip latency says nothing about a server's capacity: the
    interesting question is whether a hundred streams each doing a round trip
    get a hundredth of the throughput each, or whether the transport has
    headroom. Reported as aggregate rate and as per-stream p50.
    """
    results = []
    for name, transport in TRANSPORTS.items():
        async with transport() as connect:
            for streams in (1, 8, 64):
                pairs = [await connect() for _ in range(streams)]
                per_stream = _scaled(
                    200 if name == "in-process" else 60, scale
                )
                payload = _message(b"x" * 256)
                reply = _message(b"y" * 256)

                async def exercise(pair, count=per_stream) -> list[float]:
                    samples = []
                    for _ in range(count):
                        started = time.perf_counter_ns()
                        pair.client.send(payload)
                        await asyncio.wait_for(
                            pair.server.receive(), timeout=_TIMEOUT
                        )
                        pair.server.send(reply)
                        await asyncio.wait_for(
                            pair.client.receive(), timeout=_TIMEOUT
                        )
                        samples.append(time.perf_counter_ns() - started)
                    return samples

                started = time.perf_counter_ns()
                gathered = await asyncio.gather(
                    *(exercise(pair) for pair in pairs)
                )
                elapsed = (time.perf_counter_ns() - started) / 1e9
                samples = [s for run in gathered for s in run]
                metrics = percentiles(samples)
                metrics["ops_per_s"] = len(samples) / elapsed
                metrics["elapsed_s"] = elapsed
                results.append(
                    Result(
                        SUITE,
                        "concurrent_round_trips",
                        metrics,
                        {"transport": name, "streams": streams},
                        note=(
                            "ops/s is aggregate; percentiles are per round"
                            " trip"
                        ),
                    )
                )
                for pair in pairs:
                    await pair.aclose()
    return results


def _human(size: int) -> str:
    if size >= 1024 * 1024:
        return f"{size // 1024 // 1024}M"
    if size >= 1024:
        return f"{size // 1024}K"
    return f"{size}B"
