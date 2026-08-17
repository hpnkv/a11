# Copyright 2026 The A11 Authors.

"""Does A11 hold up as a server? Not "how fast is one call".

Every other suite measures one thing with everything else held still, which is
how a cost gets attributed and is not how a server is used. A deployment runs a
mixture, continuously, while its client population changes underneath it, and
the failure modes that matter are the ones a clean benchmark cannot produce:

* a tail that only appears when clients are joining and leaving;
* a throughput that is fine for ten seconds and drifts over ten minutes;
* one population of clients starving another;
* a rate that looks good until you notice it used eleven of twelve cores.

So the unit of measurement here is a **window**: a fixed span of wall clock
during which several populations of clients do different things at once, with
the server's own CPU and memory sampled from inside the server process. Each
row reports what the server delivered, what the *established* clients felt
while it did, and what it cost the host.

Four rules this suite is built on, each of which came from a number that was
wrong before it:

**A mean is not an answer; a distribution under interference is.** Every row
that reports a rate also reports the p99 and p99.9 of the clients that were
already connected, because "aggregate throughput held up" and "half the clients
saw a 200ms stall" are simultaneously true more often than anyone expects.

**Degradation is measured as a ratio inside one run, never across runs.** The
churn and mixing rows always measure the quiet case and the loaded case back to
back on the same connections and the same server process. A ratio between two
separate runs of a two-machine benchmark is not a measurement.

**A rate without a CPU cost is not comparable across machines.** Every row
carries `server_cpu_us_per_op` and `server_cores_busy`, from the server's own
`getrusage`. A host with more cores delivering more actions per second has said
nothing about A11 until the per-operation CPU is beside it.

**Consistency is a metric, not a hope.** The soak row reports per-second
buckets and their coefficient of variation and first-third-to-last-third drift.
A server whose mean is high and whose CoV is 0.4 is not a server anybody can
put a latency budget on.

Requires a `bench.peer` agent and `A11_BENCH_PEER=host:port`; skips otherwise.
Run it against `127.0.0.1` first for the control -- the two-process topology
with no network -- and then against the real host.
"""

from __future__ import annotations

import asyncio
import contextlib
import math
import statistics
import time
from typing import Any

import a11
from a11 import net, timing
from a11.data import types
from bench.harness import Result, Skip, benchmark, percentiles
from bench.peer import ECHO, SINK, SOURCE, STORE, PeerClient, ServerCost

SUITE = "server"

_WAIT = timing.Duration.seconds(30)
_CONNECT_TIMEOUT = 20.0

#: Per-call ceiling. A server benchmark must never hang: a call that does not
#: come back inside this is counted as a failure and reported as one, which is
#: a finding, where a hang is a lost run.
_CALL_TIMEOUT = 30.0

_OCTET = "application/octet-stream"


def _scaled(count: int, scale: float) -> int:
    return max(int(count * scale), 2)


def _window_seconds(base: float, scale: float) -> float:
    """A measurement window, floored so a scaled-down run still means something.

    Below about two seconds the per-second buckets in the soak row stop being a
    distribution and the churn row stops overlapping enough joins with enough
    calls to interfere at all.
    """
    return max(base * scale, 2.0)


def _tail(samples_ns: list[float]) -> dict[str, float]:
    """`percentiles` plus p99.9, which is the one a server is judged on."""
    metrics = percentiles(samples_ns)
    if samples_ns:
        ordered = sorted(samples_ns)
        index = min(
            len(ordered) - 1,
            max(0, math.ceil(0.999 * len(ordered)) - 1),
        )
        metrics["p999_us"] = ordered[index] / 1000.0
    return metrics


async def _peer() -> PeerClient:
    client = PeerClient.from_environment()
    if client is None:
        raise Skip(
            "no A11_BENCH_PEER=host:port -- start `python -m bench.peer` on"
            " the host that is to be the server"
        )
    try:
        await client.connect()
    except (OSError, asyncio.TimeoutError) as unreachable:
        raise Skip(
            f"peer at {client.host}:{client.port} is unreachable"
            f" ({unreachable!r})"
        ) from unreachable
    return client


def _link(peer: PeerClient) -> str:
    return (
        "loopback" if peer.host in ("127.0.0.1", "localhost", "::1") else "lan"
    )


# --------------------------------------------------------------------------
# Clients
# --------------------------------------------------------------------------


