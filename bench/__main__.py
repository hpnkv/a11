# Copyright 2026 The A11 Authors.

"""`python -m bench` -- run the suites, print the table, keep the record.

    python -m bench --list
    python -m bench --suite stores --suite wire
    python -m bench -k websocket --json runs/today.json
    python -m bench --json runs/today.json --baseline runs/last-week.json

`--scale` multiplies every iteration count. Use it below 1 for a smoke run that
proves the suite still works, and above 1 when a number looks noisy.

`--loop uvloop` runs everything on uvloop instead of the selector loop. That is
not a detail: A11's sequential async throughput is bounded by the cost of one
event-loop turn, and the loop implementation is the biggest single lever on it
that requires no change to A11 at all. Compare the two runs before optimising
anything that is really just paying for turns.

**Each suite runs in its own process by default.** These benchmarks open
servers, hold hundreds of live streams, start shells and drive native pumps,
and a suite that leaves any of that behind changes the numbers the next suite
reports -- or wedges it. One process per suite makes each suite's result depend
only on that suite, and turns a hang into one lost suite instead of one lost
run. `--in-process` puts them all back in one interpreter, which is what you
want when profiling or debugging a single suite.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import subprocess
import sys
import tempfile

from bench import harness, report, suites


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="python -m bench")
    parser.add_argument(
        "--suite", action="append", default=[], help="run only this suite"
    )
    parser.add_argument(
        "-k",
        "--pattern",
        action="append",
        default=[],
        help="substring match on suite or benchmark name",
    )
    parser.add_argument("--list", action="store_true", help="list and exit")
    parser.add_argument(
        "--slow", action="store_true", help="include the long capacity ramps"
    )
    parser.add_argument(
        "--scale",
        type=float,
        default=1.0,
        help="multiply every iteration count",
    )
    parser.add_argument("--json", help="write results here")
    parser.add_argument("--baseline", help="compare against this results file")
    parser.add_argument(
        "--in-process",
        action="store_true",
        help="run every suite in this interpreter instead of one each",
    )
    parser.add_argument(
        "--loop",
        choices=("asyncio", "uvloop"),
        default="asyncio",
        help="event loop to run the async benchmarks on",
    )
    parser.add_argument(
        "--suite-timeout",
        type=float,
        default=1800.0,
        help="seconds before a whole suite's subprocess is killed",
    )
    parser.add_argument(
        "--benchmark-timeout",
        type=float,
        default=600.0,
        help="seconds before one benchmark is abandoned as hung",
    )
    return parser


def _install_loop(name: str):
    """Return the `asyncio.run` to use, having selected the loop.

    Returns a callable rather than setting a policy, because a policy is a
    process-global and the isolated runner starts one interpreter per suite --
    a callable keeps the choice where it is used and visible in the result.
    """
    harness.event_loop_name = name
    if name != "uvloop":
        return asyncio.run
    try:
        import uvloop
    except ImportError:
        # A requested loop must not silently measure another implementation.
        # uvloop is optional and `uv sync` may remove it.
        raise SystemExit(
            "--loop uvloop was requested but uvloop is not importable.\n"
            "Install it (`uv pip install uvloop`) or run with --loop asyncio.\n"
            "Note that `uv sync` removes it again: it is not a project"
            " dependency."
        )
    return uvloop.run


#: Above this, `latency` rows carry more measurement overhead than a laptop's
#: entire event-loop turn and stop being comparable with another machine's.
_SLOW_CLOCK_NS = 200.0


def _warn_about_the_clock(environment: dict) -> None:
    """Say it out loud when the clock is too expensive to measure latency with.

    A field in the JSON is not enough. A run on a VM whose only clocksource is
    `acpi_pm` reads the clock in ~3us, which puts ~3us inside every `latency`
    sample and made a bare Python coroutine look 21x slower than on a laptop of
    equivalent integer throughput. That is the kind of number somebody quotes in
    a document, so it gets a banner rather than a field.
    """
    cost = environment.get("clock_call_ns") or 0.0
    if cost < _SLOW_CLOCK_NS:
        return
    source = environment.get("clocksource") or "unknown"
    print(
        f"\n  !! perf_counter_ns() costs {cost:.0f}ns here"
        f" (clocksource: {source}).\n"
        "     About that much lands inside every sample `latency` takes, so"
        " every p50/p99 below\n"
        "     is inflated by roughly that and is NOT comparable with a machine"
        " whose clock is\n"
        "     cheap. `throughput`, `throughput_sync` and `pipelined` rows read"
        " the clock twice per\n"
        "     batch and are unaffected -- prefer those. On a VM this usually"
        " means the hypervisor\n"
        "     is not exposing a usable TSC; there is no fix from inside the"
        " guest.\n",
        flush=True,
    )


def _run_isolated(options, selected) -> list[harness.Result]:
    """One subprocess per suite, results merged.

    A suite that hangs or dies takes its own process with it and nothing else;
    the run continues and says which suite was lost.
    """
    wanted: list[str] = []
    for entry in selected:
        if entry.suite not in wanted:
            wanted.append(entry.suite)

    merged: list[harness.Result] = []
    for suite in wanted:
        print(f"\n=== {suite} ===", flush=True)
        handle, path = tempfile.mkstemp(
            prefix=f"a11-bench-{suite}-", suffix=".json"
        )
        os.close(handle)
        command = [
            sys.executable,
            "-m",
            "bench",
            "--in-process",
            "--suite",
            suite,
            "--scale",
            str(options.scale),
            "--json",
            path,
            "--benchmark-timeout",
            str(options.benchmark_timeout),
            "--loop",
            options.loop,
        ]
        if options.slow:
            command.append("--slow")
        for pattern in options.pattern:
            command += ["-k", pattern]
        # Isolate timeout signals to the child. Disable its crash handler so a
        # deliberate timeout is not reported as a native crash.
        child_environment = {
            **os.environ,
            "A11_DISABLE_FAILURE_SIGNAL_HANDLER": "1",
        }
        try:
            subprocess.run(
                command,
                timeout=options.suite_timeout,
                check=False,
                start_new_session=True,
                env=child_environment,
            )
        except subprocess.TimeoutExpired:
            print(
                f"  SUITE TIMEOUT {suite}: killed after"
                f" {options.suite_timeout:.0f}s",
                flush=True,
            )
        try:
            with open(path) as loaded:
                payload = json.load(loaded)
            merged.extend(
                harness.Result(**record) for record in payload["results"]
            )
        except (OSError, ValueError, KeyError):
            # Recover completed measurements from a killed or crashed child's
            # partial log when it could not write the final payload.
            recovered = harness.read_partial(harness.partial_path(path))
            if recovered:
                merged.extend(recovered)
                print(
                    f"  PARTIAL {suite}: recovered {len(recovered)} result(s)"
                    " from the killed run",
                    flush=True,
                )
            else:
                print(f"  LOST {suite}: no results written", flush=True)
        finally:
            with open(os.devnull):
                pass
            for leftover in (path, harness.partial_path(path)):
                try:
                    os.unlink(leftover)
                except OSError:
                    pass
    return merged


def main(argv: list[str] | None = None) -> int:
    options = _parser().parse_args(argv)
    suites.load_all()

    selected = harness.registry.select(options.suite, options.pattern)
    if not options.slow:
        selected = [entry for entry in selected if not entry.slow]

    if options.list:
        for entry in selected:
            marks = " (slow)" if entry.slow else ""
            marks += f" (needs {entry.needs})" if entry.needs else ""
            print(f"{entry.suite:12} {entry.name:38} {entry.doc}{marks}")
        return 0

    if not selected:
        print("nothing selected", file=sys.stderr)
        return 1

    runner = _install_loop(options.loop) if options.in_process else None
    # The isolated runner selects the loop in each suite subprocess, so the
    # parent records the requested name itself.
    harness.event_loop_name = options.loop
    print(f"a11 bench: {len(selected)} benchmark(s), scale={options.scale}")
    environment = harness.environment()
    for key, value in environment.items():
        print(f"  {key}: {value}")
    _warn_about_the_clock(environment)

    if options.in_process:

        def _record(result: harness.Result) -> None:
            print(f"  ok   {result.key}", flush=True)
            # Streamed as it finishes, so a suite killed for exceeding its
            # timeout still reports what it measured.
            if options.json:
                harness.append_partial(
                    harness.partial_path(options.json), result
                )

        results = runner(
            harness.run_selected(
                selected,
                on_result=_record,
                scale=options.scale,
                timeout_s=options.benchmark_timeout,
            )
        )
    else:
        results = _run_isolated(options, selected)

    print(report.table(results))

    if options.json:
        harness.write_json(options.json, results)
        print(f"\nwrote {options.json}")

    if options.baseline:
        print(f"\nagainst {options.baseline}:")
        print(report.compare(results, harness.read_json(options.baseline)))

    return 0


def _relaunch_argv() -> list[str]:
    """Arguments that re-run this command under a fresh interpreter.

    `python -m bench` has to be reconstructed as `-m`, or the relaunched
    process gets a different `sys.path[0]` than the one it was started with.
    """
    if __package__ and sys.argv and sys.argv[0].endswith("__main__.py"):
        return ["-m", __package__, *sys.argv[1:]]
    return list(sys.argv)


if __name__ == "__main__":
    # The allocator is selected by launching the process with its library
    # preloaded. Per-suite subprocesses inherit the preload setting.
    from a11 import allocator

    allocator.reexec_with_preload(_relaunch_argv())
    raise SystemExit(main())
