# Copyright 2026 The A11 Authors.

"""Turning results into something a person reads, and into a diff.

The column set is fixed rather than derived: a table whose columns change with
whatever the last benchmark happened to report is unreadable, and the columns
here (rate, percentiles, byte rate, memory, and the two server-cost figures)
cover every metric a reader needs in the table. Anything else lands in the note.

Three of these columns are only ever populated by the two-host suites, and they
are columns rather than notes on purpose. `p99.9 us` is the figure a server is
judged on and hiding it in prose invites reading the p50 instead. `cores` and
`cpu us/op` come from the *server's* own `getrusage`, and without them a
cross-machine rate comparison is not a comparison: a host with more cores
delivering more operations per second has said nothing until the per-operation
CPU is beside it.
"""

from __future__ import annotations

from collections.abc import Sequence

from bench.harness import Result

#: Column label -> metric key. Order is the column order.
_COLUMNS: list[tuple[str, str]] = [
    ("ops/s", "ops_per_s"),
    ("items/s", "items_per_s"),
    ("p50 us", "p50_us"),
    ("p99 us", "p99_us"),
    ("p99.9 us", "p999_us"),
    ("MiB/s", "mib_per_s"),
    ("bytes ea", "bytes_each"),
    ("tax us", "dispatch_tax_us"),
    ("inflate", "inflation"),
    ("cores", "server_cores_busy"),
    ("cpu us/op", "server_cpu_us_per_op"),
]


def _render(value: float, metric: str) -> str:
    if metric in ("ops_per_s", "items_per_s"):
        if value >= 1_000_000:
            return f"{value / 1_000_000:.2f}M"
        if value >= 1_000:
            return f"{value / 1_000:.1f}k"
        return f"{value:.0f}"
    if metric == "bytes_each":
        if value >= 1024 * 1024:
            return f"{value / 1024 / 1024:.2f}M"
        if value >= 1024:
            return f"{value / 1024:.1f}K"
        return f"{value:.0f}"
    if value >= 1000:
        return f"{value:,.0f}"
    if value >= 10:
        return f"{value:.1f}"
    return f"{value:.3f}"


def _label(result: Result) -> str:
    if not result.params:
        return result.name
    rendered = " ".join(f"{k}={v}" for k, v in result.params.items())
    return f"{result.name}  {rendered}"


def table(results: Sequence[Result]) -> str:
    """A fixed-column table, grouped by suite."""
    if not results:
        return "(no results)"
    present = [
        (label, metric)
        for label, metric in _COLUMNS
        if any(metric in result.metrics for result in results)
    ]
    width = max(len(_label(result)) for result in results) + 2
    width = max(width, 30)
    lines: list[str] = []
    header = "benchmark".ljust(width) + "".join(
        label.rjust(11) for label, _ in present
    )
    for suite in _suites(results):
        lines.append("")
        lines.append(f"[{suite}]")
        lines.append(header)
        lines.append("-" * len(header))
        for result in results:
            if result.suite != suite:
                continue
            row = _label(result).ljust(width)
            for _, metric in present:
                value = result.metrics.get(metric)
                row += ("-" if value is None else _render(value, metric)).rjust(
                    11
                )
            lines.append(row)
            if result.note:
                lines.append(f"{'':{width}}  -- {result.note}")
    return "\n".join(lines)


def _suites(results: Sequence[Result]) -> list[str]:
    seen: list[str] = []
    for result in results:
        if result.suite not in seen:
            seen.append(result.suite)
    return seen


#: Metrics where a bigger number is better; everything else is a latency.
_HIGHER_IS_BETTER = {"ops_per_s", "items_per_s", "mib_per_s"}
#: Below this, treat a difference as noise rather than as news.
_NOISE = 0.05


def compare(
    current: Sequence[Result],
    baseline: dict[str, Result],
    *,
    threshold: float = _NOISE,
) -> str:
    """Every metric that moved more than `threshold` since the baseline run."""
    lines: list[str] = []
    for result in current:
        previous = baseline.get(result.key)
        if previous is None:
            lines.append(f"  new     {result.key}")
            continue
        for metric, value in sorted(result.metrics.items()):
            was = previous.metrics.get(metric)
            if not was or metric in ("elapsed_s", "max_us", "stdev_us"):
                continue
            change = (value - was) / was
            if abs(change) < threshold:
                continue
            better = change > 0 if metric in _HIGHER_IS_BETTER else change < 0
            mark = "better" if better else "WORSE "
            lines.append(
                f"  {mark}  {result.key} {metric}: "
                f"{was:.4g} -> {value:.4g} ({change:+.1%})"
            )
    missing = sorted(set(baseline) - {result.key for result in current})
    lines.extend(f"  gone    {key}" for key in missing)
    return "\n".join(lines) or "  (no change beyond threshold)"
