# Copyright 2026 The A11 Authors.

"""The measuring apparatus: timing loops, percentiles, memory, and results.

Three things are deliberate here.

**Latency and throughput are measured separately.** A `perf_counter_ns` pair
around an operation that takes 300ns is measuring the clock as much as the
operation. So `latency` runs a modest number of iterations and times each one,
while `throughput` times a whole batch and divides. A benchmark that wants both
runs both, and says so.

**Memory is reported as a marginal cost per object.** Peak RSS of a process is
nearly useless for capacity planning; "42 KiB per idle session" is exactly what
a person sizing a host needs. `memory_slope` builds the population in stages
and fits a line through (count, RSS), which discards both the pages the
allocator had already reserved and the fixed costs the first object would
otherwise be charged for. `MemoryProbe` is the cruder single-delta version, and
is only right when the population is large enough to swamp both.

**A result is a record, not a line of text.** Printing is one consumer; the JSON
file a later run diffs against is the other, and it is the one that makes a
regression visible.
"""

from __future__ import annotations

import asyncio
import ctypes
import ctypes.util
import gc
import json
import math
import os
import platform
import statistics
import sys
import time
from collections.abc import Awaitable, Callable, Iterable, Sequence
from dataclasses import dataclass, field, asdict
from typing import Any

__all__ = [
    "Result",
    "MemoryProbe",
    "benchmark",
    "current_rss_bytes",
    "latency",
    "latency_sync",
    "memory_slope",
    "percentiles",
    "pipelined",
    "registry",
    "run_selected",
    "throughput",
    "throughput_sync",
]


# --------------------------------------------------------------------------
# Resident memory
# --------------------------------------------------------------------------


def _darwin_rss_reader() -> Callable[[], int] | None:
    """Resident size from libproc, which is what Activity Monitor shows.

    `resource.getrusage` only offers the *peak*, and a peak never comes back
    down -- useless for "what does one more session cost". `proc_pidinfo` with
    PROC_PIDTASKINFO gives the live number.
    """
    path = ctypes.util.find_library("proc")
    if path is None:
        return None
    try:
        libproc = ctypes.CDLL(path, use_errno=True)
    except OSError:
        return None

    class _TaskInfo(ctypes.Structure):
        _fields_ = [
            ("pti_virtual_size", ctypes.c_uint64),
            ("pti_resident_size", ctypes.c_uint64),
            ("pti_total_user", ctypes.c_uint64),
            ("pti_total_system", ctypes.c_uint64),
            ("pti_threads_user", ctypes.c_uint64),
            ("pti_threads_system", ctypes.c_uint64),
            ("pti_policy", ctypes.c_int32),
            ("pti_faults", ctypes.c_int32),
            ("pti_pageins", ctypes.c_int32),
            ("pti_cow_faults", ctypes.c_int32),
            ("pti_messages_sent", ctypes.c_int32),
            ("pti_messages_received", ctypes.c_int32),
            ("pti_syscalls_mach", ctypes.c_int32),
            ("pti_syscalls_unix", ctypes.c_int32),
            ("pti_csw", ctypes.c_int32),
            ("pti_threadnum", ctypes.c_int32),
            ("pti_numrunning", ctypes.c_int32),
            ("pti_priority", ctypes.c_int32),
        ]

    pid = os.getpid()
    size = ctypes.sizeof(_TaskInfo)
    proc_pidtaskinfo = 4

    def read() -> int:
        info = _TaskInfo()
        written = libproc.proc_pidinfo(
            pid, proc_pidtaskinfo, ctypes.c_uint64(0), ctypes.byref(info), size
        )
        if written != size:
            return 0
        return int(info.pti_resident_size)

    return read if read() else None


def _linux_rss_reader() -> Callable[[], int] | None:
    page = os.sysconf("SC_PAGE_SIZE")

    def read() -> int:
        try:
            with open("/proc/self/statm", "rb") as handle:
                return int(handle.read().split()[1]) * page
        except OSError:
            return 0

    return read if read() else None


