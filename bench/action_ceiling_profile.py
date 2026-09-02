# Copyright 2026 The A11 Authors.

"""Hold one 64-client action window open for external profilers."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import resource

from bench.peer import PeerClient
from bench.suites.server import (
    Tally,
    _fleet,
    _run_window,
    _steady_driver,
)


def _cpu_seconds() -> float:
    usage = resource.getrusage(resource.RUSAGE_SELF)
    return usage.ru_utime + usage.ru_stime


async def _run(args: argparse.Namespace) -> int:
    peer = PeerClient(args.host, args.port)
    await peer.connect()
    try:
        async with _fleet(peer, "websocket") as fleet:
            clients = [await fleet.connect() for _index in range(64)]
            await asyncio.gather(*(client.echo() for client in clients))
            print(
                json.dumps(
                    {
                        "ready": True,
                        "client_pid": os.getpid(),
                        "peer_pid": peer.environment["pid"],
                    }
                ),
                flush=True,
            )
            await asyncio.sleep(args.delay)
            await peer.call("attribution_begin")
            before_cpu = _cpu_seconds()
            tally = Tally("profile")
            elapsed, cost, _ = await _run_window(
                peer,
                [(tally, _steady_driver(clients))],
                args.duration,
            )
            handler = (await peer.call("attribution_end"))["metrics"]
            completed = len(tally.samples)
            result = {
                "environment": peer.environment,
                "elapsed_s": elapsed,
                "completed": completed,
                "failures": tally.failures,
                "ops_per_s": completed / elapsed,
                "client_cores_busy": (_cpu_seconds() - before_cpu) / elapsed,
                **cost.metrics(max(completed, 1)),
                "server_completed": cost.server_operations(),
                "handler": handler,
            }
            with open(args.output, "w") as handle:
                json.dump(result, handle, indent=2)
                handle.write("\n")
    finally:
        await peer.aclose()
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m bench.action_ceiling_profile"
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8899)
    parser.add_argument("--duration", type=float, default=55.0)
    parser.add_argument("--delay", type=float, default=5.0)
    parser.add_argument("--output", required=True)
    parser.add_argument(
        "--loop", choices=("asyncio", "uvloop"), default="asyncio"
    )
    args = parser.parse_args(argv)
    runner = asyncio.run
    if args.loop == "uvloop":
        import uvloop

        runner = uvloop.run
    return runner(_run(args))


if __name__ == "__main__":
    raise SystemExit(main())
