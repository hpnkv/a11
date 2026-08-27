# Copyright 2026 The A11 Authors.

"""The far side of a two-host benchmark: a server, and a way to ask it things.

Every other suite in `bench/` measures A11 with both ends of the connection in
one process, or in two processes on one machine. That is the only way to
attribute a microsecond, and it is also the only way to be wrong about a
network: loopback has no propagation delay, no MTU, no queue discipline, no
NIC, and no second machine whose clock and cores are not yours.

This module is the missing half. `python -m bench.peer` runs on the *other*
host and does three things:

* **serves** -- a raw TCP echo and sink with no A11 in them at all (the link
  floor), an A11 `WireStream` echo per transport (the protocol cost), and a
  real `a11.Service` with a registry of workload actions (the server);
* **reports its own resources** -- CPU seconds, resident bytes, live session
  count, and interface byte counters sampled inside the measured process. This
  allows cross-machine comparisons to distinguish throughput from CPU cost;
* **stays out of the measurement** -- the control channel is a separate TCP
  connection carrying one JSON object per line, used only between measured
  windows. Nothing on it is timed, and no benchmark sends anything on it while
  a clock is running.

The client half is `PeerClient`, used by `bench/suites/link.py` and
`bench/suites/server.py`. Both skip unless `A11_BENCH_PEER=host:port` names a
reachable agent, so a single-machine run is unaffected.

Run it like this, on the host that is to be the server:

```bash
python -m bench.peer --bind 0.0.0.0 --port 8899
```

and point the benchmark host at it:

```bash
A11_BENCH_PEER=192.168.1.209:8899 python -m bench --suite link --suite server
```

**Point it at itself for a control.** `A11_BENCH_PEER=127.0.0.1:8899` runs the
identical code over loopback, which is the only defensible baseline for a LAN
number: it holds the code, the harness and the two-process topology still and
changes nothing but the wire. A LAN figure quoted against the in-process `wire`
suite instead conflates three differences at once.

## The workload actions, and why these four

A "real server" is not a round-trip benchmark repeated. It is a mixture, and
the mixture is the point -- so the registry holds one action per kind of cost a
server actually carries, and `bench/suites/server.py` runs them together:

| action | the cost it isolates |
|---|---|
| `bench-echo` | dispatch: a call, two nodes, a session, a round trip.
  The actions/s unit. |
| `bench-sink` | inbound data plane: the client streams payload into an
  input port. |
| `bench-source` | outbound data plane: the server streams payload out of an
  output port. |
| `bench-store` | pure data management: fragments written to and read back
  from a server-side store, with no payload on the wire at all. |

`bench-store` exists precisely because it moves no bytes: when a mixed run
degrades, it separates "the transport is saturated" from "the server is busy".
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import hashlib
import json
import os
import platform
import resource
import socket
import sys
import time
from typing import Any

import a11
from a11 import net
from a11.data import types
from a11.net.wire_stream import WireStreamWithRecv

#: The control protocol's framing: one JSON object per line, both directions.
#: Uses JSON lines instead of A11 framing so the control channel remains
#: independent when the A11 server under test is unresponsive.
_ENCODING = "utf-8"

_WAIT = a11.timing.Duration.seconds(60)

#: Raw bytes need a mimetype or the object reader cannot decode them; see the
#: `text/plain has no deserializer` rule in the project notes.
_OCTET = "application/octet-stream"


# --------------------------------------------------------------------------
# The workload registry
# --------------------------------------------------------------------------


ECHO = a11.ActionSchema(
    name="bench-echo",
    description="Return the input unchanged.",
    inputs={
        "text": a11.ActionPortSchema(
            name="text", type="text/plain", unary=True, required=True
        )
    },
    outputs={"out": a11.ActionPortSchema(name="out", type="text/plain")},
)

SINK = a11.ActionSchema(
    name="bench-sink",
    description="Consume a stream of chunks and report what arrived.",
    inputs={
        "data": a11.ActionPortSchema(name="data", type=_OCTET, required=True)
    },
    outputs={
        "report": a11.ActionPortSchema(
            name="report", type="application/json", unary=True
        )
    },
)

SOURCE = a11.ActionSchema(
    name="bench-source",
    description="Produce `count` chunks of `size` bytes.",
    inputs={
        "request": a11.ActionPortSchema(
            name="request", type="application/json", unary=True, required=True
        )
    },
    outputs={"data": a11.ActionPortSchema(name="data", type=_OCTET)},
)

STORE = a11.ActionSchema(
    name="bench-store",
    description="Write and read back fragments in a server-side store.",
    inputs={
        "request": a11.ActionPortSchema(
            name="request", type="application/json", unary=True, required=True
        )
    },
    outputs={
        "report": a11.ActionPortSchema(
            name="report", type="application/json", unary=True
        )
    },
)

SCHEMAS = (ECHO, SINK, SOURCE, STORE)


class _Counters:
    """What the server thinks it did, so a client's count can be checked.

    A client that measures only its own completions cannot tell a slow server
    from a server that dropped the work. These are read between windows and
    compared against the client's own tally; a mismatch invalidates the row
    rather than being averaged into it.
    """

    def __init__(self) -> None:
        self.actions = 0
        self.bytes_in = 0
        self.bytes_out = 0
        self.fragments_in = 0
        self.fragments_out = 0
        self.failures = 0
        self.accepts = 0
        self.closes = 0
        self.compute_rounds = 0

    def snapshot(self) -> dict[str, int]:
        return dict(vars(self))


_counters = _Counters()

#: Node ids must be unique per store; `id(action)` is not, because CPython
#: reuses addresses the moment an action is collected, and two live nodes with
#: one id is a silent data mix.
_STORE_IDS = iter(range(1 << 40))


async def _handle_echo(action: a11.Action) -> None:
    text = await action["text"].consume(str)
    await action["out"].finalize(text)
    _counters.actions += 1


async def _handle_sink(action: a11.Action) -> None:
    """Drain the input port, counting fragments and bytes.

    Reads chunks rather than objects: this benchmark is about moving payload,
    and a deserialization the application would not do belongs in the `data`
    suite, not here.
    """
    total = 0
    fragments = 0
    port = action["data"]
    while True:
        # Batched: `next_fragments` returns whatever is already buffered and
        # waits only when nothing is, so a hundred arriving chunks cost one
        # event-loop turn rather than a hundred. Reading one at a time here
        # would make the server's own consumer the bottleneck and the row would
        # be measuring the benchmark.
        batch = await port.next_fragments(64)
        for fragment in batch:
            if fragment is None:
                _counters.bytes_in += total
                _counters.fragments_in += fragments
                await action["report"].finalize({
                    "bytes": total,
                    "fragments": fragments,
                })
                _counters.actions += 1
                return
            total += len(fragment.data.data) if fragment.data else 0
            fragments += 1


async def _handle_source(action: a11.Action) -> None:
    request = await action["request"].consume(dict)
    count = int(request.get("count", 1))
    size = int(request.get("size", 64))
    chunk = types.Chunk(
        data=b"s" * size, metadata=types.ChunkMetadata(mimetype=_OCTET)
    )
    port = action["data"]
    for _index in range(count - 1):
        await port.put_chunk(chunk)
    await port.finalize(chunk)
    _counters.bytes_out += count * size
    _counters.fragments_out += count
    _counters.actions += 1


async def _handle_store(action: a11.Action) -> None:
    """Write `count` fragments into a node and read them all back.

    The one workload here with no payload on the wire: the request and the
    report are tens of bytes and everything in between is the server's own
    store. When a mixed run slows down, the ratio between this row and the
    `sink`/`source` rows is what separates a saturated link from a busy host.
    """
    request = await action["request"].consume(dict)
    count = int(request.get("count", 64))
    size = int(request.get("size", 256))
    batch = int(request.get("batch", 1))
    chunk = types.Chunk(
        data=b"d" * size, metadata=types.ChunkMetadata(mimetype=_OCTET)
    )
    node = a11.AsyncNode.create(f"bench-store-{next(_STORE_IDS)}")
    written = 0
    pending = []
    for index in range(count):
        final = index == count - 1
        confirmation = await node.put_chunk(chunk, final=final)
        pending.append(confirmation)
        if len(pending) >= batch:
            for item in pending:
                await item
            pending.clear()
        written += 1
    for item in pending:
        await item
    # The last chunk above carried finality, so the node is only closed.
    await node.close()
    read = 0
    read_bytes = 0
    while True:
        batch = await node.next_fragments(64)
        done = False
        for fragment in batch:
            if fragment is None:
                done = True
                break
            read += 1
            read_bytes += len(fragment.data.data) if fragment.data else 0
        if done:
            break
    await action["report"].finalize({
        "written": written,
        "read": read,
        "bytes": read_bytes,
    })
    _counters.actions += 1


COMPUTE = a11.ActionSchema(
    name="bench-compute",
    description="Burn CPU on the server, to price a compute-bound handler.",
    inputs={
        "request": a11.ActionPortSchema(
            name="request", type="application/json", unary=True, required=True
        )
    },
    outputs={
        "report": a11.ActionPortSchema(
            name="report", type="application/json", unary=True
        )
    },
)


async def _handle_compute(action: a11.Action) -> None:
    """Hash a buffer `rounds` times, holding a core for a measurable while.

    This CPU-bound workload measures a small request's latency while other
    requests are computing rather than waiting. The pool cannot overlap that
    work as it can I/O.

    sha256 rather than a spin loop so the cost is real work a profiler will
    attribute, and so the figure is comparable to the `sha256 of 4 KiB` row in
    the reference table.
    """
    request = await action["request"].consume(dict)
    rounds = max(1, int(request.get("rounds", 64)))
    size = max(1, int(request.get("size", 4096)))
    buffer = b"c" * size
    digest = b""
    for _index in range(rounds):
        digest = hashlib.sha256(buffer + digest).digest()
    _counters.compute_rounds += rounds
    _counters.actions += 1
    await action["report"].finalize({
        "rounds": rounds,
        "digest": digest[:8].hex(),
    })


def registry() -> a11.ActionRegistry:
    """The workload registry, identical on both sides of the connection."""
    entries = a11.ActionRegistry()
    entries.register(ECHO.name, ECHO, _handle_echo)
    entries.register(SINK.name, SINK, _handle_sink)
    entries.register(SOURCE.name, SOURCE, _handle_source)
    entries.register(STORE.name, STORE, _handle_store)
    entries.register(COMPUTE.name, COMPUTE, _handle_compute)
    return entries


# --------------------------------------------------------------------------
# Resource accounting, from inside the process being measured
# --------------------------------------------------------------------------


def _rss_bytes() -> int:
    if sys.platform.startswith("linux"):
        try:
            with open("/proc/self/statm", "rb") as handle:
                pages = int(handle.read().split()[1])
            return pages * os.sysconf("SC_PAGE_SIZE")
        except (OSError, IndexError, ValueError):
            pass
    usage = resource.getrusage(resource.RUSAGE_SELF)
    return usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024)


def _open_file_count() -> int:
    """Return this process's descriptor count, or 0 when unavailable."""
    try:
        return len(os.listdir(f"/proc/{os.getpid()}/fd"))
    except OSError:
        pass
    # macOS has no /proc; lsof is the only portable answer and it is slow, so
    # this is a fallback rather than the path a Linux run takes.
    try:
        import subprocess  # noqa: PLC0415 - fallback only

        out = subprocess.run(
            ["lsof", "-p", str(os.getpid())],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        return max(0, len(out.stdout.splitlines()) - 1)
    except Exception:  # noqa: BLE001 - accounting must never fail a run
        return 0


def _cpu_seconds() -> float:
    """User + system CPU for the whole process, threads included.

    `getrusage(RUSAGE_SELF)` and not `time.process_time()`: A11 runs a worker
    pool, and a per-thread clock would attribute none of the runtime's work to
    the server. This is the number that turns "actions per second" into
    "CPU microseconds per action", which is the only form in which a 12-core
    host and a 14-core host can be compared.
    """
    usage = resource.getrusage(resource.RUSAGE_SELF)
    return usage.ru_utime + usage.ru_stime


def _interface_counters() -> dict[str, Any]:
    """Bytes in and out of the host's interfaces, for wire-inflation rows.

    Linux only, from `/proc/net/dev`. Whole-interface counters include whatever
    else the host is doing. `bench/suites/link.py` takes an idle control sample
    and reports it as the error bound for small effects such as base64 overhead.
    """
    if not sys.platform.startswith("linux"):
        return {}
    totals: dict[str, Any] = {}
    try:
        with open("/proc/net/dev") as handle:
            for line in handle.read().splitlines()[2:]:
                name, _, rest = line.partition(":")
                fields = rest.split()
                if len(fields) < 9:
                    continue
                name = name.strip()
                if name == "lo":
                    continue
                totals[name] = {
                    "rx_bytes": int(fields[0]),
                    "rx_packets": int(fields[1]),
                    "tx_bytes": int(fields[8]),
                    "tx_packets": int(fields[9]),
                }
    except (OSError, IndexError, ValueError):
        return {}
    return totals


def _environment() -> dict[str, Any]:
    try:
        import a11 as _a11

        version = getattr(_a11, "__version__", "unknown")
    except Exception:  # noqa: BLE001 - metadata must never fail a run
        version = "unknown"
    native: dict[str, Any] = {"path": "unknown"}
    try:
        from a11 import _native

        path = getattr(_native, "__file__", None)
        if path:
            stat = os.stat(path)
            native = {
                "path": path,
                "bytes": stat.st_size,
                "built": time.strftime(
                    "%Y-%m-%dT%H:%M:%S", time.localtime(stat.st_mtime)
                ),
            }
    except Exception:  # noqa: BLE001
        pass
    return {
        "hostname": socket.gethostname(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": sys.version.split()[0],
        "cpu_count": os.cpu_count(),
        "a11": version,
        "native": native,
        "pid": os.getpid(),
    }


# --------------------------------------------------------------------------
# The agent
# --------------------------------------------------------------------------


class Agent:
    """Servers this host is running, and the state to tear them down."""

    def __init__(self) -> None:
        self.service: a11.Service | None = None
        self._service_servers: list[Any] = []
        self._wire_servers: list[Any] = []
        self._raw_servers: list[asyncio.AbstractServer] = []
        self._wire_tasks: set[asyncio.Task] = set()
        self._sink_bytes = 0
        self._held: list[Any] = []

    # -- raw references, with no A11 in them ------------------------------

    async def tcp_echo(self, port: int) -> dict[str, Any]:
        """A bare socket ping-pong: the floor the link itself imposes.

        Length-prefix replies so they can be matched without a parser. Set
        `TCP_NODELAY` on both ends so one-request-at-a-time measurements capture
        link latency rather than Nagle delays.
        """

        async def serve(reader, writer) -> None:
            writer.transport.get_extra_info("socket").setsockopt(
                socket.IPPROTO_TCP, socket.TCP_NODELAY, 1
            )
            try:
                while True:
                    header = await reader.readexactly(4)
                    size = int.from_bytes(header, "big")
                    body = await reader.readexactly(size)
                    writer.write(len(body).to_bytes(4, "big") + body)
                    await writer.drain()
            except (
                asyncio.IncompleteReadError,
                ConnectionError,
                asyncio.CancelledError,
            ):
                pass
            finally:
                with contextlib.suppress(Exception):
                    writer.close()

        server = await asyncio.start_server(serve, "0.0.0.0", port)
        self._raw_servers.append(server)
        return {"port": server.sockets[0].getsockname()[1]}

    async def tcp_sink(self, port: int) -> dict[str, Any]:
        """A bare socket that reads and counts: the one-way link floor."""

        async def serve(reader, writer) -> None:
            try:
                while True:
                    block = await reader.read(1 << 16)
                    if not block:
                        break
                    self._sink_bytes += len(block)
            except (ConnectionError, asyncio.CancelledError):
                pass
            finally:
                with contextlib.suppress(Exception):
                    writer.close()

        server = await asyncio.start_server(serve, "0.0.0.0", port)
        self._raw_servers.append(server)
        return {"port": server.sockets[0].getsockname()[1]}

    def sink_bytes(self) -> dict[str, Any]:
        return {"bytes": self._sink_bytes}

    # -- A11 WireStream echo, per transport -------------------------------

    async def wire_echo(self, transport: str, port: int) -> dict[str, Any]:
        """Echo one fragment back per arriving fragment, in one message.

        The credit contract the `wire` suite settled on, and it is not
        cosmetic: both `Sender` loops fold whatever is already queued into the
        message they are about to deliver, so N messages sent can arrive as
        fewer, larger ones. A window counted in *messages* then waits for
        echoes that will never come separately and reports a stall that is not
        one. One fragment back per fragment in makes the credit exact through a
        fold without a second round trip.
        """

        async def on_stream(stream) -> None:
            endpoint = WireStreamWithRecv(stream)
            await endpoint.accept()
            self._held.append(endpoint)
            _counters.accepts += 1
            task = asyncio.ensure_future(self._echo_loop(endpoint))
            self._wire_tasks.add(task)
            task.add_done_callback(self._wire_tasks.discard)

        if transport == "websocket":
            options = net.WebSocketServerOptions()
            options.path = "/bench"
            options.bind_address = "0.0.0.0"
            options.port = port
            options.http2_options.enable_h2 = False
            options.http2_options.enable_h2c = False
            server = net.WebSocketWireServer.create(on_stream, options)
        elif transport == "sse":
            server = net.HttpSseServer.create(
                "0.0.0.0", port, on_stream, net.HttpSseOptions()
            )
        else:
            raise ValueError(f"unknown transport {transport!r}")
        self._wire_servers.append(server)
        return {"port": server.port}

    async def _echo_loop(self, endpoint) -> None:
        reply_bytes = 1
        try:
            while True:
                message = await endpoint.receive()
                if message is None:
                    break
                fragments = list(message.node_fragments or ())
                _counters.fragments_in += len(fragments) or 1
                endpoint.send(
                    types.WireMessage(
                        node_fragments=[
                            types.NodeFragment(
                                id="bench",
                                data=types.Chunk(data=b"y" * reply_bytes),
                            )
                            for _ in range(max(len(fragments), 1))
                        ]
                    )
                )
                _counters.fragments_out += max(len(fragments), 1)
        except Exception:  # noqa: BLE001 - a closed peer ends the loop
            _counters.closes += 1

    # -- the real thing ---------------------------------------------------

    async def start_service(self, transport: str, port: int) -> dict[str, Any]:
        """An `a11.Service` with the workload registry, on a real socket."""
        if self.service is None:
            self.service = a11.Service(action_registry=registry())

        service = self.service

        async def accept(stream) -> None:
            _counters.accepts += 1
            try:
                await service.accept(stream)
            except Exception:  # noqa: BLE001 - a client that leaves is normal
                _counters.closes += 1

        if transport == "websocket":
            options = net.WebSocketServerOptions()
            options.path = "/bench"
            options.bind_address = "0.0.0.0"
            options.port = port
            options.http2_options.enable_h2 = False
            options.http2_options.enable_h2c = False
            server = net.WebSocketWireServer.create(accept, options)
        elif transport == "sse":
            server = net.HttpSseServer.create(
                "0.0.0.0", port, accept, net.HttpSseOptions()
            )
        else:
            raise ValueError(f"unknown transport {transport!r}")
        self._service_servers.append(server)
        return {"port": server.port}

    # -- accounting -------------------------------------------------------

    def stats(self) -> dict[str, Any]:
        return {
            "monotonic": time.perf_counter(),
            "cpu_s": _cpu_seconds(),
            "rss_bytes": _rss_bytes(),
            "sessions": (
                self.service.session_count if self.service is not None else 0
            ),
            # Descriptors matter at population scale: 30k sessions is 30k of
            # these, and running out looks like four different transport errors
            # rather than like a limit (see FINDINGS.md item 0).
            "open_files": _open_file_count(),
            "counters": _counters.snapshot(),
            "interfaces": _interface_counters(),
            "sink_bytes": self._sink_bytes,
        }

    def reset_counters(self) -> dict[str, Any]:
        global _counters
        _counters = _Counters()
        self._sink_bytes = 0
        return {"ok": True}

    async def teardown(self) -> dict[str, Any]:
        """Drop every server and session, so the next row starts clean.

        Called between suites rather than between rows: a benchmark that wants
        a warm server must be able to have one, and a benchmark that wants a
        cold one asks for this explicitly.
        """
        for server in self._wire_servers + self._service_servers:
            with contextlib.suppress(Exception):
                server.stop()
        self._wire_servers.clear()
        self._service_servers.clear()
        for raw in self._raw_servers:
            with contextlib.suppress(Exception):
                raw.close()
        self._raw_servers.clear()
        for task in list(self._wire_tasks):
            task.cancel()
        with contextlib.suppress(Exception):
            await asyncio.gather(*self._wire_tasks, return_exceptions=True)
        self._wire_tasks.clear()
        self._held.clear()
        if self.service is not None:
            with contextlib.suppress(Exception):
                self.service.abort(
                    a11.Status(
                        code=a11.StatusCode.CANCELLED, message="bench teardown"
                    )
                )
            self.service = None
        self._sink_bytes = 0
        return {"ok": True}


async def _dispatch(agent: Agent, request: dict[str, Any]) -> dict[str, Any]:
    op = request.get("op")
    if op == "hello":
        return {"environment": _environment(), "stats": agent.stats()}
    if op == "stats":
        return agent.stats()
    if op == "reset":
        return agent.reset_counters()
    if op == "tcp_echo":
        return await agent.tcp_echo(int(request.get("port", 0)))
    if op == "tcp_sink":
        return await agent.tcp_sink(int(request.get("port", 0)))
    if op == "sink_bytes":
        return agent.sink_bytes()
    if op == "wire_echo":
        return await agent.wire_echo(
            str(request["transport"]), int(request.get("port", 0))
        )
    if op == "service":
        return await agent.start_service(
            str(request["transport"]), int(request.get("port", 0))
        )
    if op == "teardown":
        return await agent.teardown()
    if op == "shutdown":
        await agent.teardown()
        return {"ok": True, "shutdown": True}
    raise ValueError(f"unknown op {op!r}")


async def _serve_control(agent: Agent, reader, writer) -> bool:
    """One control connection. True when it asked the agent to shut down."""
    stop = False
    peer = writer.get_extra_info("peername")
    print(f"peer: control connection from {peer}", flush=True)
    try:
        while True:
            line = await reader.readline()
            if not line:
                break
            try:
                request = json.loads(line.decode(_ENCODING))
            except ValueError as bad:
                response = {"error": f"unparseable request: {bad}"}
            else:
                try:
                    response = await _dispatch(agent, request)
                except Exception as failure:  # noqa: BLE001 - report, not die
                    import traceback

                    traceback.print_exc()
                    response = {"error": repr(failure)}
                stop = stop or bool(response.get("shutdown"))
                response = {"op": request.get("op"), **response}
            writer.write(json.dumps(response).encode(_ENCODING) + b"\n")
            await writer.drain()
            if stop:
                break
    except (ConnectionError, asyncio.IncompleteReadError):
        pass
    finally:
        with contextlib.suppress(Exception):
            writer.close()
    print(f"peer: control connection {peer} closed", flush=True)
    return stop


async def _main(bind: str, port: int) -> int:
    agent = Agent()
    finished = asyncio.get_running_loop().create_future()

    async def handle(reader, writer) -> None:
        if await _serve_control(agent, reader, writer) and not finished.done():
            finished.set_result(None)

    server = await asyncio.start_server(handle, bind, port)
    address = server.sockets[0].getsockname()
    print(
        f"peer: listening on {address[0]}:{address[1]} --"
        f" {json.dumps(_environment())}",
        flush=True,
    )
    async with server:
        with contextlib.suppress(asyncio.CancelledError):
            await finished
    await agent.teardown()
    print("peer: done", flush=True)
    return 0


# --------------------------------------------------------------------------
# The client half
# --------------------------------------------------------------------------


class PeerClient:
    """A control connection to a `bench.peer` agent on the other host.

    Every call is a request and a reply on a channel nothing else uses. The
    contract the suites rely on: **no method here is called while a clock is
    running**, so the control channel's own latency never lands in a
    measurement.
    """

    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = port
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self.environment: dict[str, Any] = {}

    @classmethod
    def from_environment(cls) -> PeerClient | None:
        """`A11_BENCH_PEER=host:port`, or None when there is no peer."""
        target = os.environ.get("A11_BENCH_PEER", "").strip()
        if not target:
            return None
        host, _, port = target.rpartition(":")
        if not host:
            host, port = target, "8899"
        return cls(host, int(port))

    async def connect(self, timeout: float = 10.0) -> None:
        self._reader, self._writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), timeout=timeout
        )
        hello = await self.call("hello")
        self.environment = hello.get("environment", {})

    async def call(
        self, op: str, timeout: float = 60.0, **fields: Any
    ) -> dict[str, Any]:
        if self._writer is None or self._reader is None:
            raise RuntimeError("peer client is not connected")
        payload = json.dumps({"op": op, **fields}).encode(_ENCODING) + b"\n"
        self._writer.write(payload)
        await self._writer.drain()
        line = await asyncio.wait_for(self._reader.readline(), timeout=timeout)
        if not line:
            raise ConnectionError(f"peer closed while answering {op!r}")
        response = json.loads(line.decode(_ENCODING))
        if "error" in response:
            raise RuntimeError(f"peer {op!r} failed: {response['error']}")
        return response

    async def aclose(self) -> None:
        if self._writer is not None:
            with contextlib.suppress(Exception):
                self._writer.close()
        self._reader = self._writer = None

    # -- convenience -------------------------------------------------------

    async def stats(self) -> dict[str, Any]:
        return await self.call("stats")

    async def reset(self) -> None:
        await self.call("reset")

    async def teardown(self) -> None:
        await self.call("teardown", timeout=120.0)