class Client:
    """One client's session and stream against the remote service."""

    def __init__(self, session, stream) -> None:
        self.session = session
        self.stream = stream
        self.failures = 0

    def _call(self, schema: a11.ActionSchema):
        return (
            a11
            .Action(schema)
            .bind_node_map(self.session.node_map)
            .bind_session(self.session)
            .bind_stream(self.stream)
        )

    async def echo(self, payload: str = "payload") -> None:
        call = self._call(ECHO)
        await call.call()
        async with call["text"] as port:
            await port.put_final(payload)
        await call["out"].consume(str)
        await call.wait(_WAIT)

    async def sink(self, count: int, size: int) -> int:
        """Stream `count` chunks of `size` into the server's input port."""
        chunk = types.Chunk(
            data=b"u" * size, metadata=types.ChunkMetadata(mimetype=_OCTET)
        )
        call = self._call(SINK)
        await call.call()
        async with call["data"] as port:
            for _index in range(count - 1):
                await port.put_chunk(chunk)
            await port.put_chunk(chunk, final=True)
        report = await call["report"].consume(dict)
        await call.wait(_WAIT)
        return int(report.get("bytes", 0))

    async def source(self, count: int, size: int) -> int:
        """Ask the server to stream `count` chunks of `size` back."""
        call = self._call(SOURCE)
        await call.call()
        async with call["request"] as port:
            await port.put_final({"count": count, "size": size})
        total = 0
        port = call["data"]
        while True:
            batch = await port.next_fragments(64)
            done = False
            for fragment in batch:
                if fragment is None:
                    done = True
                    break
                total += len(fragment.data.data) if fragment.data else 0
            if done:
                break
        await call.wait(_WAIT)
        return total

    async def store(self, count: int, size: int, batch: int = 8) -> int:
        """Server-side write-and-read-back; no payload crosses the wire."""
        call = self._call(STORE)
        await call.call()
        async with call["request"] as port:
            await port.put_final({"count": count, "size": size, "batch": batch})
        report = await call["report"].consume(dict)
        await call.wait(_WAIT)
        return int(report.get("read", 0))


class Fleet:
    """Client connections to the peer's service, and how to churn them."""

    def __init__(self, peer: PeerClient, transport: str, port: int) -> None:
        self.peer = peer
        self.transport = transport
        self.port = port
        self.clients: list[Client] = []
        self.joins = 0
        self.leaves = 0
        self.join_failures = 0

    def _stream(self):
        if self.transport == "websocket":
            options = net.WebSocketClientOptions()
            options.http2_options.enable_h2 = False
            options.http2_options.enable_h2c = False
            return net.WebSocketWireStream.connect(
                f"ws://{self.peer.host}:{self.port}/bench",
                websocket_options=options,
            )
        if self.transport == "sse":
            return net.HttpSseClientWireStream(
                f"http://{self.peer.host}:{self.port}"
            )
        raise ValueError(self.transport)

    async def connect(self, track: bool = True) -> Client:
        stream = self._stream()
        session = a11.Session(action_registry=a11.ActionRegistry())
        await asyncio.wait_for(
            session.add_stream(stream, mode="start"),
            timeout=_CONNECT_TIMEOUT,
        )
        client = Client(session, stream)
        self.joins += 1
        if track:
            self.clients.append(client)
        return client

    async def disconnect(self, client: Client) -> None:
        """Half-close, drain, then **abort** -- and the abort is load-bearing.

        `half_close()` closes the write half. It does not close the connection,
        because a half-closed connection is still a connection: the peer's write
        half is open and the socket stays. Measured directly, with concurrency
        one against a real service: `half_close()` plus
        `drain_outgoing_messages()` retains **1.04 file descriptors per
        connection**, and so does simply dropping the last Python reference and
        collecting it. `abort()` holds flat.

        That cost this suite two rounds of wrong numbers. With a default
        `ulimit -n` of 1024 the churn population died at about a thousand cycles
        and reported it as the *server* refusing connections -- three different
        error strings across three configurations
        (`connection reset by peer`, `HTTP/1.1 connection is not connected`,
        `Starting HTTP/2 TCP connection failed: too many open files`), of which
        only the last names the actual cause. The first two were healthy
        connections failing downstream of the client's exhausted descriptor
        table.

        So: drain for the graceful path, because that is what an application
        does and it is what makes the peer see an orderly end of stream, then
        abort to reclaim the socket. See `FINDINGS.md`, "A closed client stream
        keeps its file descriptor".
        """
        with contextlib.suppress(Exception):
            client.stream.half_close()
            await asyncio.wait_for(
                client.stream.drain_outgoing_messages(), timeout=5.0
            )
        with contextlib.suppress(Exception):
            client.stream.abort(
                a11.Status(code=a11.StatusCode.CANCELLED, message="client left")
            )
        self.leaves += 1
        if client in self.clients:
            self.clients.remove(client)

    async def aclose(self) -> None:
        for client in list(self.clients):
            await self.disconnect(client)


@contextlib.asynccontextmanager
async def _fleet(peer: PeerClient, transport: str):
    port = (await peer.call("service", transport=transport, port=0))["port"]
    fleet = Fleet(peer, transport, port)
    try:
        yield fleet
    finally:
        with contextlib.suppress(Exception):
            await fleet.aclose()
        with contextlib.suppress(Exception):
            await peer.teardown()


