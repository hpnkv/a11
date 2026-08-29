# Copyright 2026 The A11 Authors.

"""Measure A11 transports across a network link.

`bench/suites/wire.py` isolates protocol overhead on one machine. This suite
adds network propagation, bandwidth limits, and separate client and server
resource measurements:

* Round-trip results show how propagation changes transport latency ratios.
* `wire_inflation` measures payload expansion against finite link bandwidth.
* Server CPU metrics identify which endpoint limits throughput.

Requires a `bench.peer` agent on the far host and `A11_BENCH_PEER=host:port`;
skips with a reason otherwise. Run it against `127.0.0.1` first to record the
two-process loopback baseline, then compare the LAN result with that baseline.

The `raw-tcp` rows are the same reference `wire` uses, re-measured on this link:
a bare length-prefixed socket ping-pong without A11. Its results provide the
link-level latency and throughput baseline.
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import socket
import time
from collections.abc import AsyncIterator, Callable

import a11
from a11 import net
from a11.data import types
from a11.net.wire_stream import WireStreamWithRecv
from bench.harness import Result, Skip, benchmark, latency, percentiles
from bench.peer import PeerClient, ServerCost

SUITE = "link"

_TIMEOUT = 30.0

#: Sizes to sweep. The 1 MiB case exercises WebSocket's 64 KiB splitting and
#: multi-packet reassembly on a network link.
_LATENCY_SIZES = (64, 4096, 65536)
_THROUGHPUT_SIZES = (64, 4096, 65536, 1 << 20)

#: Messages in flight in the throughput rows. Not a tuning choice: `Send` has
#: no admission signal, so an unpaced flood aborts the connection instead of
#: pushing back. The peer's one-fragment echo per arriving fragment is the
#: credit that releases the next message.
#:
#: On a link with a real RTT, the window limits throughput according to the
#: bandwidth-delay product. A window of W messages
#: of S bytes on a link of RTT R cannot exceed `W * S / R` however fast either
#: end is, so on a 3.6ms wireless RTT a 32-message window of 64-byte messages
#: caps at 8.9k msg/s. `A11_BENCH_LINK_WINDOW`
#: sweeps it, and `window_sweep` below reports the curve: where a row stops
#: improving with the window, the transport is the limit. Continued improvement
#: indicates that the benchmark window was limiting throughput.
_WINDOW = int(os.environ.get("A11_BENCH_LINK_WINDOW", "32"))


def _scaled(count: int, scale: float) -> int:
    return max(int(count * scale), 8)


def _human(size: int) -> str:
    if size >= 1 << 20:
        return f"{size >> 20}M"
    if size >= 1024:
        return f"{size >> 10}K"
    return f"{size}B"


def _message(payload: bytes) -> types.WireMessage:
    return types.WireMessage(
        node_fragments=[
            types.NodeFragment(id="bench", data=types.Chunk(data=payload))
        ]
    )


async def _peer() -> PeerClient:
    client = PeerClient.from_environment()
    if client is None:
        raise Skip(
            "no A11_BENCH_PEER=host:port -- start `python -m bench.peer` on"
            " the other host (or on this one, for the loopback control)"
        )
    try:
        await client.connect()
    except (OSError, asyncio.TimeoutError) as unreachable:
        raise Skip(
            f"peer at {client.host}:{client.port} is unreachable"
            f" ({unreachable!r})"
        ) from unreachable
    return client


def _link_params(peer: PeerClient) -> dict[str, str]:
    """Return the result label for the measured link.

    Local peers use `link=loopback`; remote peers use `link=lan`. Distinct keys
    prevent an implicit `--baseline` comparison between the two environments.
    """
    loopback = peer.host in ("127.0.0.1", "localhost", "::1")
    return {"link": "loopback" if loopback else "lan"}


# --------------------------------------------------------------------------
# The link floor: a bare socket, no A11
# --------------------------------------------------------------------------


class _RawClient:
    """Length-prefixed ping-pong against the peer's `tcp_echo`."""

    def __init__(self, reader, writer) -> None:
        self.reader = reader
        self.writer = writer

    @classmethod
    async def connect(cls, host: str, port: int) -> _RawClient:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(host, port), timeout=_TIMEOUT
        )
        writer.transport.get_extra_info("socket").setsockopt(
            socket.IPPROTO_TCP, socket.TCP_NODELAY, 1
        )
        return cls(reader, writer)

    async def round_trip(self, payload: bytes) -> None:
        self.writer.write(len(payload).to_bytes(4, "big") + payload)
        await self.writer.drain()
        header = await self.reader.readexactly(4)
        await self.reader.readexactly(int.from_bytes(header, "big"))

    async def aclose(self) -> None:
        with contextlib.suppress(Exception):
            self.writer.close()


