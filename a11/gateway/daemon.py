# Copyright 2026 The A11 Authors.

"""Running the gateway in the background, and managing it afterwards.

A detached gateway needs three things that a foreground one does not: somewhere
to put its state so a later `a11 gateway status` can find it, somewhere to put
its output so `a11 gateway logs` can show it, and a way to be stopped that the
process actually honours.

The single-instance gateway stores state in a small JSON file beside its log.
Liveness uses `kill(pid, 0)` and, when required, the same ``__ping`` probe as a
client. A pid recycled by an unrelated process must not be reported as a running
gateway.
"""

from __future__ import annotations

import contextlib
import dataclasses
import json
import os
import pathlib
import signal
import subprocess
import sys
import time
from collections.abc import Iterator, Sequence

from absl import logging

from a11.gateway.config import GatewayConfig

#: How long `stop` waits for a graceful exit before sending SIGKILL.
STOP_TIMEOUT_SECONDS = 10.0
#: How long `start --detach` waits for the child to report that it is listening.
START_TIMEOUT_SECONDS = 20.0


def runtime_dir() -> pathlib.Path:
    """Where the daemon keeps its state and log.

    ``XDG_RUNTIME_DIR`` when set, because that is where a user daemon's runtime
    state belongs; otherwise the cache directory, which at least survives having
    no session bus.
    """
    if runtime := os.environ.get("XDG_RUNTIME_DIR", ""):
        return pathlib.Path(runtime).expanduser() / "a11" / "gateway"
    cache = os.environ.get("XDG_CACHE_HOME", "") or "~/.cache"
    return pathlib.Path(cache).expanduser() / "a11" / "gateway"


def state_file() -> pathlib.Path:
    """The JSON file describing the running gateway."""
    return runtime_dir() / "gateway.json"


def log_file() -> pathlib.Path:
    """Where a detached gateway's output goes."""
    return runtime_dir() / "gateway.log"


@dataclasses.dataclass(frozen=True)
class GatewayStatus:
    """What is known about the gateway right now."""

    running: bool
    pid: int | None = None
    url: str = ""
    started_at: float | None = None
    #: Set when the state file described a gateway that is no longer there.
    stale: bool = False

    @property
    def uptime_seconds(self) -> float | None:
        if self.started_at is None or not self.running:
            return None
        return max(0.0, time.time() - self.started_at)

    def as_fields(self) -> dict[str, object]:
        """The status as stable, scriptable fields."""
        fields: dict[str, object] = {"running": self.running}
        if self.pid is not None:
            fields["pid"] = self.pid
        if self.url:
            fields["url"] = self.url
        uptime = self.uptime_seconds
        if uptime is not None:
            fields["uptime_seconds"] = int(uptime)
        fields["log"] = str(log_file())
        return fields


def _read_state() -> dict | None:
    try:
        return json.loads(state_file().read_text())
    except (OSError, ValueError):
        return None


def _alive(pid: int) -> bool:
    """Whether a process with this pid is actually still running.

    `os.kill(pid, 0)` is not enough on its own. When the gateway was spawned by
    *this* process -- `a11 gateway start --detach` followed by a `stop` in the
    same interpreter, or a test doing both -- an exited child stays a zombie
    until it is reaped, and signalling a zombie succeeds. Without the reap, a
    gateway that shut down promptly still looks alive for the whole stop timeout
    and then gets a pointless SIGKILL.
    """
    try:
        # Non-blocking, and only meaningful for our own children; anything else
        # raises ChildProcessError and falls through to the signal probe.
        reaped, _status = os.waitpid(pid, os.WNOHANG)
        if reaped == pid:
            return False
    except (ChildProcessError, OSError):
        pass
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        # It exists but belongs to someone else, which for a user daemon means
        # it is not the gateway we wrote down.
        return False
    return True


def write_state(config: GatewayConfig, urls: Sequence[str]) -> None:
    """Record that this process is now serving. Called by the gateway itself."""
    directory = runtime_dir()
    directory.mkdir(parents=True, exist_ok=True)
    payload = {
        "pid": os.getpid(),
        "url": urls[0] if urls else config.url,
        "urls": list(urls) or [config.url],
        "started_at": time.time(),
        "argv": sys.argv,
    }
    state_file().write_text(json.dumps(payload, indent=2))


def clear_state() -> None:
    """Remove state unless it describes a live gateway that is not us.

    A live foreign pid is left alone so its gateway remains manageable. A record
    whose process is gone is stale and can be cleared.
    """
    state = _read_state()
    if state is not None:
        pid = state.get("pid")
        if isinstance(pid, int) and pid != os.getpid() and _alive(pid):
            return
    with contextlib.suppress(OSError):
        state_file().unlink()


@contextlib.contextmanager
def recorded(config: GatewayConfig, urls: Sequence[str]) -> Iterator[None]:
    """Record this gateway while it serves, and clean up on the way out."""
    write_state(config, urls)
    try:
        yield
    finally:
        clear_state()


def status() -> GatewayStatus:
    """What the gateway is doing, judged from the state file and the pid.

    Cheap and synchronous. `probed_status` additionally confirms the process
    answers, which a recycled pid or a wedged gateway would not.

    Returns:
        The status. A state file describing a process that is gone is reported
        with ``stale`` set and removed.
    """
    state = _read_state()
    if state is None:
        return GatewayStatus(running=False)
    pid = state.get("pid")
    if not isinstance(pid, int) or not _alive(pid):
        clear_state()
        return GatewayStatus(
            running=False, pid=pid if isinstance(pid, int) else None, stale=True
        )
    return GatewayStatus(
        running=True,
        pid=pid,
        url=str(state.get("url", "")),
        started_at=state.get("started_at"),
    )