# --------------------------------------------------------------------------
# Driving a window
# --------------------------------------------------------------------------


class Tally:
    """Completions and their latencies, timestamped so they can be bucketed."""

    def __init__(self, label: str) -> None:
        self.label = label
        self.samples: list[float] = []
        self.finished_at: list[float] = []
        self.failures = 0
        self.items = 0
        self.payload_bytes = 0
        #: Failure kind -> count, and the first message seen for each.
        #:
        #: A failure count alone is not actionable. The soak row first reported
        #: 2585 failed connection attempts against 994 successes and it was not
        #: possible to say from the record whether that was a refused connect, a
        #: handshake timeout or an aborted session -- three problems with three
        #: different owners. The kind is one dict and settles it.
        self.failure_kinds: dict[str, int] = {}
        self.failure_examples: dict[str, str] = {}

    def record(self, duration_ns: float, at: float) -> None:
        self.samples.append(duration_ns)
        self.finished_at.append(at)

    def blame(self, failure: BaseException) -> None:
        kind = type(failure).__name__
        text = str(failure)[:160] or repr(failure)[:160]
        # A StatusException's code is the useful discriminator, not its type.
        code = getattr(getattr(failure, "status", None), "code", None)
        if code is not None:
            kind = f"{kind}({code})"
        self.failures += 1
        self.failure_kinds[kind] = self.failure_kinds.get(kind, 0) + 1
        self.failure_examples.setdefault(kind, text)

    def blame_summary(self) -> str:
        if not self.failure_kinds:
            return ""
        ranked = sorted(self.failure_kinds.items(), key=lambda item: -item[1])
        parts = [
            f"{count}x {kind}: {self.failure_examples[kind]}"
            for kind, count in ranked[:3]
        ]
        return "; ".join(parts)

    def metrics(self, elapsed: float) -> dict[str, float]:
        found = _tail(self.samples)
        found["ops_per_s"] = len(self.samples) / elapsed if elapsed else 0.0
        found["completed"] = float(len(self.samples))
        found["failures"] = float(self.failures)
        if self.payload_bytes:
            found["mib_per_s"] = (
                self.payload_bytes / elapsed / (1024 * 1024) if elapsed else 0.0
            )
        return found

    def buckets(self, started: float, elapsed: float) -> list[int]:
        """Completions per whole second of the window."""
        count = max(int(elapsed), 1)
        buckets = [0] * count
        for at in self.finished_at:
            index = int(at - started)
            if 0 <= index < count:
                buckets[index] += 1
        return buckets


async def _drive(
    operation,
    tally: Tally,
    deadline: float,
    stop: asyncio.Event,
) -> None:
    """Run `operation` back to back until the deadline, recording each call.

    A failure is counted and the loop continues. That is deliberate: on a
    server benchmark the interesting question is how many calls a load *lost*,
    and a single exception that ends the driver converts a partial failure into
    an absent row.
    """
    while not stop.is_set() and time.perf_counter() < deadline:
        started = time.perf_counter_ns()
        try:
            await asyncio.wait_for(operation(), timeout=_CALL_TIMEOUT)
        except asyncio.CancelledError:
            raise
        except Exception as failure:  # noqa: BLE001 - a lost call is data
            tally.blame(failure)
            continue
        now = time.perf_counter_ns()
        tally.record(now - started, now / 1e9)


async def _churn(
    fleet: Fleet,
    tally: Tally,
    deadline: float,
    stop: asyncio.Event,
    concurrency: int,
) -> None:
    """Join, do one call, leave -- continuously, `concurrency` at a time.

    This is the load a real deployment has that no isolated benchmark does:
    connection lifecycle work interleaved with steady traffic. A join is a
    session, a stream, a handshake and a first dispatch; a leave is a
    half-close and everything the server unwinds behind it.
    """

    async def one() -> None:
        client = await fleet.connect(track=False)
        try:
            await client.echo()
        finally:
            await fleet.disconnect(client)

    async def worker() -> None:
        while not stop.is_set() and time.perf_counter() < deadline:
            started = time.perf_counter_ns()
            try:
                await asyncio.wait_for(one(), timeout=_CALL_TIMEOUT)
            except asyncio.CancelledError:
                raise
            except Exception as failure:  # noqa: BLE001
                tally.blame(failure)
                fleet.join_failures += 1
                continue
            now = time.perf_counter_ns()
            tally.record(now - started, now / 1e9)

    await asyncio.gather(*(worker() for _ in range(concurrency)))