@benchmark(SUITE, "raw_socket_floor")
async def raw_socket_floor(scale: float) -> list[Result]:
    """What the link costs with no A11 in the path at all.

    Two rows, and the second is the one a throughput target is about:

    * `tcp_round_trip` -- one request, one reply, nothing overlapped. This is
      the floor under every remote dispatch on this link, and on a LAN it is
      mostly propagation and interrupt handling rather than anything either
      host computes.
    * `tcp_one_way` -- bytes at the socket, as fast as the kernel will take
      them, into a peer that only reads. The practical ceiling for any A11
      transport here, and the number every `mib_per_s` below should be read as
      a fraction of.

    **This is an asyncio floor, not a kernel floor, and at 64 KiB A11 beats
    it.** Both ends are `asyncio` stream sockets, which is deliberate -- it is
    the same language and the same loop as the transports it is compared with,
    so the difference is the protocol rather than the runtime. But the client
    reads a length prefix and then a body, which is two reads and at least two
    loop turns per reply, while A11's transports do their I/O on the native uv
    loop; measured on a wireless LAN, `message_round_trip[websocket,64K]` came
    in at 4.43ms against this row's 5.65ms. So take `tcp_round_trip` as a
    same-language *reference*, not as a bound: a transport faster than it has
    beaten asyncio, not the kernel. The true kernel floor is `a11_bench`'s
    `raw-tcp` row, which is C++ on both ends.

    `tcp_one_way` does not have this problem -- the sink only reads -- and is a
    sound ceiling.
    """
    peer = await _peer()
    params = _link_params(peer)
    results: list[Result] = []
    try:
        echo_port = (await peer.call("tcp_echo", port=0))["port"]
        client = await _RawClient.connect(peer.host, echo_port)
        try:
            for size in _LATENCY_SIZES:
                payload = b"x" * size
                metrics = await latency(
                    lambda _index, p=payload: client.round_trip(p),
                    iterations=_scaled(400, scale),
                    warmup=50,
                )
                metrics["mib_per_s"] = (
                    metrics["ops_per_s"] * size / (1024 * 1024)
                )
                results.append(
                    Result(
                        SUITE,
                        "tcp_round_trip",
                        metrics,
                        {**params, "size": _human(size)},
                    )
                )
        finally:
            await client.aclose()

        sink_port = (await peer.call("tcp_sink", port=0))["port"]
        for size in (65536,):
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(peer.host, sink_port),
                timeout=_TIMEOUT,
            )
            block = b"x" * size
            count = _scaled(4000, scale)
            before = await peer.stats()
            started = time.perf_counter_ns()
            try:
                for _index in range(count):
                    writer.write(block)
                    await writer.drain()
                writer.write_eof()
                await writer.drain()
            finally:
                elapsed = (time.perf_counter_ns() - started) / 1e9
                with contextlib.suppress(Exception):
                    writer.close()
            # Let the peer finish reading what is already in flight before
            # asking what it received: a sender's `drain` returns when the
            # kernel took the bytes, not when anyone read them.
            await asyncio.sleep(0.5)
            after = await peer.stats()
            delivered = int(after["sink_bytes"]) - int(before["sink_bytes"])
            cost = ServerCost(before, after)
            metrics = {
                "mib_per_s": count * size / elapsed / (1024 * 1024),
                "gbit_per_s": count * size * 8 / elapsed / 1e9,
                "ops_per_s": count / elapsed,
                "elapsed_s": elapsed,
                "delivered_bytes": float(delivered),
                **cost.metrics(count),
            }
            results.append(
                Result(
                    SUITE,
                    "tcp_one_way",
                    metrics,
                    {**params, "size": _human(size)},
                    note=(
                        "the ceiling every A11 transport row on this link is a"
                        f" fraction of; peer read {delivered / (1 << 20):.0f}"
                        f" MiB of the {count * size / (1 << 20):.0f} MiB sent"
                    ),
                )
            )
    finally:
        with contextlib.suppress(Exception):
            await peer.teardown()
        await peer.aclose()
    return results


