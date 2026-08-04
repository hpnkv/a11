# Copyright 2026 The A11 Authors.

"""A single long-lived ``bash`` process A11 can run commands in.

``BashShell`` owns one ``bash`` subprocess and exposes an async, streaming
:meth:`BashShell.execute`. It is deliberately the *only* place that knows how
the shell process is launched and signalled, so a future kernel-level sandbox
can wrap :meth:`BashShell._spawn` (the one call that starts the process) without
touching the streaming/termination protocol layered on top -- mirroring the
process-plus-async-IO shape of ``bao.utils.secure_subprocess``.

The execution protocol keeps a persistent shell reusable across commands while
still allowing a single command to be terminated on its own:

* Commands run in the **foreground** of the shell, so state they set -- exported
  variables, the working directory, shell functions -- persists into the next
  command, which is the whole point of a stateful shell.
* Job control (``set -m``) is enabled, so any external process a command spawns
  lands in its **own** process group, distinct from the shell's. That lets us
  signal just the command's process tree on a timeout, leaving the shell itself
  alive and stateful for the next command.
* Command output (stdout, with stderr folded in) is read until a unique
  end-sentinel that the shell prints after the command finishes; the sentinel
  carries the exit code and marks exactly where this command's output ends.
* On timeout or cancellation the command's process groups are terminated
  (escalating SIGTERM -> SIGKILL); only if the shell then stays unresponsive is
  the whole shell torn down.
"""

from __future__ import annotations

import asyncio
import os
import signal
import uuid
from collections.abc import AsyncIterator

from a11.status import Status, StatusCode

#: How long, in seconds, to wait for a signalled command to release the shell
#: before escalating (SIGTERM -> SIGKILL -> tear down the whole shell).
TERMINATION_GRACE_SECONDS = 2.0


def _kill_process_group(pgid: int, sig: int) -> None:
    """Signal a process group, ignoring one that has already gone away."""
    try:
        os.killpg(pgid, sig)
    except (ProcessLookupError, PermissionError):
        pass