async def probed_status() -> GatewayStatus:
    """`status`, plus a ``__ping`` round trip to the recorded endpoint.

    A live pid alone cannot distinguish a responsive gateway from a wedged or
    unrelated process. Await this on the caller's loop because the CLI already
    runs inside one and cannot use a nested ``asyncio.run``.
    """
    resolved = status()
    if not resolved.running:
        return resolved
    if not await _answers(resolved.url):
        clear_state()
        return dataclasses.replace(resolved, running=False, stale=True)
    return resolved


async def _answers(url: str) -> bool:
    """Whether a gateway answers a ping at ``url``."""
    if not url:
        return False
    from a11.client.connection import GatewayConnection

    try:
        connection = await GatewayConnection.connect(url)
    except Exception:  # noqa: BLE001 - any failure means "not answering"
        return False
    try:
        await connection.probe()
        return True
    except Exception:  # noqa: BLE001
        return False
    finally:
        await connection.aclose()


def spawn(argv_extra: Sequence[str] = ()) -> GatewayStatus:
    """Start a detached gateway and wait until it is listening.

    Re-invokes this interpreter as ``a11 gateway run``, in its own session so a
    terminal hang-up does not take it down, with output appended to the log.

    Args:
        argv_extra: Arguments to pass through to ``a11 gateway run``.

    Returns:
        The status once the child reports it is serving.

    Raises:
        RuntimeError: When a gateway is already running, or the child neither
            started serving nor exited within `START_TIMEOUT_SECONDS`.
    """
    existing = status()
    if existing.running:
        raise RuntimeError(
            f"a gateway is already running (pid {existing.pid}) at"
            f" {existing.url}"
        )

    directory = runtime_dir()
    directory.mkdir(parents=True, exist_ok=True)
    # Removed first, so waiting for it to appear cannot observe a previous
    # run's.
    with contextlib.suppress(OSError):
        state_file().unlink()

    command = [sys.executable, "-m", "a11.cli", "gateway", "run", *argv_extra]
    # Keep redirected stderr unbuffered so `a11 gateway logs` receives output
    # while the child is running.
    child_env = dict(os.environ, PYTHONUNBUFFERED="1")
    with log_file().open("ab") as log:
        child = subprocess.Popen(
            command,
            stdout=log,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
            cwd=os.getcwd(),
            env=child_env,
        )

    deadline = time.monotonic() + START_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        current = status()
        if current.running and current.pid == child.pid:
            return current
        if child.poll() is not None:
            raise RuntimeError(
                f"the gateway exited immediately (code {child.returncode});"
                f" see {log_file()}"
            )
        time.sleep(0.05)

    child.terminate()
    raise RuntimeError(
        f"the gateway did not start within {START_TIMEOUT_SECONDS:.0f}s; see"
        f" {log_file()}"
    )


def stop(*, timeout: float = STOP_TIMEOUT_SECONDS) -> GatewayStatus:
    """Stop a detached gateway, escalating to SIGKILL if it will not go.

    ``SIGTERM`` is a clean shutdown because ``_stop_on_signals`` handles it
    before the native runtime's Abseil failure-signal handler.

    Returns:
        The status before stopping, so a caller can report what it stopped.

    Raises:
        RuntimeError: When no gateway is running.
    """
    before = status()
    if not before.running or before.pid is None:
        raise RuntimeError("no gateway is running")

    os.kill(before.pid, signal.SIGTERM)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not _alive(before.pid):
            clear_state()
            return before
        time.sleep(0.05)

    logging.warning(
        "gateway pid %d did not exit within %.0fs; sending SIGKILL",
        before.pid,
        timeout,
    )
    with contextlib.suppress(ProcessLookupError):
        os.kill(before.pid, signal.SIGKILL)
    clear_state()
    return before


def read_logs(lines: int = 40) -> list[str]:
    """The last ``lines`` lines of the gateway log.

    Returns an empty list when there is no log yet, which is not an error: a
    gateway that has never run detached has nothing to show.
    """
    path = log_file()
    try:
        content = path.read_text(errors="replace")
    except OSError:
        return []
    kept = content.splitlines()
    return kept[-lines:] if lines > 0 else kept


def follow_logs(lines: int = 40) -> Iterator[str]:
    """Yield the tail of the log, then each new line as it is written."""
    for line in read_logs(lines):
        yield line
    path = log_file()
    try:
        handle = path.open("r", errors="replace")
    except OSError:
        return
    with handle:
        handle.seek(0, os.SEEK_END)
        while True:
            line = handle.readline()
            if line:
                yield line.rstrip("\n")
            else:
                time.sleep(0.2)


__all__ = [
    "START_TIMEOUT_SECONDS",
    "STOP_TIMEOUT_SECONDS",
    "GatewayStatus",
    "clear_state",
    "follow_logs",
    "log_file",
    "probed_status",
    "read_logs",
    "recorded",
    "runtime_dir",
    "spawn",
    "state_file",
    "status",
    "stop",
    "write_state",
]