class ServerCost:
    """Server-side CPU and memory across one measured window.

    Built from two `stats` samples taken outside the clock. The two derived
    figures are the ones that survive being moved between machines:

    * `cpu_us_per_op` -- CPU microseconds the *server* burned per delivered
      operation. Independent of how many cores it has, so it is the only
      efficiency number comparable across hosts.
    * `server_cores_busy` -- CPU seconds per wall second, which says how much
      of the host the load actually used. A row at 2.1 cores busy on a 12-core
      host is not near a ceiling however large its ops/s looks.
    """

    def __init__(self, before: dict[str, Any], after: dict[str, Any]) -> None:
        self.before = before
        self.after = after

    def metrics(
        self, operations: float, cores: int | None = None
    ) -> dict[str, float]:
        wall = max(
            self.after.get("monotonic", 0.0)
            - self.before.get("monotonic", 0.0),
            1e-9,
        )
        cpu = max(
            self.after.get("cpu_s", 0.0) - self.before.get("cpu_s", 0.0), 0.0
        )
        found = {
            "server_cpu_s": cpu,
            "server_cores_busy": cpu / wall,
            "server_rss_bytes": float(self.after.get("rss_bytes", 0)),
        }
        if operations > 0:
            found["server_cpu_us_per_op"] = cpu * 1e6 / operations
        if cores:
            found["server_core_utilisation"] = cpu / wall / cores
        return found

    def server_operations(self, key: str = "actions") -> int:
        return int(self.after.get("counters", {}).get(key, 0)) - int(
            self.before.get("counters", {}).get(key, 0)
        )

    def interface_bytes(self) -> dict[str, int]:
        """rx/tx byte deltas summed over every non-loopback interface."""
        before = self.before.get("interfaces") or {}
        after = self.after.get("interfaces") or {}
        totals = {
            "rx_bytes": 0,
            "tx_bytes": 0,
            "rx_packets": 0,
            "tx_packets": 0,
        }
        for name, counters in after.items():
            start = before.get(name)
            if not start:
                continue
            for field in totals:
                totals[field] += int(counters[field]) - int(start[field])
        return totals


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m bench.peer")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8899)
    parser.add_argument(
        "--loop",
        choices=("asyncio", "uvloop"),
        default="asyncio",
        help="the loop the *server* runs on, recorded with every result",
    )
    options = parser.parse_args(argv)
    runner = asyncio.run
    if options.loop == "uvloop":
        import uvloop

        runner = uvloop.run
    return runner(_main(options.bind, options.port))


if __name__ == "__main__":
    raise SystemExit(main())