# --------------------------------------------------------------------------
# A11 transports over the link
# --------------------------------------------------------------------------


@contextlib.asynccontextmanager
async def _remote_endpoint(
    peer: PeerClient, transport: str
) -> AsyncIterator[Callable[[], object]]:
    """A `connect()` that dials the peer's `wire_echo` for this transport."""
    port = (await peer.call("wire_echo", transport=transport, port=0))["port"]

    async def connect():
        if transport == "websocket":
            options = net.WebSocketClientOptions()
            # Mirror the server: it serves RFC 6455 over HTTP/1.1, and a client
            # that offers h2 negotiates something the server will not speak.
            options.http2_options.enable_h2 = False
            options.http2_options.enable_h2c = False
            raw = net.WebSocketWireStream.connect(
                f"ws://{peer.host}:{port}/bench", websocket_options=options
            )
        elif transport == "sse":
            raw = net.HttpSseClientWireStream(f"http://{peer.host}:{port}")
        else:
            raise ValueError(transport)
        endpoint = WireStreamWithRecv(raw)
        await asyncio.wait_for(endpoint.start(), timeout=_TIMEOUT)
        return endpoint

    try:
        yield connect
    finally:
        with contextlib.suppress(Exception):
            await peer.teardown()


async def _close(endpoint) -> None:
    """Drain, then abort -- `half_close` does not reclaim the socket.

    A half-closed connection is still a connection, and the descriptor stays:
    measured at 1.04 fds retained per connection with `half_close` plus a drain,
    flat with `abort`. `connect_cost` opens tens of streams per run and would
    otherwise walk into the default 1024-descriptor limit. See `FINDINGS.md`,
    "A closed client stream keeps its file descriptor".
    """
    with contextlib.suppress(Exception):
        endpoint.half_close()
        await asyncio.wait_for(endpoint.drain_outgoing_messages(), timeout=10.0)
    with contextlib.suppress(Exception):
        endpoint.abort(
            a11.Status(code=a11.StatusCode.CANCELLED, message="row finished")
        )


#: Transports reachable across machines. `raw-tcp` provides the baseline;
#: `in-process` is excluded because it does not use a network socket.
_TRANSPORTS = ("websocket", "sse")


@benchmark(SUITE, "message_round_trip")
async def message_round_trip(scale: float) -> list[Result]:
    """One message out, one fragment back, per transport and size.

    Read this beside `raw_socket_floor`'s `tcp_round_trip` at the same size.
    The difference is everything A11 adds -- framing, the envelope codec, two
    fiber hops per direction on each end -- and on a link with a real RTT that
    difference is a *smaller share* of the total than loopback suggests, which
    is a finding about priorities rather than about the transport.
    """
    peer = await _peer()
    params = _link_params(peer)
    results: list[Result] = []
    try:
        for transport in _TRANSPORTS:
            async with _remote_endpoint(peer, transport) as connect:
                endpoint = await connect()
                try:
                    for size in _LATENCY_SIZES:
                        payload = _message(b"x" * size)

                        async def one(_index, e=endpoint, m=payload):
                            e.send(m)
                            await asyncio.wait_for(
                                e.receive(), timeout=_TIMEOUT
                            )

                        before = await peer.stats()
                        iterations = _scaled(300, scale)
                        metrics = await latency(
                            one, iterations=iterations, warmup=40
                        )
                        after = await peer.stats()
                        metrics["mib_per_s"] = (
                            metrics["ops_per_s"] * size / (1024 * 1024)
                        )
                        metrics.update(
                            ServerCost(before, after).metrics(iterations)
                        )
                        results.append(
                            Result(
                                SUITE,
                                "message_round_trip",
                                metrics,
                                {
                                    **params,
                                    "transport": transport,
                                    "size": _human(size),
                                },
                            )
                        )
                finally:
                    await _close(endpoint)
    finally:
        with contextlib.suppress(Exception):
            await peer.teardown()
        await peer.aclose()
    return results