async def _run_window(
    peer: PeerClient,
    drivers: list[tuple[Tally, Any]],
    seconds: float,
) -> tuple[float, ServerCost, float]:
    """Run every driver concurrently for `seconds`; return elapsed and cost.

    The server stats are sampled outside the clock, before the first driver
    starts and after the last one stops, so the control channel's own latency
    is never inside a measured window.
    """
    stop = asyncio.Event()
    before = await peer.stats()
    started = time.perf_counter()
    deadline = started + seconds
    tasks = [
        asyncio.ensure_future(factory(tally, deadline, stop))
        for tally, factory in drivers
    ]
    try:
        await asyncio.wait_for(
            asyncio.gather(*tasks, return_exceptions=True),
            timeout=seconds + _CALL_TIMEOUT * 2,
        )
    except asyncio.TimeoutError:
        stop.set()
        for task in tasks:
            task.cancel()
        with contextlib.suppress(asyncio.CancelledError, Exception):
            await asyncio.gather(*tasks, return_exceptions=True)
    elapsed = time.perf_counter() - started
    after = await peer.stats()
    return elapsed, ServerCost(before, after), started


def _steady_driver(
    clients: list[Client], operation_name: str = "echo", **kwargs
):
    """A factory that puts every client in `clients` in a loop of one call."""

    def factory(tally: Tally, deadline: float, stop: asyncio.Event):
        async def run() -> None:
            async def per_client(client: Client) -> None:
                method = getattr(client, operation_name)

                async def call():
                    return await method(**kwargs)

                await _drive(call, tally, deadline, stop)

            await asyncio.gather(*(per_client(c) for c in clients))

        return run()

    return factory


# --------------------------------------------------------------------------
# Rows
# --------------------------------------------------------------------------


@benchmark(SUITE, "join_rate")
async def join_rate(scale: float) -> list[Result]:
    """How fast clients can arrive, and whether an existing population slows it.

    Two different numbers, both of which get called "connections per second":

    * **sequential** -- connect, dispatch one action, and only then start the
      next. This is one client's experience of joining, and it is round trips,
      so on a LAN it is bounded by the link.
    * **concurrent** -- `batch` clients joining at once. This is the
      accept-rate ceiling, the number a reconnect storm after a deploy is sized
      against, and it is bounded by the server.

    Measured with an idle population already connected and again with a large
    one, because an accept path that walks a session list is fine at ten and
    quadratic at a thousand.
    """
    peer = await _peer()
    link = _link(peer)
    results: list[Result] = []
    try:
        async with _fleet(peer, "websocket") as fleet:
            batch = _scaled(24, scale)
            for background in (0, _scaled(120, scale)):
                while len(fleet.clients) < background:
                    await fleet.connect()

                # Sequential: join then use, one at a time.
                samples: list[float] = []
                joined: list[Client] = []
                before = await peer.stats()
                for _index in range(batch):
                    started = time.perf_counter_ns()
                    client = await fleet.connect(track=False)
                    await client.echo()
                    samples.append(time.perf_counter_ns() - started)
                    joined.append(client)
                after = await peer.stats()
                metrics = _tail(samples)
                total = sum(samples) / 1e9
                metrics["ops_per_s"] = batch / total if total else 0.0
                metrics.update(ServerCost(before, after).metrics(batch))
                results.append(
                    Result(
                        SUITE,
                        "join_then_call",
                        metrics,
                        {
                            "link": link,
                            "transport": "websocket",
                            "idle_population": background,
                        },
                        note=(
                            "one client's join-to-first-result; sequential, so"
                            " ops/s is 1/latency and not a server ceiling"
                        ),
                    )
                )
                for client in joined:
                    await fleet.disconnect(client)

                # Concurrent: the accept ceiling.
                before = await peer.stats()
                started = time.perf_counter()
                results_or_errors = await asyncio.gather(
                    *(fleet.connect(track=False) for _index in range(batch)),
                    return_exceptions=True,
                )
                elapsed = time.perf_counter() - started
                after = await peer.stats()
                opened = [
                    item
                    for item in results_or_errors
                    if isinstance(item, Client)
                ]
                failures = len(results_or_errors) - len(opened)
                metrics = {
                    "ops_per_s": len(opened) / elapsed if elapsed else 0.0,
                    "elapsed_s": elapsed,
                    "completed": float(len(opened)),
                    "failures": float(failures),
                    **ServerCost(before, after).metrics(max(len(opened), 1)),
                }
                results.append(
                    Result(
                        SUITE,
                        "join_burst",
                        metrics,
                        {
                            "link": link,
                            "transport": "websocket",
                            "idle_population": background,
                        },
                        note=(
                            f"{batch} clients dialling at once;"
                            f" server reports {after.get('sessions')} sessions."
                            " This is the reconnect-storm number"
                        ),
                    )
                )
                for client in opened:
                    await fleet.disconnect(client)
    finally:
        await peer.aclose()
    return results


