# Copyright 2026 The A11 Authors.

"""Turning a termination signal into a clean shutdown, for serving commands."""

from __future__ import annotations

import asyncio
import contextlib
import signal
from collections.abc import Iterator


@contextlib.contextmanager
def stop_on_signals() -> Iterator[asyncio.Event]:
    """An event set by ``SIGINT``/``SIGTERM``, so a signal is a clean shutdown.

    Installing these matters for more than tidiness: the native runtime installs
    Abseil's failure-signal handler, which treats a plain ``SIGTERM`` as a crash
    and dumps a stack trace before the process dies with the server still
    listening. Handling the signal on the loop takes those two back, and the
    handlers are removed again on the way out so nothing outlives the command.

    It is also what makes ``a11 gateway stop`` work at all, since that sends
    SIGTERM and expects a clean exit.

    Yields:
        The event to await; it is set once either signal arrives.
    """
    loop = asyncio.get_running_loop()
    stop = asyncio.Event()
    installed: list[signal.Signals] = []
    for number in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(number, stop.set)
            installed.append(number)
        except NotImplementedError:
            # Not a POSIX loop; the KeyboardInterrupt path in `cli.app` remains.
            pass
    try:
        yield stop
    finally:
        for number in installed:
            loop.remove_signal_handler(number)


__all__ = ["stop_on_signals"]