def _fallback_rss_reader() -> Callable[[], int]:
    import resource

    scale = 1 if sys.platform == "darwin" else 1024

    def read() -> int:
        return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * scale

    return read


def _select_rss_reader() -> Callable[[], int]:
    for candidate in (_darwin_rss_reader, _linux_rss_reader):
        try:
            reader = candidate()
        except Exception:  # noqa: BLE001 - probing a platform API
            reader = None
        if reader is not None:
            return reader
    return _fallback_rss_reader()


_read_rss = _select_rss_reader()


def current_rss_bytes() -> int:
    """The process's resident set right now, in bytes."""
    return _read_rss()


class MemoryProbe:
    """Resident-memory cost of whatever is alive inside the block.

    Usage:

        with MemoryProbe(count=1000) as probe:
            things = [make_one() for _ in range(1000)]
        probe.bytes_each   # what one costs

    The allocator does not return pages promptly, so this is honest in one
    direction only: it measures *growth*. Take it as an upper bound per object
    and a lower bound on nothing.
    """

    def __init__(self, count: int = 1) -> None:
        self.count = max(count, 1)
        self.before = 0
        self.after = 0

    def __enter__(self) -> MemoryProbe:
        gc.collect()
        self.before = current_rss_bytes()
        return self

    def __exit__(self, *exc: object) -> None:
        gc.collect()
        self.after = current_rss_bytes()

    @property
    def delta(self) -> int:
        return max(self.after - self.before, 0)

    @property
    def bytes_each(self) -> float:
        return self.delta / self.count


async def memory_slope(
    make: Callable[[int], Any],
    *,
    counts: Sequence[int],
) -> tuple[float, str]:
    """Marginal resident bytes per object, from a fit rather than one delta.

    A single before/after reading is wrong in two directions at once: the
    allocator has already reserved pages the first objects fall into for free,
    and the process has fixed costs the first object gets charged for. Building
    the population in stages and fitting a line through (count, RSS) throws
    both away and leaves the *marginal* cost, which is the number that answers
    "how many more can this host hold".

    `make(n)` must produce and return `n` more objects; the caller's list of
    them is held until the fit is done.
    """
    gc.collect()
    held: list[Any] = []
    total = 0
    points: list[tuple[int, int]] = []
    for count in counts:
        produced = make(count)
        if isinstance(produced, Awaitable):
            produced = await produced
        held.append(produced)
        total += count
        gc.collect()
        points.append((total, current_rss_bytes()))

    # Least squares against the later points only: the first stage absorbs
    # whatever the allocator had already reserved.
    usable = points[1:] if len(points) > 2 else points
    n = len(usable)
    mean_x = statistics.fmean(x for x, _ in usable)
    mean_y = statistics.fmean(y for _, y in usable)
    covariance = sum((x - mean_x) * (y - mean_y) for x, y in usable)
    variance = sum((x - mean_x) ** 2 for x, _ in usable)
    slope = covariance / variance if variance else 0.0
    held.clear()
    trail = " ".join(f"{x}:{y // 1024}K" for x, y in points)
    return max(slope, 0.0), f"{n}-point fit over {trail}"


# --------------------------------------------------------------------------
# Statistics
# --------------------------------------------------------------------------


def clock_call_ns(iterations: int = 200_000) -> float:
    """What one `perf_counter_ns()` call costs on this machine.

    Recorded with every run, and it is not a curiosity. `latency` brackets each
    operation with two of these, so roughly one call's cost lands *inside* every
    sample it takes -- which is invisible at 42ns and ruinous at 3us.

    A Linux guest whose only available clocksource is `acpi_pm` (no usable TSC
    exposed by the hypervisor -- check
    `/sys/devices/system/clocksource/clocksource0/available_clocksource`) reads
    the clock through a VM exit rather than the vDSO, and it measured **3047ns
    per call** on exactly such a host: 72x a laptop's 42ns. Every `latency` row
    on that host is inflated by about 3us, which made a bare Python coroutine
    look 21x slower than on an Apple laptop whose integer and float throughput
    is within 3% of it. Nothing about A11 was different; the ruler was.

    `throughput`, `throughput_sync` and `pipelined` read the clock twice for a
    whole batch and are unaffected, so on a host like that they are the only
    comparable rows. There is no workaround from inside the guest -- a coarse
    clock has millisecond resolution and no other source is offered -- so the
    honest response is to record the number, warn, and read the throughput rows.
    """
    started = time.perf_counter_ns()
    for _ in range(iterations):
        time.perf_counter_ns()
    return (time.perf_counter_ns() - started) / iterations