async def _windowed(
    peer: PeerClient,
    endpoint,
    transport: str,
    size: int,
    count: int,
    window: int,
) -> dict[str, float] | None:
    """Push `count` messages of `size`, `window` outstanding; None if stalled.

    Credit is counted in *fragments*, not messages. Both `Sender` loops fold
    whatever is already queued into the delivery they are about to make, so N
    messages sent can arrive as fewer, larger ones; a window counted in messages
    then waits for echoes that will never come separately and reports a stall
    that is not one.
    """
    payload = _message(b"x" * size)
    credit = asyncio.Semaphore(window)
    received = 0

    async def drain() -> None:
        nonlocal received
        while received < count:
            message = await asyncio.wait_for(
                endpoint.receive(), timeout=_TIMEOUT
            )
            if message is None:
                return
            for _ in range(len(message.node_fragments or ()) or 1):
                if received >= count:
                    break
                received += 1
                credit.release()

    reader = asyncio.ensure_future(drain())
    before = await peer.stats()
    started = time.perf_counter_ns()
    try:
        for _index in range(count):
            await asyncio.wait_for(credit.acquire(), timeout=_TIMEOUT)
            endpoint.send(payload)
        await asyncio.wait_for(reader, timeout=_TIMEOUT)
    except Exception as stalled:  # noqa: BLE001 - a stall is a result
        reader.cancel()
        # `CancelledError` must be named here, and leaving it out cost a whole
        # suite. It derives from `BaseException` in 3.8+, so awaiting a task we
        # just cancelled re-raises straight past `suppress(Exception)` -- and
        # past the harness's own `except Exception`. The symptom was
        # `LOST link: no results written` for the entire suite the first time a
        # `window_sweep` cell found the peer's admission edge, which is a cell
        # that is *expected* to fail and should have cost one row.
        with contextlib.suppress(asyncio.CancelledError, Exception):
            await reader
        # A stall is a claim, not a fact. Ask the stream what it thinks
        # happened and abandon the row, rather than reporting a rate for a
        # transfer that did not finish.
        status = "unknown"
        with contextlib.suppress(Exception):
            status = str(endpoint.get_status())
        print(
            f"    skip {transport} {_human(size)} window={window}:"
            f" {stalled!r}; delivered {received}/{count};"
            f" stream status {status}",
            flush=True,
        )
        return None
    elapsed = (time.perf_counter_ns() - started) / 1e9
    after = await peer.stats()
    metrics = {
        "ops_per_s": count / elapsed,
        "mib_per_s": count * size / elapsed / (1024 * 1024),
        "gbit_per_s": count * size * 8 / elapsed / 1e9,
        "elapsed_s": elapsed,
        "window": float(window),
        **ServerCost(before, after).metrics(count),
    }
    return metrics


