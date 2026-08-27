"""What the server's descriptors actually are while clients churn.

`FINDINGS.md` item 0 established two things by *counting*: a client's
`half_close()` retains 1.04 descriptors per connection, and a server under
connection churn grows by about 128 descriptors per second and releases them all
at once when the client process exits. Counting is where that investigation
stopped, and a count cannot distinguish the three explanations it is consistent
with:

* the server never calls `close()` on a socket whose peer has gone -- which
  shows up as **CLOSE_WAIT**, and is a server bug;
* the server still believes the connection is live -- **ESTABLISHED**, which
  means the client's abort did not reach it, and is a client or protocol bug;
* the descriptors are not sockets at all (eventfd, timerfd, pipe, epoll), which
  means something per-connection in the runtime is not being torn down and the
  socket is a red herring.

So this samples `/proc/<pid>/fd` *by kind*, joins the socket inodes against
`/proc/net/tcp` for their state, and reports the service's own `session_count`
beside them. Those three series together name the bug; any one of them alone
does not.

Linux only: descriptor classification uses `/proc`, and the observed leak is
Linux-specific.

    # both halves on one host, which is enough -- the leak is not about the link
    python -m bench.fdprobe --serve --port 8811          # the server
    python -m bench.fdprobe --churn 400 --port 8811      # the client

    # or one process that does both and prints the table
    python -m bench.fdprobe --port 8811
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import os
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

import a11
from a11 import net
from bench.peer import ECHO, registry

_CONNECT_TIMEOUT = 10.0
_DRAIN_TIMEOUT = 5.0


# --------------------------------------------------------------------------
# Reading the descriptor table
# --------------------------------------------------------------------------


def _socket_states() -> dict[int, str]:
    """inode -> TCP state name, from /proc/net/tcp{,6}.

    The state column is hex and the table is the kernel's own, so this is the
    only reading that can tell CLOSE_WAIT from ESTABLISHED without guessing.
    """
    names = {
        "01": "ESTABLISHED",
        "02": "SYN_SENT",
        "03": "SYN_RECV",
        "04": "FIN_WAIT1",
        "05": "FIN_WAIT2",
        "06": "TIME_WAIT",
        "07": "CLOSE",
        "08": "CLOSE_WAIT",
        "09": "LAST_ACK",
        "0A": "LISTEN",
        "0B": "CLOSING",
    }
    states: dict[int, str] = {}
    for table in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            lines = Path(table).read_text().splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            fields = line.split()
            if len(fields) < 10:
                continue
            with contextlib.suppress(ValueError):
                states[int(fields[9])] = names.get(fields[3].upper(), fields[3])
    return states


def classify(pid: int) -> Counter[str]:
    """One descriptor table, bucketed by kind and (for sockets) TCP state."""
    counts: Counter[str] = Counter()
    directory = Path(f"/proc/{pid}/fd")
    states = _socket_states()
    try:
        entries = list(directory.iterdir())
    except OSError:
        return counts
    for entry in entries:
        try:
            target = os.readlink(entry)
        except OSError:
            continue
        counts["total"] += 1
        if target.startswith("socket:["):
            inode = int(target[8:-1])
            counts["socket"] += 1
            counts[f"tcp:{states.get(inode, 'not-tcp')}"] += 1
        elif target.startswith("pipe:["):
            counts["pipe"] += 1
        elif target.startswith("anon_inode:"):
            counts[f"anon:{target[len('anon_inode:') :]}"] += 1
        else:
            counts["file"] += 1
    return counts


def render(samples: list[dict]) -> str:
    """Render the probe's three series side by side."""
    keys: list[str] = []
    for sample in samples:
        for key in sample["fds"]:
            if key != "total" and key not in keys:
                keys.append(key)
    keys.sort()
    header = ["t s", "cycles", "sessions", "srv", "cli"] + [
        k.replace("tcp:", "").replace("anon:", "") for k in keys
    ]
    widths = [max(6, len(h)) for h in header]
    rows = [header]
    for sample in samples:
        row = [
            f"{sample['t']:.1f}",
            str(sample["cycles"]),
            str(sample["sessions"]),
            str(sample["fds"].get("total", 0)),
            str(sample.get("client_fds", 0)),
        ] + [str(sample["fds"].get(k, 0)) for k in keys]
        rows.append(row)
    for row in rows:
        for index, cell in enumerate(row):
            widths[index] = max(widths[index], len(cell))
    return "\n".join(
        "  ".join(cell.rjust(widths[index]) for index, cell in enumerate(row))
        for row in rows
    )