def percentiles(samples_ns: Sequence[float]) -> dict[str, float]:
    """p50/p90/p99/max plus mean, in microseconds, from nanosecond samples."""
    if not samples_ns:
        return {}
    ordered = sorted(samples_ns)

    def at(fraction: float) -> float:
        index = min(
            len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1)
        )
        return ordered[index] / 1000.0

    stats = {
        "p50_us": at(0.50),
        "p90_us": at(0.90),
        "p99_us": at(0.99),
        "max_us": ordered[-1] / 1000.0,
        "mean_us": statistics.fmean(ordered) / 1000.0,
    }
    if len(ordered) > 1:
        stats["stdev_us"] = statistics.stdev(ordered) / 1000.0
    return stats


# --------------------------------------------------------------------------
# Results
# --------------------------------------------------------------------------


@dataclass
class Result:
    """One measured thing, in units a person can act on.

    `metrics` keys carry their unit as a suffix (`_us`, `_per_s`, `_bytes`,
    `_mib_per_s`) so a table or a diff never has to guess. `params` is what
    distinguishes two runs of the same benchmark -- payload size, backend name,
    stream count -- and is part of the identity used when comparing runs.
    """

    suite: str
    name: str
    metrics: dict[str, float] = field(default_factory=dict)
    params: dict[str, Any] = field(default_factory=dict)
    note: str = ""

    @property
    def key(self) -> str:
        if not self.params:
            return f"{self.suite}/{self.name}"
        rendered = ",".join(f"{k}={v}" for k, v in sorted(self.params.items()))
        return f"{self.suite}/{self.name}[{rendered}]"

    def to_json(self) -> dict[str, Any]:
        return asdict(self)


# --------------------------------------------------------------------------
# Timing loops
# --------------------------------------------------------------------------


async def _drain(value: Any) -> Any:
    if asyncio.iscoroutine(value) or isinstance(value, Awaitable):
        return await value
    return value


async def throughput(
    operation: Callable[[int], Any],
    *,
    iterations: int,
    warmup: int = 0,
    per_op_items: int = 1,
    per_op_bytes: int = 0,
) -> dict[str, float]:
    """Time `iterations` calls as one batch; report rate, not per-call latency.

    `per_op_items` is for an operation that moves more than one thing (a
    `put_many` of 64 fragments is one call and 64 items), and `per_op_bytes`
    turns the same run into a byte rate.
    """
    for index in range(warmup):
        await _drain(operation(index))
    gc.collect()
    started = time.perf_counter_ns()
    for index in range(iterations):
        await _drain(operation(warmup + index))
    elapsed_ns = time.perf_counter_ns() - started
    return _rate_metrics(elapsed_ns, iterations, per_op_items, per_op_bytes)


def throughput_sync(
    operation: Callable[[int], Any],
    *,
    iterations: int,
    warmup: int = 0,
    per_op_items: int = 1,
    per_op_bytes: int = 0,
) -> dict[str, float]:
    """`throughput` for work with no await -- codecs, parsers, builders."""
    for index in range(warmup):
        operation(index)
    gc.collect()
    started = time.perf_counter_ns()
    for index in range(iterations):
        operation(warmup + index)
    elapsed_ns = time.perf_counter_ns() - started
    return _rate_metrics(elapsed_ns, iterations, per_op_items, per_op_bytes)