@benchmark(SUITE, "window_sweep")
async def window_sweep(scale: float) -> list[Result]:
    """Measure how the credit window limits transport throughput.

    A window of W messages of S bytes on a link whose round trip is R cannot
    carry more than
    `W * S / R` regardless of what either end can do -- so on a wireless LAN
    with a 3.6ms RTT, the default 32-message window caps 64-byte traffic at
    about 8.9k msg/s before A11 is involved at all.

    Compare the curve across window sizes. Where doubling the window stops
    increasing throughput, the transport or link is the ceiling. If throughput
    continues to increase, `stream_throughput` measured the benchmark's pacing
    and does not represent A11's limit.

    The window cannot simply be made large: `Send` has no admission signal, so
    past the peer's reassembly budget the connection is aborted rather than
    pushed back (`FINDINGS.md` item 7). A row that vanishes from this table at a
    large window found that edge, and the skip line says so.
    """
    peer = await _peer()
    params = _link_params(peer)
    results: list[Result] = []
    try:
        for transport in _TRANSPORTS:
            async with _remote_endpoint(peer, transport) as connect:
                for size in (64, 65536):
                    # Up to 2048 because 512 was not enough: on a 1 Gbit
                    # wireless link the 64-byte row was still climbing at
                    # 512 (32.2k msg/s), so a sweep that stopped there
                    # reported a floor and called it a ceiling.
                    for window in (8, 32, 128, 512, 2048):
                        # A fresh connection per cell, and this is not tidiness.
                        #
                        # The large-window cells are *expected* to find the
                        # peer's reassembly limit, and past it the connection is
                        # aborted rather than pushed back (`FINDINGS.md` item
                        # 7).
                        # Sharing one endpoint across the sweep therefore lost
                        # every row after the first cell that hit the edge: the
                        # 64-byte window-2048 cell killed the stream and all
                        # four
                        # 64 KiB cells then reported
                        # `FAILED_PRECONDITION: This endpoint has already
                        # terminated` with 0 of 200 delivered. A row that finds
                        # the edge must cost only itself.
                        endpoint = await connect()
                        try:
                            budget = 4000 if size <= 64 else 800
                            count = _scaled(int(budget * 0.5), scale)
                            # Never fewer messages than the window, or the row
                            # measures the ramp rather than the steady state.
                            count = max(count, window * 4)
                            metrics = await _windowed(
                                peer, endpoint, transport, size, count, window
                            )
                            if metrics is None:
                                continue
                            results.append(
                                Result(
                                    SUITE,
                                    "stream_throughput_by_window",
                                    metrics,
                                    {
                                        **params,
                                        "transport": transport,
                                        "size": _human(size),
                                        "in_flight": window,
                                    },
                                )
                            )
                        finally:
                            await _close(endpoint)
        for transport in _TRANSPORTS:
            for size in ("64B", "64K"):
                curve = [
                    r
                    for r in results
                    if r.params.get("transport") == transport
                    and r.params.get("size") == size
                ]
                if len(curve) < 2:
                    continue
                best = max(curve, key=lambda r: r.metrics["mib_per_s"])
                first = curve[0]
                peak = best.metrics["mib_per_s"]
                smallest = first.metrics["mib_per_s"] or peak
                best.note = (
                    f"best of the sweep: {peak:.1f} MiB/s at window"
                    f" {best.params['in_flight']}, {peak / smallest:.2f}x the"
                    f" window-{first.params['in_flight']} row. If this is the"
                    " largest window measured, the ceiling has not been found"
                    " and the number is a floor"
                )
    finally:
        with contextlib.suppress(Exception):
            await peer.teardown()
        await peer.aclose()
    return results


@benchmark(SUITE, "stream_throughput")
async def stream_throughput(scale: float) -> list[Result]:
    """Sustained one-way rate with a window open: the bandwidth row.

    Paced by the peer's echo -- one fragment back per arriving fragment -- and
    that pacing is a safety requirement, not a knob. `Send` has no admission
    signal, so a sender that outruns its reader aborts the connection rather
    than being pushed back; the credit window is what keeps a throughput
    benchmark from measuring how fast A11 can drop a connection.

    Report `gbit_per_s` against the link's rated speed and against
    `tcp_one_way`. A transport at half of `tcp_one_way` on a 2.5 Gbit link and
    a transport at half of it on loopback are not the same finding: on the link
    the remaining half may not exist to be won.

    **On a LAN, read `window_sweep` before quoting any small-message row here.**
    The default window of 32 is what keeps these rows comparable with the
    loopback `wire` suite, and on a link with a 3.6ms round trip it is itself
    the ceiling for small messages: the 64-byte WebSocket row measured 5.9k
    msg/s at window 32 and 9.7k at window 128, so the default understated it by
    1.7x. The 64 KiB rows are window-insensitive over 8-512 and need no such
    caveat.
    """
    peer = await _peer()
    params = _link_params(peer)
    results: list[Result] = []
    try:
        for transport in _TRANSPORTS:
            async with _remote_endpoint(peer, transport) as connect:
                endpoint = await connect()
                try:
                    for size in _THROUGHPUT_SIZES:
                        budget = {64: 6000, 4096: 6000, 65536: 1200}.get(
                            size, 200
                        )
                        count = _scaled(int(budget * 0.5), scale)
                        metrics = await _windowed(
                            peer, endpoint, transport, size, count, _WINDOW
                        )
                        if metrics is None:
                            continue
                        results.append(
                            Result(
                                SUITE,
                                "stream_throughput",
                                metrics,
                                {
                                    **params,
                                    "transport": transport,
                                    "size": _human(size),
                                },
                            )
                        )
                finally:
                    await _close(endpoint)
    finally:
        with contextlib.suppress(Exception):
            await peer.teardown()
        await peer.aclose()
    return results