class BashShell:
    """One persistent ``bash`` process with an async command protocol.

    A shell runs at most one command at a time (enforced by an internal lock),
    matching the semantics of a real interactive terminal.
    """

    def __init__(self, bash_path: str = "bash") -> None:
        self._bash_path = bash_path
        self._proc: asyncio.subprocess.Process | None = None
        self._exec_lock = asyncio.Lock()
        self._last_exit_code: int | None = None

    # -- lifecycle ---------------------------------------------------------

    async def _spawn(self) -> asyncio.subprocess.Process:
        """Launch the ``bash`` process. The single sandbox seam.

        A future sandboxed variant swaps only this method (e.g. prefixing the
        argv with a jailer, or launching through ``secure_subprocess``); the
        contract is a process with stdin/stdout pipes, stderr folded into
        stdout, and its own session so the whole tree is signallable as a group.
        """
        return await asyncio.create_subprocess_exec(
            self._bash_path,
            "--noprofile",
            "--norc",
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
            start_new_session=True,
        )

    async def start(self) -> None:
        """Start the shell process and enable job control."""
        if self._proc is not None:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="Shell is already started.",
            ).to_exception()

        self._proc = await self._spawn()
        # Job control makes each external command its own process group, so a
        # timed-out command can be signalled without hitting the shell.
        self._proc.stdin.write(b"set -m\n")
        await self._proc.stdin.drain()

    @property
    def is_alive(self) -> bool:
        return self._proc is not None and self._proc.returncode is None

    @property
    def last_exit_code(self) -> int | None:
        """Exit status of the most recently completed command, if any."""
        return self._last_exit_code

    def terminate(self) -> None:
        """Terminate the whole shell process group (SIGTERM)."""
        if self._proc is not None:
            _kill_process_group(self._proc.pid, signal.SIGTERM)

    def kill(self) -> None:
        """Hard-kill the whole shell process group (SIGKILL)."""
        if self._proc is not None:
            _kill_process_group(self._proc.pid, signal.SIGKILL)

    async def close(self) -> None:
        """Gracefully shut the shell down and release its resources.

        Closes stdin so ``bash`` exits on its own, escalating to SIGTERM and
        then SIGKILL if it lingers. Safe to call more than once.
        """
        if self._proc is None:
            return

        if self._proc.returncode is None:
            try:
                if self._proc.stdin is not None and not (
                    self._proc.stdin.is_closing()
                ):
                    self._proc.stdin.close()
            except (BrokenPipeError, OSError):
                pass
            try:
                await asyncio.wait_for(self._proc.wait(), timeout=1.0)
            except (asyncio.TimeoutError, asyncio.CancelledError):
                self.terminate()
                try:
                    await asyncio.wait_for(self._proc.wait(), timeout=1.0)
                except (asyncio.TimeoutError, asyncio.CancelledError):
                    self.kill()

    async def wait_closed(self) -> None:
        """Wait for the shell process to fully exit."""
        if self._proc is not None:
            await self._proc.wait()

    # -- execution ---------------------------------------------------------

    async def execute(
        self, command: str, timeout: float | None = None
    ) -> AsyncIterator[str]:
        """Run ``command`` and yield its output lines as they complete.

        ``timeout`` is a hard wall-clock ceiling (in seconds) on the command's
        completion; ``None`` means no ceiling. If the command overruns it, or
        this generator is closed early because the surrounding action was
        cancelled, the command's process groups are terminated (escalating to a
        kill, and to tearing down the whole shell only if it stays
        unresponsive).

        Raises:
            StatusException: ``DEADLINE_EXCEEDED`` if ``timeout`` elapses;
                ``UNAVAILABLE`` if the shell dies mid-command;
                ``FAILED_PRECONDITION`` if the shell was never started.
        """
        if not self.is_alive:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="Shell is not running.",
            ).to_exception()

        async with self._exec_lock:
            token = f"__A11_END_{uuid.uuid4().hex}__".encode()
            await self._write_command(command, token)

            loop = asyncio.get_running_loop()
            end_time = None if timeout is None else loop.time() + timeout
            buffer = b""
            finished = False
            try:
                while True:
                    remaining = (
                        None if end_time is None else end_time - loop.time()
                    )
                    chunk = await self._read_stdout(remaining)
                    if chunk == b"":
                        raise Status(
                            code=StatusCode.UNAVAILABLE,
                            message="Shell exited before the command finished.",
                        ).to_exception()

                    buffer += chunk
                    index = buffer.find(token)
                    if index != -1:
                        for line in buffer[:index].splitlines():
                            yield line.decode("utf-8", "replace")
                        self._last_exit_code = await self._read_exit_code(
                            buffer[index + len(token) :]
                        )
                        finished = True
                        return

                    # Flush complete lines; keep the unterminated tail so it is
                    # not split across the coming end-sentinel.
                    if b"\n" in buffer:
                        head, _, buffer = buffer.rpartition(b"\n")
                        for line in head.splitlines():
                            yield line.decode("utf-8", "replace")
            except asyncio.TimeoutError as exc:
                await self._terminate_command(token)
                finished = True
                raise Status(
                    code=StatusCode.DEADLINE_EXCEEDED,
                    message=(
                        f"Command timed out after {timeout:g} seconds and was"
                        " terminated."
                    ),
                ).to_exception() from exc
            finally:
                # Covers early generator close (action cancellation), the shell
                # dying, and any other error: never leave a command running.
                if not finished:
                    await self._terminate_command(token)

    async def _write_command(self, command: str, token: bytes) -> None:
        """Write the foreground command wrapper for ``command`` to the shell.

        The command runs inside a brace group (which, unlike a subshell, runs in
        the shell's own environment so ``export``/``cd`` persist) with its stdin
        redirected from ``/dev/null`` so it can never steal the command stream.
        The sentinel that follows carries the command's exit status.
        """
        wrapper = (
            b"{ "
            + command.encode("utf-8")
            + b"\n} </dev/null\n"
            + b"printf '"
            + token
            + b" %d\\n' \"$?\"\n"
        )
        self._proc.stdin.write(wrapper)
        await self._proc.stdin.drain()

    async def _read_stdout(self, timeout: float | None) -> bytes:
        """Read the next stdout chunk within the remaining timeout budget."""
        if timeout is not None and timeout <= 0:
            raise asyncio.TimeoutError
        read = self._proc.stdout.read(65536)
        if timeout is None:
            return await read
        return await asyncio.wait_for(read, timeout=timeout)

    async def _read_exit_code(self, trailing: bytes) -> int:
        """Parse the exit code that follows the end-sentinel."""
        while b"\n" not in trailing:
            more = await asyncio.wait_for(
                self._proc.stdout.read(4096), timeout=TERMINATION_GRACE_SECONDS
            )
            if more == b"":
                break
            trailing += more
        parts = trailing.split()
        try:
            return int(parts[0])
        except (IndexError, ValueError):
            return -1

    async def _terminate_command(self, token: bytes) -> None:
        """Stop the running command, keeping the shell if it recovers quickly.

        Signals every process group spawned by the command (discovered from the
        process table) with SIGTERM, then SIGKILL, draining the shell's output
        up to this command's end-sentinel after each step to confirm the shell
        became responsive again. If it never does, the whole shell is torn down.
        """
        if not self.is_alive:
            return

        for sig in (signal.SIGTERM, signal.SIGKILL):
            for pgid in await self._command_process_groups():
                _kill_process_group(pgid, sig)
            if await self._drain_to_token(token, TERMINATION_GRACE_SECONDS):
                return

        # The shell itself is wedged (e.g. an uninterruptible command): the
        # command could not be terminated quickly, so tear the shell down.
        self.terminate()

    async def _command_process_groups(self) -> set[int]:
        """Process groups of the shell's descendants, excluding the shell's own.

        Under job control the running command's processes sit in one or more
        groups distinct from the shell's, so signalling these stops the command
        without touching the shell. The shell is session-detached, so the full
        process table (``ps -A``) is consulted rather than the terminal's view.
        """
        if self._proc is None:
            return set()
        shell_pid = self._proc.pid
        shell_pgid = os.getpgid(shell_pid)

        try:
            ps = await asyncio.create_subprocess_exec(
                "ps",
                "-A",
                "-o",
                "pid=,pgid=,ppid=",
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.DEVNULL,
            )
            stdout, _ = await ps.communicate()
        except (OSError, asyncio.CancelledError):
            return set()

        children: dict[int, list[int]] = {}
        pgid_of: dict[int, int] = {}
        for line in stdout.decode("utf-8", "replace").splitlines():
            parts = line.split()
            if len(parts) < 3:
                continue
            try:
                pid, pgid, ppid = int(parts[0]), int(parts[1]), int(parts[2])
            except ValueError:
                continue
            children.setdefault(ppid, []).append(pid)
            pgid_of[pid] = pgid

        groups: set[int] = set()
        seen: set[int] = set()
        stack = [shell_pid]
        while stack:
            for child in children.get(stack.pop(), []):
                if child in seen:
                    continue
                seen.add(child)
                stack.append(child)
                group = pgid_of[child]
                if group != shell_pgid and group > 0:
                    groups.add(group)
        return groups

    async def _drain_to_token(self, token: bytes, timeout: float) -> bool:
        """Discard stdout up to the command's end-sentinel; ``True`` if seen."""
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        buffer = b""
        while token not in buffer:
            remaining = deadline - loop.time()
            if remaining <= 0:
                return False
            try:
                chunk = await asyncio.wait_for(
                    self._proc.stdout.read(65536), timeout=remaining
                )
            except asyncio.TimeoutError:
                return False
            if chunk == b"":
                return False
            buffer += chunk
        await self._read_exit_code(buffer[buffer.find(token) + len(token) :])
        return True