@benchmark(SUITE, "churn_under_load")
async def churn_under_load(scale: float) -> list[Result]:
    """The headline: what joining and leaving does to the clients already here.

    A steady population loops echo actions for a window. Then the *same*
    population loops for a second window while clients continuously join, make
    a call and leave. Both windows are the same length, on the same
    connections, against the same server process, so the difference is
    attributable and the ratio is reported as `degradation`.

    What to look at, in order:

    1. `p99_us` and `p999_us` on the `steady` rows. The tail is where
       connection lifecycle work shows up first, and a p50 that does not move
       while p999 triples is the normal shape of this failure.
    2. `failures`. A steady client whose call did not come back inside
       {timeout}s is a lost request, not a slow one.
    3. `server_cores_busy`. If churn costs 4x the CPU per action, the number to
       fix is the join path, not the dispatch path.
    """
    peer = await _peer()
    link = _link(peer)
    results: list[Result] = []
    seconds = _window_seconds(6.0, scale)
    steady_count = _scaled(16, scale)
    churn_concurrency = max(_scaled(6, scale), 2)
    try:
        async with _fleet(peer, "websocket") as fleet:
            for _index in range(steady_count):
                await fleet.connect()
            # One call each before any clock starts: the first dispatch on a
            # session pays for whatever it sets up lazily, and charging that to
            # the quiet window makes churn look free by comparison.
            await asyncio.gather(*(c.echo() for c in fleet.clients))

            quiet = Tally("steady")
            elapsed, cost, _ = await _run_window(
                peer,
                [(quiet, _steady_driver(fleet.clients))],
                seconds,
            )
            quiet_metrics = quiet.metrics(elapsed)
            quiet_metrics.update(cost.metrics(len(quiet.samples)))
            quiet_metrics["elapsed_s"] = elapsed
            results.append(
                Result(
                    SUITE,
                    "steady_actions",
                    quiet_metrics,
                    {
                        "link": link,
                        "clients": steady_count,
                        "churn": "off",
                    },
                    note=(
                        f"{steady_count} clients, {seconds:.0f}s window, no"
                        " connection churn -- the control for the row below"
                    ),
                )
            )

            loaded = Tally("steady")
            churned = Tally("churn")
            elapsed, cost, _ = await _run_window(
                peer,
                [
                    (loaded, _steady_driver(fleet.clients)),
                    (
                        churned,
                        lambda tally, deadline, stop: _churn(
                            fleet, tally, deadline, stop, churn_concurrency
                        ),
                    ),
                ],
                seconds,
            )
            loaded_metrics = loaded.metrics(elapsed)
            loaded_metrics.update(
                cost.metrics(len(loaded.samples) + len(churned.samples))
            )
            loaded_metrics["elapsed_s"] = elapsed
            for key in ("ops_per_s", "p50_us", "p99_us", "p999_us"):
                quiet_value = quiet_metrics.get(key)
                loaded_value = loaded_metrics.get(key)
                if quiet_value and loaded_value:
                    ratio = (
                        loaded_value / quiet_value
                        if key != "ops_per_s"
                        else quiet_value / loaded_value
                    )
                    loaded_metrics[f"degradation_{key}"] = ratio
            results.append(
                Result(
                    SUITE,
                    "steady_actions",
                    loaded_metrics,
                    {
                        "link": link,
                        "clients": steady_count,
                        "churn": "on",
                    },
                    note=(
                        f"same {steady_count} clients while"
                        f" {churn_concurrency} joiners cycle;"
                        " `degradation_*` is this window over the quiet one"
                        " (>1 is worse)"
                    ),
                )
            )
            churn_metrics = churned.metrics(elapsed)
            churn_metrics["elapsed_s"] = elapsed
            results.append(
                Result(
                    SUITE,
                    "churn_cycle",
                    churn_metrics,
                    {"link": link, "concurrency": churn_concurrency},
                    note=(
                        "one connect + one action + one disconnect per"
                        " completion, measured while the steady population was"
                        " working; ops/s is the sustained churn rate."
                        + (
                            f" FAILURES: {churned.blame_summary()}"
                            if churned.failures
                            else ""
                        )
                    ),
                )
            )
    finally:
        await peer.aclose()
    return results