# --------------------------------------------------------------------------
# The server half
# --------------------------------------------------------------------------


async def serve(port: int) -> None:
    """A real `a11.Service` on a real listener, reporting its own state.

    Writes one JSON line per second to stdout so the client can read the
    session count without a control channel of its own -- the count has to come
    from inside the process that owns the sessions, and inferring it from the
    outside is exactly the mistake this probe exists to avoid.
    """
    service = a11.Service(action_registry=registry())

    async def accept(stream) -> None:
        with contextlib.suppress(Exception):
            await service.accept(stream)

    # Report this beside the session count. A session with no streams is reaped
    # only after `no_stream_timeout`, so samples inside that window do not show
    # whether the count will fall.
    no_stream_timeout = a11.SessionOptions().no_stream_timeout.float_seconds()

    options = net.WebSocketServerOptions()
    options.path = "/probe"
    options.bind_address = "127.0.0.1"
    options.port = port
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False
    server = net.WebSocketWireServer.create(accept, options)
    print(
        json.dumps({
            "ready": True,
            "pid": os.getpid(),
            "port": server.port,
            "no_stream_timeout_s": no_stream_timeout,
        }),
        flush=True,
    )
    try:
        while True:
            await asyncio.sleep(0.25)
            print(
                json.dumps({"sessions": service.session_count}),
                flush=True,
            )
    finally:
        with contextlib.suppress(Exception):
            server.stop()


# --------------------------------------------------------------------------
# The client half
# --------------------------------------------------------------------------


async def one_cycle(port: int, teardown: str) -> None:
    """Connect, dispatch one action, disconnect the way `teardown` says.

    The three teardowns are the three things an application can currently do,
    and item 0's measurement is that they are not equivalent.
    """
    options = net.WebSocketClientOptions()
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False
    stream = net.WebSocketWireStream.connect(
        f"ws://127.0.0.1:{port}/probe", websocket_options=options
    )
    session = a11.Session(action_registry=a11.ActionRegistry())
    await asyncio.wait_for(
        session.add_stream(stream, mode="start"), _CONNECT_TIMEOUT
    )
    try:
        # One real action, so the cycle exercises a session that carried work
        # rather than one that only handshook.
        call = (
            a11
            .Action(ECHO)
            .bind_node_map(session.node_map)
            .bind_session(session)
            .bind_stream(stream)
        )
        await call.call()
        await call["text"].finalize("x")
        await call["out"].consume(str)
        await asyncio.wait_for(call.wait(), _DRAIN_TIMEOUT)
    except Exception:  # noqa: BLE001 - the teardown is what is under test
        pass
    if teardown in ("half_close", "both"):
        with contextlib.suppress(Exception):
            stream.half_close()
            await asyncio.wait_for(
                stream.drain_outgoing_messages(), _DRAIN_TIMEOUT
            )
    if teardown in ("abort", "both"):
        with contextlib.suppress(Exception):
            stream.abort(
                a11.Status(code=a11.StatusCode.CANCELLED, message="probe left")
            )
    if teardown == "drop":
        del stream
        del session


async def churn(
    port: int, cycles: int, concurrency: int, teardown: str
) -> None:
    done = 0
    semaphore = asyncio.Semaphore(concurrency)

    async def one() -> None:
        nonlocal done
        async with semaphore:
            with contextlib.suppress(Exception):
                await one_cycle(port, teardown)
            done += 1

    await asyncio.gather(*(one() for _ in range(cycles)))
    print(f"completed {done} of {cycles} cycles", file=sys.stderr)


# --------------------------------------------------------------------------
# Both halves, with the sampling in between
# --------------------------------------------------------------------------