def _rate_metrics(
    elapsed_ns: int, iterations: int, per_op_items: int, per_op_bytes: int
) -> dict[str, float]:
    seconds = elapsed_ns / 1e9
    if seconds <= 0:
        seconds = 1e-9
    metrics = {
        "ops_per_s": iterations / seconds,
        "ns_per_op": elapsed_ns / max(iterations, 1),
        "elapsed_s": seconds,
    }
    if per_op_items != 1:
        metrics["items_per_s"] = iterations * per_op_items / seconds
    if per_op_bytes:
        metrics["mib_per_s"] = (
            iterations * per_op_bytes / seconds / (1024 * 1024)
        )
    return metrics


async def pipelined(
    operation: Callable[[int], Any],
    *,
    iterations: int,
    window: int,
    warmup: int = 0,
    per_op_items: int = 1,
    per_op_bytes: int = 0,
) -> dict[str, float]:
    """Throughput with `window` operations in flight rather than one.

    Sequential `await` measurement charges every operation a full event-loop
    turn, and on a selector loop that turn is a syscall -- tens of
    microseconds, none of it A11's. That floor is real for a caller that awaits
    one thing at a time, but it hides what the component underneath can
    actually do. Running a window of operations concurrently amortises the turn
    across the window and measures the component instead.

    Report both. The sequential number is what a naive caller sees; this one is
    the ceiling, and the gap between them is how much a caller can win by
    pipelining.
    """
    for index in range(warmup):
        await _drain(operation(index))
    gc.collect()
    started = time.perf_counter_ns()
    issued = warmup
    remaining = iterations
    while remaining > 0:
        batch = min(window, remaining)
        if batch == 1:
            # `gather` of one still allocates a Task, which would make a
            # window of 1 look slower than plain sequential awaiting and
            # misstate the baseline the other windows are compared against.
            await _drain(operation(issued))
        else:
            await asyncio.gather(
                *(_drain(operation(issued + offset)) for offset in range(batch))
            )
        issued += batch
        remaining -= batch
    elapsed_ns = time.perf_counter_ns() - started
    metrics = _rate_metrics(elapsed_ns, iterations, per_op_items, per_op_bytes)
    metrics["window"] = float(window)
    return metrics


async def latency(
    operation: Callable[[int], Any],
    *,
    iterations: int,
    warmup: int = 0,
) -> dict[str, float]:
    """Time each call separately and report the distribution.

    Use for anything above roughly a microsecond -- a round trip, a dispatch, a
    store write. Below that the clock is part of what you are measuring; use
    `throughput`.
    """
    for index in range(warmup):
        await _drain(operation(index))
    gc.collect()
    samples: list[float] = []
    for index in range(iterations):
        started = time.perf_counter_ns()
        await _drain(operation(warmup + index))
        samples.append(time.perf_counter_ns() - started)
    stats = percentiles(samples)
    total = sum(samples) / 1e9
    stats["ops_per_s"] = iterations / total if total > 0 else 0.0
    return stats


def latency_sync(
    operation: Callable[[int], Any],
    *,
    iterations: int,
    warmup: int = 0,
) -> dict[str, float]:
    """`latency` for synchronous work."""
    for index in range(warmup):
        operation(index)
    gc.collect()
    samples: list[float] = []
    for index in range(iterations):
        started = time.perf_counter_ns()
        operation(warmup + index)
        samples.append(time.perf_counter_ns() - started)
    stats = percentiles(samples)
    total = sum(samples) / 1e9
    stats["ops_per_s"] = iterations / total if total > 0 else 0.0
    return stats


# --------------------------------------------------------------------------
# Registration
# --------------------------------------------------------------------------


@dataclass
class Benchmark:
    suite: str
    name: str
    run: Callable[..., Awaitable[Iterable[Result]]]
    doc: str
    slow: bool = False
    needs: str = ""