@benchmark(SUITE, "mixed_workload")
async def mixed_workload(scale: float) -> list[Result]:
    """Four populations alone, then all at once: who starves whom.

    The populations, and what each is for:

    * `rpc` -- echo actions. Small, latency-shaped, the dispatch unit.
    * `upstream` -- clients streaming 64 KiB chunks *into* the server.
    * `downstream` -- clients asking the server to stream chunks *out*.
    * `data` -- `bench-store`, which writes and reads back fragments in a
      server-side store and puts **no payload on the wire at all**.

    That last one is why this row can distinguish two very different failures.
    If mixing costs the byte-moving populations throughput and leaves `data`
    alone, the link is saturated and no server work will help. If `data`
    degrades too, the host is busy and the transport is not the problem.

    `interference` on each row is that population's solo rate over its rate in
    the mix. Above 1 it lost throughput to the others; a fair server under a
    saturated resource shows all four somewhat above 1, and an unfair one shows
    one population near 1 and another at 5.
    """
    peer = await _peer()
    link = _link(peer)
    results: list[Result] = []
    seconds = _window_seconds(5.0, scale)
    per_population = max(_scaled(4, scale), 2)
    populations = {
        "rpc": ("echo", {}),
        "upstream": ("sink", {"count": 16, "size": 65536}),
        "downstream": ("source", {"count": 16, "size": 65536}),
        "data": ("store", {"count": 256, "size": 512, "batch": 16}),
    }
    try:
        async with _fleet(peer, "websocket") as fleet:
            groups: dict[str, list[Client]] = {}
            for name in populations:
                groups[name] = [
                    await fleet.connect() for _ in range(per_population)
                ]
            solo: dict[str, float] = {}

            for name, (operation, kwargs) in populations.items():
                tally = Tally(name)
                elapsed, cost, _ = await _run_window(
                    peer,
                    [
                        (
                            tally,
                            _steady_driver(groups[name], operation, **kwargs),
                        )
                    ],
                    seconds,
                )
                metrics = tally.metrics(elapsed)
                metrics.update(cost.metrics(max(len(tally.samples), 1)))
                metrics["elapsed_s"] = elapsed
                solo[name] = metrics["ops_per_s"]
                results.append(
                    Result(
                        SUITE,
                        "population_alone",
                        metrics,
                        {
                            "link": link,
                            "population": name,
                            "clients": per_population,
                        },
                    )
                )

            tallies = {name: Tally(name) for name in populations}
            drivers = [
                (
                    tallies[name],
                    _steady_driver(groups[name], operation, **kwargs),
                )
                for name, (operation, kwargs) in populations.items()
            ]
            elapsed, cost, _ = await _run_window(peer, drivers, seconds)
            total_ops = sum(len(t.samples) for t in tallies.values())
            for name in populations:
                tally = tallies[name]
                metrics = tally.metrics(elapsed)
                metrics["elapsed_s"] = elapsed
                if solo.get(name) and metrics["ops_per_s"]:
                    metrics["interference"] = solo[name] / metrics["ops_per_s"]
                results.append(
                    Result(
                        SUITE,
                        "population_mixed",
                        metrics,
                        {
                            "link": link,
                            "population": name,
                            "clients": per_population,
                        },
                        note=(
                            "`interference` is its solo rate over this one;"
                            " compare the four to each other, not to 1"
                        ),
                    )
                )
            whole = cost.metrics(max(total_ops, 1))
            whole["ops_per_s"] = total_ops / elapsed if elapsed else 0.0
            whole["elapsed_s"] = elapsed
            results.append(
                Result(
                    SUITE,
                    "mixed_total",
                    whole,
                    {
                        "link": link,
                        "clients": per_population * len(populations),
                    },
                    note=(
                        "every population at once; read `server_cores_busy`"
                        " before reading `ops_per_s` -- a rate reached on 2 of"
                        " 12 cores and one reached on 11 are different results"
                    ),
                )
            )
    finally:
        await peer.aclose()
    return results