async def drive(args: argparse.Namespace) -> int:
    """Spawns the server as a child, churns against it, samples its table."""
    child = subprocess.Popen(
        [
            sys.executable,
            "-m",
            "bench.fdprobe",
            "--serve",
            "--port",
            str(args.port),
        ],
        stdout=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    assert child.stdout is not None
    ready = json.loads(child.stdout.readline())
    pid, port = ready["pid"], ready["port"]
    reap_after = float(ready.get("no_stream_timeout_s", 30.0))
    # Sample past the longest timeout in the path, not past a round number. A
    # session with no streams is reaped `no_stream_timeout` after its last one
    # left, so a settle shorter than that reports a session count still inside
    # its grace period, indistinguishable from one that will never fall. Reap
    # times are staggered across the churn window, so allow twice the timeout
    # for sessions created near the final cycle.
    if args.settle < reap_after * 2.0:
        args.settle = reap_after * 2.0
        print(
            f"settle raised to {args.settle:.0f}s for a "
            f"{reap_after:.0f}s no_stream_timeout",
            file=sys.stderr,
        )

    sessions = 0
    cycles = 0
    samples: list[dict] = []
    start = time.perf_counter()

    def read_sessions() -> None:
        """Drains whatever the server has reported without blocking on it."""
        nonlocal sessions
        import select

        while select.select([child.stdout], [], [], 0)[0]:
            line = child.stdout.readline()
            if not line:
                return
            with contextlib.suppress(Exception):
                payload = json.loads(line)
                sessions = payload.get("sessions", sessions)

    async def sampler(stop: asyncio.Event) -> None:
        while not stop.is_set():
            read_sessions()
            samples.append({
                "t": time.perf_counter() - start,
                "cycles": cycles,
                "sessions": sessions,
                "fds": dict(classify(pid)),
                "client_fds": classify(os.getpid())["total"],
            })
            with contextlib.suppress(asyncio.TimeoutError):
                await asyncio.wait_for(stop.wait(), args.interval)

    async def worker() -> None:
        nonlocal cycles
        semaphore = asyncio.Semaphore(args.concurrency)

        async def one() -> None:
            nonlocal cycles
            async with semaphore:
                with contextlib.suppress(Exception):
                    await one_cycle(port, args.teardown)
                cycles += 1

        await asyncio.gather(*(one() for _ in range(args.churn)))

    stop = asyncio.Event()
    sampling = asyncio.create_task(sampler(stop))
    try:
        await worker()
        # The settle window is the discriminator between "teardown lags" and
        # "teardown never happens", and item 0 reports the difference as the
        # former without having measured a long enough one.
        settle = time.perf_counter()
        while time.perf_counter() - settle < args.settle:
            await asyncio.sleep(args.interval)
    finally:
        stop.set()
        await sampling
        read_sessions()
        samples.append({
            "t": time.perf_counter() - start,
            "cycles": cycles,
            "sessions": sessions,
            "fds": dict(classify(pid)),
            "client_fds": classify(os.getpid())["total"],
        })
        child.terminate()
        with contextlib.suppress(Exception):
            child.wait(timeout=5)

    print(
        f"\nteardown={args.teardown} cycles={args.churn} "
        f"concurrency={args.concurrency}"
    )
    print(render(samples))
    first, last = samples[0], samples[-1]
    grew = last["fds"].get("total", 0) - first["fds"].get("total", 0)
    client_grew = last.get("client_fds", 0) - first.get("client_fds", 0)
    print(
        f"\nserver fds {first['fds'].get('total', 0)} -> "
        f"{last['fds'].get('total', 0)} ({grew / max(cycles, 1):+.3f}/cycle); "
        f"client fds {first.get('client_fds', 0)} -> "
        f"{last.get('client_fds', 0)} "
        f"({client_grew / max(cycles, 1):+.3f}/cycle); "
        f"over {cycles} cycles; sessions ended at {last['sessions']} "
        f"(reaped {reap_after:.0f}s after their last stream; "
        f"settled {args.settle:.0f}s)"
    )
    if args.json:
        Path(args.json).write_text(json.dumps(samples, indent=2))
        print(f"wrote {args.json}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--serve", action="store_true", help="be the server half"
    )
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument(
        "--churn", type=int, default=300, help="connection cycles"
    )
    parser.add_argument("--concurrency", type=int, default=8)
    parser.add_argument(
        "--teardown",
        default="both",
        choices=["half_close", "abort", "both", "drop"],
        help="how the client finishes with its connection",
    )
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument(
        "--settle",
        type=float,
        default=0.0,
        help=(
            "seconds to keep sampling after the last cycle; raised "
            "automatically to 2x the server's no_stream_timeout, because a "
            "shorter window cannot tell a session inside its grace period from "
            "one that will never be reaped"
        ),
    )
    parser.add_argument("--json", default="")
    args = parser.parse_args(argv)

    if sys.platform != "linux" and not args.serve:
        print("fdprobe classifies /proc/<pid>/fd; Linux only", file=sys.stderr)
        return 2
    if args.serve:
        with contextlib.suppress(KeyboardInterrupt):
            asyncio.run(serve(args.port))
        return 0
    return asyncio.run(drive(args))


if __name__ == "__main__":
    raise SystemExit(main())
