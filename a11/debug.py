"""Read A11 fiber wait state from Python.

A11 blocks in fibers. When one deadlocks, every OS thread is parked in the
scheduler and ``faulthandler.dump_traceback()`` shows nothing about it. The
frames belong to fibers whose stacks are mmap'd regions no thread points at.

:func:`fiber_report` renders those frames. Reach for it when a request never
returns, an async iterator never yields, or a test hangs::

    import a11.debug

    print(a11.debug.fiber_report())

To have the process report on itself, set ``A11_FIBER_WATCHDOG=<seconds>``
before starting it, or call :func:`install_fiber_watchdog`. Both log a report
once a fiber has waited past the threshold. With
:func:`install_fiber_dump_signal_handler`, ``kill -USR2 <pid>`` prints one on
demand.

For a process too wedged to run Python, or a core file, use
``scripts/a11_fibers.py`` under LLDB or GDB.

See ``doc/docs/guides/debugging-concurrency.md``.
"""

from __future__ import annotations

import contextlib
from collections.abc import Iterator
from typing import Any

from a11._native import current_fiber_id as current_fiber_id
from a11._native import fiber_report as fiber_report
from a11._native import fiber_snapshot as fiber_snapshot
from a11._native import find_fiber_deadlock as find_fiber_deadlock
from a11._native import (
    install_fiber_dump_signal_handler as install_fiber_dump_signal_handler,
)
from a11._native import install_fiber_watchdog as install_fiber_watchdog
from a11._native import request_fiber_dump as request_fiber_dump
from a11._native import set_current_fiber_name as set_current_fiber_name
from a11._native import (
    total_completed_fiber_waits as total_completed_fiber_waits,
)

__all__ = [
    "current_fiber_id",
    "fiber_report",
    "fiber_snapshot",
    "fiber_watchdog",
    "find_fiber_deadlock",
    "install_fiber_dump_signal_handler",
    "install_fiber_watchdog",
    "print_fiber_report",
    "request_fiber_dump",
    "set_current_fiber_name",
    "total_completed_fiber_waits",
    "waiting_fibers",
]


def print_fiber_report(
    stall_threshold_seconds: float = 0.0,
    max_frames: int = 24,
    include_running: bool = False,
) -> None:
    """Print :func:`fiber_report` to stdout.

    Args:
        stall_threshold_seconds: Only report fibers that have waited at least
            this long. Zero reports every waiting fiber.
        max_frames: Stack frames per fiber.
        include_running: Include running fibers, which have no parked stack.
    """
    print(
        fiber_report(
            stall_threshold_seconds=stall_threshold_seconds,
            max_frames=max_frames,
            include_running=include_running,
        )
    )


def waiting_fibers(min_waited_seconds: float = 0.0) -> list[dict[str, Any]]:
    """Fibers currently blocked, longest wait first.

    Args:
        min_waited_seconds: Drop fibers that have waited less than this.

    Returns:
        Snapshot dicts as returned by :func:`fiber_snapshot`, excluding running
        fibers and the placeholders A11 keeps for non-fiber threads.
    """
    blocked = [
        fiber
        for fiber in fiber_snapshot()
        if fiber["wait"] not in ("running", "os-thread")
        and fiber["waited_seconds"] >= min_waited_seconds
    ]
    blocked.sort(key=lambda fiber: fiber["waited_seconds"], reverse=True)
    return blocked


@contextlib.contextmanager
def fiber_watchdog(
    stall_threshold_seconds: float = 10.0, abort_on_stall: bool = False
) -> Iterator[None]:
    """Run a block with the fiber watchdog active.

    The watchdog thread outlives the block: A11 keeps one per process and the
    context manager only sets its threshold, so nesting or re-entering is safe.

    Args:
        stall_threshold_seconds: Report once a fiber has waited this long.
        abort_on_stall: Abort the process after reporting, so a hung test fails
            with a report attached instead of timing out silently.

    Yields:
        None.
    """
    install_fiber_watchdog(
        stall_threshold_seconds=stall_threshold_seconds,
        abort_on_stall=abort_on_stall,
    )
    try:
        yield
    finally:
        # Zero disables stall reporting without stopping the thread, which
        # still serves request_fiber_dump().
        install_fiber_watchdog(
            stall_threshold_seconds=0.0, abort_on_stall=False
        )
