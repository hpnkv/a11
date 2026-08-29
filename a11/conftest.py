import os

import pytest

import a11


@pytest.fixture(scope="session", autouse=True)
def init_logging():
    os.environ["A11_DEBUG"] = "1"
    a11.enable_logging("debug")
    a11.get_logger(__name__).info("Logging initialized")


@pytest.fixture(scope="session", autouse=True)
def fiber_watchdog():
    """Report parked fibers when A11_FIBER_WATCHDOG is set.

    A hung test otherwise reaches the harness timeout with nothing to look at:
    the frames that explain it belong to fibers no OS thread points at. Set
    ``A11_FIBER_WATCHDOG=<seconds>``, and ``A11_FIBER_WATCHDOG_ABORT=1`` to fail
    the run instead of waiting. See ``a11.debug``.
    """
    seconds = os.environ.get("A11_FIBER_WATCHDOG")
    if seconds is None:
        yield
        return
    import a11.debug

    a11.debug.install_fiber_watchdog(
        stall_threshold_seconds=float(seconds),
        abort_on_stall=os.environ.get("A11_FIBER_WATCHDOG_ABORT", "0") != "0",
    )
    yield
