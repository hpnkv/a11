# Copyright 2026 The A11 Authors.

"""Drive one action-ceiling client shard behind a coordinator barrier."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import resource
import sys
import time
from pathlib import Path

from bench.peer import PeerClient
from bench.suites.server import Client, Fleet, Tally, _LoopLag, _drive


def _cpu_seconds() -> float:
    usage = resource.getrusage(resource.RUSAGE_SELF)
    return usage.ru_utime + usage.ru_stime


async def _run(args: argparse.Namespace) -> int:
    peer = PeerClient(args.host, 0)
    fleet = Fleet(peer, "websocket", args.port)
    clients = [await fleet.connect() for _index in range(args.sessions)]
    await asyncio.gather(*(client.echo() for client in clients))
    print(json.dumps({"ready": True, "pid": os.getpid()}), flush=True)
    record_path = Path(args.record)
    record_path.parent.mkdir(parents=True, exist_ok=True)
    records: list[dict] = []
    try:
        while line := await asyncio.to_thread(sys.stdin.readline):
            command = json.loads(line)
            if command.get("op") == "stop":
                break
            start_at = float(command["start_at"])
            seconds = float(command["seconds"])
            delay = start_at - time.time()
            if delay > 0:
                await asyncio.sleep(delay)
            tally = Tally("client-shard")
            lag = _LoopLag()
            lag.start()
            before_cpu = _cpu_seconds()
            started = time.perf_counter()
            deadline = started + seconds
            stop = asyncio.Event()

            async def one(client: Client) -> None:
                await _drive(client.echo, tally, deadline, stop)

            await asyncio.gather(*(one(client) for client in clients))
            elapsed = time.perf_counter() - started
            cpu = max(_cpu_seconds() - before_cpu, 0.0)
            await lag.stop()
            record = {
                "pid": os.getpid(),
                "completed": len(tally.samples),
                "failures": tally.failures,
                "latencies_ns": tally.samples,
                "cpu_s": cpu,
                "elapsed_s": elapsed,
                **lag.metrics(elapsed),
            }
            records.append(record)
            record_path.write_text(json.dumps(records, indent=2) + "\n")
            print(json.dumps(record), flush=True)
    finally:
        await fleet.aclose()
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m bench.action_ceiling_worker"
    )
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--sessions", type=int, required=True)
    parser.add_argument("--record", required=True)
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