class _Registry:
    def __init__(self) -> None:
        self.benchmarks: list[Benchmark] = []

    def add(self, entry: Benchmark) -> None:
        self.benchmarks.append(entry)

    def suites(self) -> list[str]:
        seen: list[str] = []
        for entry in self.benchmarks:
            if entry.suite not in seen:
                seen.append(entry.suite)
        return seen

    def select(
        self, suites: Sequence[str] = (), patterns: Sequence[str] = ()
    ) -> list[Benchmark]:
        chosen = self.benchmarks
        if suites:
            wanted = set(suites)
            chosen = [entry for entry in chosen if entry.suite in wanted]
        if patterns:
            chosen = [
                entry
                for entry in chosen
                if any(
                    pattern in entry.name or pattern in entry.suite
                    for pattern in patterns
                )
            ]
        return chosen


registry = _Registry()


def benchmark(
    suite: str, name: str, *, slow: bool = False, needs: str = ""
) -> Callable:
    """Register a coroutine that yields (or returns) `Result` records.

    `slow` keeps it out of the default run -- it is for the capacity ramps that
    take minutes. `needs` names an external dependency (a Redis, an API key) so
    a skip can say *why*.
    """

    def decorate(function: Callable[..., Awaitable[Iterable[Result]]]):
        registry.add(
            Benchmark(
                suite=suite,
                name=name,
                run=function,
                doc=(function.__doc__ or "").strip().split("\n")[0],
                slow=slow,
                needs=needs,
            )
        )
        return function

    return decorate


class Skip(Exception):
    """Raised by a benchmark whose prerequisite is not here."""


# --------------------------------------------------------------------------
# Running
# --------------------------------------------------------------------------


#: Which event loop the async benchmarks ran on. Set by the runner before
#: anything async starts; recorded with every result because a number measured
#: on uvloop is not comparable with one measured on the selector loop.
event_loop_name = "asyncio"


def _source_revision() -> str:
    """The commit the tree is on, with a marker when it is not clean.

    Recorded because a baseline is only a baseline if the build that produced
    it can be rebuilt. A run of this suite was once compared against a file
    from the same morning whose extension no longer existed anywhere on disk,
    and the difference -- 14% on a core store path -- could not be attributed
    to anything, because there was no way to reconstruct what it had measured.
    A commit and a dirty flag are not a build id, but they are enough to know
    whether two files are even talking about the same code.
    """
    import subprocess

    try:
        revision = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True,
            text=True,
            timeout=5,
            check=True,
        ).stdout.strip()
        dirty = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            capture_output=True,
            text=True,
            timeout=5,
            check=True,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    return f"{revision}-dirty" if dirty else revision


def _native_extension_build() -> dict[str, Any]:
    """Which `_native` was loaded, and when it was built.

    The extension is what the numbers actually measure, and it is installed
    separately from the source tree -- an editable install can be running a
    build from days ago while the tree in front of you says otherwise.
    """
    try:
        from a11 import _native

        path = getattr(_native, "__file__", None)
        if path is None:
            return {"path": "unknown"}
        stat = os.stat(path)
        return {
            "path": path,
            "built": time.strftime(
                "%Y-%m-%dT%H:%M:%S", time.localtime(stat.st_mtime)
            ),
            "bytes": stat.st_size,
        }
    except Exception:  # noqa: BLE001 - metadata must never fail a run
        return {"path": "unknown"}


def _clocksource() -> str:
    """The kernel's chosen clocksource, on a platform that names one.

    `tsc` is the fast one. `acpi_pm`, `hpet` or `xen` mean the clock is read
    through something slower than the vDSO, and `clock_call_ns` will say how
    much slower.
    """
    try:
        with open(
            "/sys/devices/system/clocksource/clocksource0/current_clocksource"
        ) as handle:
            return handle.read().strip()
    except OSError:
        return ""


def environment() -> dict[str, Any]:
    """What a number means only in the context of."""
    import a11

    return {
        "event_loop": event_loop_name,
        "python": sys.version.split()[0],
        "platform": platform.platform(),
        "machine": platform.machine(),
        "cpu_count": os.cpu_count(),
        "a11": getattr(a11, "__version__", "unknown"),
        "revision": _source_revision(),
        "native": _native_extension_build(),
        # The ruler, measured. See `clock_call_ns`: a host where this is
        # microseconds cannot be compared with one where it is nanoseconds on
        # any `latency` row, and nothing else in the record would have said so.
        "clock_call_ns": round(clock_call_ns(), 1),
        "clocksource": _clocksource(),
    }