@benchmark(SUITE, "disconnect_storm")
async def disconnect_storm(scale: float) -> list[Result]:
    """Half the population leaves at once. What do the survivors feel?

    A deployment loses many clients simultaneously for ordinary reasons -- a
    load balancer draining, a client rollout, a wifi access point going away --
    and the unwinding is server work that lands all at once. The survivors are
    measured continuously across the event, and their latency is bucketed
    before and during, because an average over the whole window hides exactly
    the spike this row exists to find.
    """
    peer = await _peer()
    link = _link(peer)
    results: list[Result] = []
    population = _scaled(40, scale)
    seconds = _window_seconds(6.0, scale)
    try:
        async with _fleet(peer, "websocket") as fleet:
            survivors = [await fleet.connect() for _ in range(population // 2)]
            doomed = [
                await fleet.connect()
                for _ in range(population - population // 2)
            ]
            await asyncio.gather(*(c.echo() for c in survivors + doomed))

            tally = Tally("survivors")
            stop = asyncio.Event()
            before = await peer.stats()
            started = time.perf_counter()
            deadline = started + seconds
            driver = asyncio.ensure_future(
                _steady_driver(survivors)(tally, deadline, stop)
            )
            # Let the survivors establish a baseline, then drop the others in
            # one go, and keep measuring through it.
            await asyncio.sleep(seconds / 3)
            drop_at = time.perf_counter()
            await asyncio.gather(
                *(fleet.disconnect(client) for client in doomed),
                return_exceptions=True,
            )
            drop_done = time.perf_counter()
            with contextlib.suppress(asyncio.TimeoutError):
                await asyncio.wait_for(driver, timeout=seconds * 2)
            if not driver.done():
                stop.set()
                driver.cancel()
                with contextlib.suppress(asyncio.CancelledError, Exception):
                    await driver
            elapsed = time.perf_counter() - started
            after = await peer.stats()

            quiet_samples = [
                duration
                for duration, at in zip(tally.samples, tally.finished_at)
                if at < drop_at
            ]
            storm_samples = [
                duration
                for duration, at in zip(tally.samples, tally.finished_at)
                if drop_at <= at <= drop_done + 1.0
            ]
            metrics = tally.metrics(elapsed)
            metrics.update(
                ServerCost(before, after).metrics(len(tally.samples))
            )
            metrics["elapsed_s"] = elapsed
            metrics["drop_seconds"] = drop_done - drop_at
            metrics["dropped"] = float(len(doomed))
            if quiet_samples:
                metrics["before_p50_us"] = _tail(quiet_samples)["p50_us"]
                metrics["before_p99_us"] = _tail(quiet_samples)["p99_us"]
            if storm_samples:
                storm = _tail(storm_samples)
                metrics["during_p50_us"] = storm["p50_us"]
                metrics["during_p99_us"] = storm["p99_us"]
                metrics["during_max_us"] = storm["max_us"]
                if quiet_samples:
                    metrics["spike_p99"] = (
                        storm["p99_us"] / metrics["before_p99_us"]
                        if metrics.get("before_p99_us")
                        else 0.0
                    )
            results.append(
                Result(
                    SUITE,
                    "survivor_latency",
                    metrics,
                    {"link": link, "dropped": len(doomed)},
                    note=(
                        f"{len(survivors)} clients working throughout;"
                        f" {len(doomed)} disconnected at once in"
                        f" {metrics['drop_seconds'] * 1000:.0f}ms."
                        " `spike_p99` is during-the-storm over before-it"
                    ),
                )
            )
    finally:
        await peer.aclose()
    return results


@benchmark(SUITE, "soak", slow=True)
async def soak(scale: float) -> list[Result]:
    """Hold a mixed load for minutes and ask whether the rate is *steady*.

    **Known to hang on macOS over loopback at a 36-second window**
    (`--scale 0.6`), reproduced twice, while a 20-second window
    (`--scale 0.3`) completes in 21s -- and the same 36-second window
    completes on Linux over loopback and on both machines over the LAN. It
    is a stall, not a spin: 19% of one core over six minutes.

    `_run_window` bounds and cancels its own gather, so the stall is *after*
    the measured window. Read `Fleet.aclose()` and `PeerClient.teardown()`,
    and remember that `drain_outgoing_messages` hangs forever when a peer is
    not reading. `kill -ABRT` both processes to see it; a fibre-level stall
    is invisible to `sample`. Recorded in `FINDINGS.md` under the soak table.

    Marked slow because it is long, not because it is fragile. Everything else
    in this suite runs for seconds, and seconds cannot see the two things that
    ruin a server in production: a slow drift, and a periodic stall.

    The metrics are about shape rather than magnitude:

    * `cov` -- standard deviation of the per-second completion counts over
      their mean. Under about 0.1 the rate is flat enough to put a budget on;
      above 0.3 the mean is not a number anybody can plan with.
    * `drift` -- the last third of the window's mean rate over the first
      third's. Below 1 the server is getting slower as it runs, which is the
      signature of something accumulating.
    * `worst_second` against `mean_per_s` -- the floor a client actually
      experiences.
    * `server_rss_bytes` at the end against the start, which is where a leak
      shows up as a number rather than a suspicion.
    """
    peer = await _peer()
    link = _link(peer)
    seconds = max(60.0 * scale, 20.0)
    per_population = max(_scaled(4, scale), 2)
    results: list[Result] = []
    try:
        async with _fleet(peer, "websocket") as fleet:
            rpc = [await fleet.connect() for _ in range(per_population)]
            up = [await fleet.connect() for _ in range(per_population)]
            data = [await fleet.connect() for _ in range(per_population)]
            await asyncio.gather(*(c.echo() for c in rpc + up + data))

            tallies = {
                "rpc": Tally("rpc"),
                "upstream": Tally("upstream"),
                "data": Tally("data"),
            }
            churn = Tally("churn")
            drivers = [
                (tallies["rpc"], _steady_driver(rpc)),
                (
                    tallies["upstream"],
                    _steady_driver(up, "sink", count=16, size=65536),
                ),
                (
                    tallies["data"],
                    _steady_driver(
                        data, "store", count=256, size=512, batch=16
                    ),
                ),
                (
                    churn,
                    lambda tally, deadline, stop: _churn(
                        fleet, tally, deadline, stop, 2
                    ),
                ),
            ]
            before_rss = (await peer.stats())["rss_bytes"]
            elapsed, cost, started = await _run_window(peer, drivers, seconds)
            after = await peer.stats()

            for name, tally in list(tallies.items()) + [("churn", churn)]:
                buckets = tally.buckets(started, elapsed)
                # Drop the first and last bucket: both are partial windows and
                # a partial second reads as a stall.
                usable = buckets[1:-1] if len(buckets) > 3 else buckets
                if not usable:
                    continue
                mean = statistics.fmean(usable)
                metrics = tally.metrics(elapsed)
                metrics["elapsed_s"] = elapsed
                metrics["mean_per_s"] = mean
                metrics["worst_second"] = float(min(usable))
                metrics["best_second"] = float(max(usable))
                metrics["cov"] = (
                    statistics.stdev(usable) / mean
                    if len(usable) > 1 and mean
                    else 0.0
                )
                third = max(len(usable) // 3, 1)
                head = statistics.fmean(usable[:third])
                tail = statistics.fmean(usable[-third:])
                metrics["drift"] = tail / head if head else 0.0
                metrics["server_rss_growth_bytes"] = float(
                    after["rss_bytes"] - before_rss
                )
                results.append(
                    Result(
                        SUITE,
                        "soak_bucket",
                        metrics,
                        {"link": link, "population": name},
                        note=(
                            f"{len(usable)} whole seconds of"
                            f" {elapsed:.0f}s; cov {metrics['cov']:.3f},"
                            f" drift {metrics['drift']:.3f}, worst second"
                            f" {metrics['worst_second']:.0f} against mean"
                            f" {mean:.0f}"
                            + (
                                f". FAILURES {tally.failures} of"
                                f" {tally.failures + len(tally.samples)}"
                                f" attempts -- {tally.blame_summary()}"
                                if tally.failures
                                else ""
                            )
                        ),
                    )
                )
            total = sum(len(t.samples) for t in tallies.values()) + len(
                churn.samples
            )
            whole = cost.metrics(max(total, 1))
            whole["ops_per_s"] = total / elapsed if elapsed else 0.0
            whole["elapsed_s"] = elapsed
            whole["server_rss_growth_bytes"] = float(
                after["rss_bytes"] - before_rss
            )
            results.append(
                Result(
                    SUITE,
                    "soak_total",
                    whole,
                    {"link": link},
                    note=(
                        "every population plus churn for"
                        f" {elapsed:.0f}s; RSS growth is the leak row"
                    ),
                )
            )
    finally:
        await peer.aclose()
    return results


@benchmark(SUITE, "capacity_ramp", slow=True)
async def capacity_ramp(scale: float) -> list[Result]:
    """Actions per second against client count, until it stops improving.

    The gateway-sizing curve, and the point of it is the *knee*, not the peak:
    where adding clients stops adding throughput, what the tail is doing at
    that point, and how much of the host is in use when it happens. A knee at
    2 cores busy on a 12-core host is a serialisation inside A11; a knee at 11
    is the host.
    """
    peer = await _peer()
    link = _link(peer)
    seconds = _window_seconds(5.0, scale)
    results: list[Result] = []
    counts = [1, 4, 16, 64, 192]
    try:
        async with _fleet(peer, "websocket") as fleet:
            for count in counts:
                scaled = max(int(count * min(scale * 2, 1.0)), 1)
                while len(fleet.clients) < scaled:
                    await fleet.connect()
                clients = fleet.clients[:scaled]
                await asyncio.gather(
                    *(c.echo() for c in clients), return_exceptions=True
                )
                tally = Tally("rpc")
                elapsed, cost, _ = await _run_window(
                    peer, [(tally, _steady_driver(clients))], seconds
                )
                metrics = tally.metrics(elapsed)
                metrics.update(cost.metrics(max(len(tally.samples), 1)))
                metrics["elapsed_s"] = elapsed
                metrics["clients"] = float(scaled)
                results.append(
                    Result(
                        SUITE,
                        "actions_by_clients",
                        metrics,
                        {"link": link, "clients": scaled},
                    )
                )
        if results:
            best = max(r.metrics["ops_per_s"] for r in results)
            first = results[0].metrics["ops_per_s"]
            knee = next(
                (r for r in results if r.metrics["ops_per_s"] >= best * 0.95),
                results[-1],
            )
            results[-1].note = (
                f"peak {best:.0f}/s at {knee.params['clients']} clients"
                f" ({best / first:.1f}x one client), with"
                f" {knee.metrics.get('server_cores_busy', 0):.1f} server cores"
                " busy there"
            )
    finally:
        await peer.aclose()
    return results