@benchmark(SUITE, "wire_inflation")
async def wire_inflation(scale: float) -> list[Result]:
    """Bytes actually on the link per byte of payload, per transport.

    The one measurement loopback cannot make, and the one that decides whether
    SSE's JSON-plus-base64 body is a real cost or a stylistic complaint. On a
    2.5 Gbit link a 1.33x inflation is a third of the bandwidth, and no amount
    of work above the codec recovers it.

    Method and its error bar, stated because whole-interface counters are a
    blunt instrument: the peer reads `/proc/net/dev` before and after a known
    transfer, and an idle window of the same length is measured first and
    reported as `idle_drift_bytes`. Anything within a few times the drift is
    not a result. Linux peer only -- the counters come from `/proc`.
    """
    peer = await _peer()
    params = _link_params(peer)
    if params["link"] == "loopback":
        raise Skip(
            "wire inflation over loopback measures nothing: `lo` is excluded"
            " from the interface counters, and including it would count the"
            " bytes twice"
        )
    probe = await peer.stats()
    if not probe.get("interfaces"):
        raise Skip("peer has no /proc/net/dev interface counters (not Linux)")
    results: list[Result] = []
    try:
        # An idle window first, the same length the transfers will take, so the
        # host's own background traffic has a number attached to it.
        idle_before = await peer.stats()
        await asyncio.sleep(2.0)
        idle_after = await peer.stats()
        drift = ServerCost(idle_before, idle_after).interface_bytes()
        idle_bytes = drift["rx_bytes"] + drift["tx_bytes"]

        for transport in _TRANSPORTS:
            async with _remote_endpoint(peer, transport) as connect:
                endpoint = await connect()
                try:
                    for size in (4096, 65536):
                        count = _scaled(400 if size > 4096 else 2000, scale)
                        payload = _message(b"x" * size)
                        credit = asyncio.Semaphore(_WINDOW)
                        received = 0

                        async def drain(e=endpoint, c=count):
                            nonlocal received
                            while received < c:
                                message = await asyncio.wait_for(
                                    e.receive(), timeout=_TIMEOUT
                                )
                                if message is None:
                                    return
                                for _ in range(
                                    len(message.node_fragments or ()) or 1
                                ):
                                    if received >= c:
                                        break
                                    received += 1
                                    credit.release()

                        reader = asyncio.ensure_future(drain())
                        before = await peer.stats()
                        try:
                            for _index in range(count):
                                await asyncio.wait_for(
                                    credit.acquire(), timeout=_TIMEOUT
                                )
                                endpoint.send(payload)
                            await asyncio.wait_for(reader, timeout=_TIMEOUT)
                        except Exception as stalled:  # noqa: BLE001
                            reader.cancel()
                            with contextlib.suppress(
                                asyncio.CancelledError, Exception
                            ):
                                await reader
                            print(
                                f"    skip {transport} {_human(size)}"
                                f" inflation: {stalled!r}",
                                flush=True,
                            )
                            continue
                        after = await peer.stats()
                        moved = ServerCost(before, after).interface_bytes()
                        payload_bytes = count * size
                        # The peer *receives* the payload and sends back one
                        # byte per fragment, so rx is the direction the
                        # inflation of a large payload lands in.
                        on_wire = moved["rx_bytes"]
                        metrics = {
                            "payload_bytes": float(payload_bytes),
                            "wire_bytes": float(on_wire),
                            "inflation": (
                                on_wire / payload_bytes
                                if payload_bytes
                                else 0.0
                            ),
                            "bytes_per_packet": (
                                on_wire / moved["rx_packets"]
                                if moved["rx_packets"]
                                else 0.0
                            ),
                            "idle_drift_bytes": float(idle_bytes),
                        }
                        results.append(
                            Result(
                                SUITE,
                                "wire_inflation",
                                metrics,
                                {
                                    **params,
                                    "transport": transport,
                                    "size": _human(size),
                                },
                                note=(
                                    "interface counters; idle drift over a"
                                    f" comparable window was {idle_bytes} bytes"
                                    f" against {payload_bytes} of payload"
                                ),
                            )
                        )
                finally:
                    await _close(endpoint)
    finally:
        with contextlib.suppress(Exception):
            await peer.teardown()
        await peer.aclose()
    return results