async def run_selected(
    selected: Sequence[Benchmark],
    *,
    on_result: Callable[[Result], None] | None = None,
    scale: float = 1.0,
    timeout_s: float = 600.0,
) -> list[Result]:
    """Run each benchmark, collecting results and surviving failures.

    A benchmark that raises is reported and skipped rather than taking the run
    down with it: half a table is worth more than a traceback. A benchmark that
    *hangs* is bounded by `timeout_s` for the same reason -- and because a hang
    under load is a genuine finding about the component, not a reason to lose
    the rest of the run.
    """
    collected: list[Result] = []
    for entry in selected:
        budget = timeout_s * (10 if entry.slow else 1)
        started_at = time.perf_counter()
        try:
            produced = await asyncio.wait_for(entry.run(scale), timeout=budget)
        except asyncio.TimeoutError:
            # Tell the harness's own timeout apart from the benchmark's.
            #
            # A benchmark that bounds its own waits raises exactly this
            # exception type, so catching it blindly reported "hung after 600s"
            # for something that had in fact failed after twenty -- and led to
            # a diagnosis of a deadlock that was not there. Only an elapsed time
            # at the budget is a hang; anything sooner came from inside.
            if time.perf_counter() - started_at < budget * 0.9:
                print(
                    f"  TIMED OUT {entry.suite}/{entry.name}: the benchmark's"
                    " own wait expired after"
                    f" {time.perf_counter() - started_at:.0f}s -- it failed,"
                    " it did not hang",
                    flush=True,
                )
                continue
            # A benchmark that really hangs is a result, not an accident: the
            # thing being measured stopped making progress under the load the
            # benchmark applied. Say so, keep whatever the run produced before
            # it, and carry on -- an overnight run must not be lost to one
            # stalled component.
            print(
                f"  HUNG {entry.suite}/{entry.name}: no result within"
                f" {timeout_s:.0f}s -- treat this as a finding",
                flush=True,
            )
            continue
        except Skip as skipped:
            print(f"  skip {entry.suite}/{entry.name}: {skipped}", flush=True)
            continue
        except Exception as failure:  # noqa: BLE001 - one bad bench, not all
            import traceback

            print(f"  FAIL {entry.suite}/{entry.name}: {failure!r}", flush=True)
            traceback.print_exc()
            continue
        for result in produced or ():
            collected.append(result)
            if on_result is not None:
                on_result(result)
    return collected


def write_json(path: str, results: Sequence[Result]) -> None:
    payload = {
        "environment": environment(),
        "recorded_at": time.time(),
        "results": [result.to_json() for result in results],
    }
    with open(path, "w") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")


def partial_path(path: str) -> str:
    """Where results are streamed as they finish, alongside @p path."""
    return path + ".partial"


def append_partial(path: str, result: Result) -> None:
    """Append one finished result to the partial log, as a JSON line.

    A suite that has to be killed for exceeding its timeout used to take every
    result it had already produced with it, because the JSON was written once at
    the end: an hour of measurement could vanish because the last benchmark hung.
    Each line here is flushed and fsynced as it is produced, so whatever finished
    survives the kill.
    """
    with open(path, "a") as handle:
        handle.write(json.dumps(result.to_json(), sort_keys=True) + "\n")
        handle.flush()
        os.fsync(handle.fileno())


def read_partial(path: str) -> list[Result]:
    """Read a partial log, ignoring a final line truncated by a kill."""
    results = []
    try:
        with open(path) as handle:
            lines = handle.readlines()
    except OSError:
        return results
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            results.append(Result(**json.loads(line)))
        except (ValueError, TypeError):
            # Only ever the last line, and only when the process died mid-write.
            continue
    return results


def read_json(path: str) -> dict[str, Result]:
    with open(path) as handle:
        payload = json.load(handle)
    loaded = {}
    for record in payload["results"]:
        result = Result(**record)
        loaded[result.key] = result
    return loaded