@benchmark(SUITE, "connect_cost")
async def connect_cost(scale: float) -> list[Result]:
    """What opening a stream to another machine costs, per transport.

    Separate from everything else because it is the number a client fleet's
    reconnect storm is sized by, and because it is dominated by handshakes
    (TCP, then HTTP, then the protocol's own) whose cost is round trips rather
    than work. On a LAN it should be a small multiple of `tcp_round_trip`; a
    large multiple means handshake round trips that could be folded.
    """
    peer = await _peer()
    params = _link_params(peer)
    results: list[Result] = []
    try:
        for transport in _TRANSPORTS:
            async with _remote_endpoint(peer, transport) as connect:
                opened: list[object] = []
                samples: list[float] = []
                iterations = _scaled(60, scale)
                try:
                    # One outside the clock: the first connection on a fresh
                    # server pays for whatever it sets up lazily.
                    opened.append(await connect())
                    before = await peer.stats()
                    for _index in range(iterations):
                        started = time.perf_counter_ns()
                        endpoint = await connect()
                        samples.append(time.perf_counter_ns() - started)
                        opened.append(endpoint)
                    after = await peer.stats()
                    metrics = percentiles(samples)
                    total = sum(samples) / 1e9
                    metrics["ops_per_s"] = iterations / total if total else 0.0
                    metrics.update(
                        ServerCost(before, after).metrics(iterations)
                    )
                    results.append(
                        Result(
                            SUITE,
                            "stream_connect",
                            metrics,
                            {**params, "transport": transport},
                            note=(
                                f"{len(opened)} streams held open at the end;"
                                " sequential, so ops/s is 1/latency and not an"
                                " accept-rate ceiling -- see"
                                " server/join_rate for that"
                            ),
                        )
                    )
                finally:
                    for endpoint in opened:
                        await _close(endpoint)
    finally:
        with contextlib.suppress(Exception):
            await peer.teardown()
        await peer.aclose()
    return results


@benchmark(SUITE, "peer_environment")
async def peer_environment(scale: float) -> list[Result]:
    """Not a measurement: what the other host is, recorded in the run.

    Every other row here is a two-machine number, and a two-machine number is
    uninterpretable without knowing the second machine. This puts the peer's
    platform, core count and extension build into the JSON so a run can be read
    a month later.
    """
    peer = await _peer()
    try:
        environment = peer.environment
        stats = await peer.stats()
        built = (environment.get("native") or {}).get("built")
        return [
            Result(
                SUITE,
                "peer",
                {
                    "cpu_count": float(environment.get("cpu_count") or 0),
                    "rss_bytes": float(stats.get("rss_bytes") or 0),
                },
                _link_params(peer),
                note=(
                    f"{environment.get('hostname')} --"
                    f" {environment.get('platform')} /"
                    f" {environment.get('machine')},"
                    f" python {environment.get('python')},"
                    f" a11 {environment.get('a11')},"
                    f" native built {built}"
                ),
            )
        ]
    finally:
        await peer.aclose()
