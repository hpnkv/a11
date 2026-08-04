# Copyright 2026 The A11 Authors.

"""The administrator that owns shells and runs commands in them.

``ShellManager`` is the policy layer between the four shell Actions and the raw
:class:`~a11.sdk.bash.shell.BashShell` process. It:

* tracks running shells and enforces the per-scope caps (a shell is scoped to
  the ``Session`` it was started in, or to a process-global scope when there is
  none);
* reaps a session's shells when that session completes or dies, via
  ``Session.add_done_callback``, and reaps a transient shell when the action it
  was started for completes, via ``Action.add_done_callback``;
* exposes two extension points -- input-command validators and output-line
  processors -- so callers can sanitise commands and post-process output without
  subclassing the Actions;
* drives a command through a shell while honouring the caller's timeout and the
  ``x-a11-deadline`` header.

The manager is process-global (see :func:`get_shell_manager`) because shells
outlive the single action that starts them: ``shell_start`` returns an id that a
later ``shell_execute`` reuses.
"""

from __future__ import annotations

import atexit
import uuid
from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass

import a11
from a11.actions.action import Action
from a11.service.session import Session
from a11.sdk.bash.schemas import A11ShellExecuteParameters
from a11.sdk.bash.shell import BashShell
from a11.status import Status, StatusCode

#: Most shells one Session may keep running at once.
MAX_SHELLS_PER_SESSION = 4
#: Most shells the process-global scope may keep running at once.
MAX_GLOBAL_SHELLS = 10
#: Scope key used for shells started outside any Session.
GLOBAL_SCOPE = "global"

#: Validates (and may rewrite) a command before it runs, or raises to reject it.
InputValidator = Callable[[str], str]
#: Wraps a stream of output lines, yielding a possibly-transformed stream.
OutputProcessor = Callable[[AsyncIterator[str]], AsyncIterator[str]]

#: Factory that produces a fresh, unstarted shell (swappable for tests/sandbox).
ShellFactory = Callable[[], BashShell]


@dataclass
class _ShellEntry:
    shell: BashShell
    scope: str


class ShellManager:
    """Owns running shells, enforces scope caps, and runs commands in them."""

    def __init__(self, shell_factory: ShellFactory = BashShell) -> None:
        self._shell_factory = shell_factory
        self._shells: dict[str, _ShellEntry] = {}
        #: Scopes for which a session done-callback is already registered.
        self._reaped_scopes: set[str] = set()
        #: Command sanitisers, run in order; each returns a command or raises.
        self.input_validators: list[InputValidator] = []
        #: Output-line stream wrappers, applied in order.
        self.output_processors: list[OutputProcessor] = []
        atexit.register(self._reap_all_on_exit)

    # -- scope helpers -----------------------------------------------------

    @staticmethod
    def _scope_for(session: Session | None) -> str:
        return (
            f"session:{session.get_id()}"
            if session is not None
            else (GLOBAL_SCOPE)
        )

    def _count_in_scope(self, scope: str) -> int:
        return sum(1 for entry in self._shells.values() if entry.scope == scope)

    def _ids_in_scope(self, scope: str) -> list[str]:
        return [
            shell_id
            for shell_id, entry in self._shells.items()
            if entry.scope == scope
        ]

    # -- lifecycle ---------------------------------------------------------

    async def start_shell(self, session: Session | None) -> str:
        """Start a shell in the caller's scope and return its id.

        Raises:
            StatusException: ``RESOURCE_EXHAUSTED`` if the scope is already at
                its cap.
        """
        scope = self._scope_for(session)
        limit = (
            MAX_GLOBAL_SHELLS
            if scope == GLOBAL_SCOPE
            else MAX_SHELLS_PER_SESSION
        )
        if self._count_in_scope(scope) >= limit:
            where = (
                "the global scope" if scope == GLOBAL_SCOPE else "this session"
            )
            raise Status(
                code=StatusCode.RESOURCE_EXHAUSTED,
                message=(
                    f"Cannot start another shell: {where} already has {limit}"
                    " running shells (the maximum). Exit a shell before"
                    " starting a new one."
                ),
            ).to_exception()

        shell = self._shell_factory()
        await shell.start()
        shell_id = uuid.uuid4().hex
        self._shells[shell_id] = _ShellEntry(shell=shell, scope=scope)

        # Reap this session's shells when the session completes or dies. Done
        # once per session; the callback closes whatever is still open then.
        if session is not None and scope not in self._reaped_scopes:
            self._reaped_scopes.add(scope)
            session.add_done_callback(
                lambda _session, scope=scope: self._reap_scope(scope)
            )

        return shell_id

    def get_shell(self, shell_id: str) -> BashShell:
        """Return the shell with ``shell_id``.

        Raises:
            StatusException: ``NOT_FOUND`` if no such shell exists.
        """
        entry = self._shells.get(shell_id)
        if entry is None:
            raise Status(
                code=StatusCode.NOT_FOUND,
                message=f"No running shell with id {shell_id!r}.",
            ).to_exception()
        return entry.shell

    async def exit_shell(self, shell_id: str) -> None:
        """Terminate and forget the shell with ``shell_id``.

        Raises:
            StatusException: ``NOT_FOUND`` if no such shell exists.
        """
        entry = self._shells.pop(shell_id, None)
        if entry is None:
            raise Status(
                code=StatusCode.NOT_FOUND,
                message=f"No running shell with id {shell_id!r}.",
            ).to_exception()
        await entry.shell.close()

    def list_shells(self, session: Session | None) -> list[str]:
        """Return the ids of running shells in the caller's scope."""
        return self._ids_in_scope(self._scope_for(session))

    async def _reap_scope(self, scope: str) -> None:
        """Close and forget every shell in ``scope`` (session teardown)."""
        self._reaped_scopes.discard(scope)
        for shell_id in self._ids_in_scope(scope):
            entry = self._shells.pop(shell_id, None)
            if entry is not None:
                await entry.shell.close()

    def _reap_all_on_exit(self) -> None:
        """Synchronously kill any surviving shells at interpreter exit.

        Runs in an ``atexit`` hook where no event loop is available, so it only
        signals process groups (it cannot await a graceful close). This is the
        backstop for globally-scoped shells, which have no session to reap them.
        """
        for entry in list(self._shells.values()):
            entry.shell.terminate()
            entry.shell.kill()
        self._shells.clear()

    # -- extension points --------------------------------------------------

    def validate_command(self, command: str) -> str:
        """Run every input validator in order, returning the final command.

        Each validator either raises a ``StatusException`` to reject the command
        or returns a (possibly edited) replacement.
        """
        for validator in self.input_validators:
            command = validator(command)
            if not isinstance(command, str):
                raise Status(
                    code=StatusCode.INTERNAL,
                    message="An input validator returned a non-string command.",
                ).to_exception()
        return command

    async def _process_output(
        self, lines: AsyncIterator[str]
    ) -> AsyncIterator[str]:
        """Pipe ``lines`` through every registered output processor in order."""
        stream = lines
        for processor in self.output_processors:
            stream = processor(stream)
        async for line in stream:
            yield line

    # -- running commands --------------------------------------------------

    def _effective_timeout(
        self, parameters: A11ShellExecuteParameters, action: Action
    ) -> float:
        """Combine the parameter timeout with the ``x-a11-deadline`` header.

        Returns the smaller of the (capped) requested timeout and the time left
        until the deadline.

        Raises:
            StatusException: ``DEADLINE_EXCEEDED`` if the deadline has passed.
        """
        timeout = min(parameters.timeout_seconds, A11ShellExecuteParameters.MAX)
        deadline = a11.get_deadline(action)
        now = a11.now()
        if deadline <= now:
            raise Status(
                code=StatusCode.DEADLINE_EXCEEDED,
                message="The action deadline has already passed.",
            ).to_exception()
        remaining = deadline - now
        if not remaining.is_infinite():
            timeout = min(timeout, remaining.float_seconds())
        return timeout

    async def run_command(
        self,
        command: str,
        shell_id: str | None,
        parameters: A11ShellExecuteParameters,
        action: Action,
    ) -> AsyncIterator[str]:
        """Run ``command`` and yield its processed output lines.

        If ``shell_id`` is given the command runs in that (persistent) shell; if
        it is ``None`` a transient shell is started for this command and torn
        down when it finishes -- guaranteed even on cancellation by registering
        the teardown on the action's completion.

        Raises:
            StatusException: ``NOT_FOUND`` for an unknown ``shell_id``,
                ``DEADLINE_EXCEEDED`` on timeout/expired deadline, or whatever a
                validator or the shell raises.
        """
        command = self.validate_command(command)
        timeout = self._effective_timeout(parameters, action)

        transient: BashShell | None = None
        if shell_id is not None:
            shell = self.get_shell(shell_id)
        else:
            transient = self._shell_factory()
            await transient.start()
            # Safety net: if the handler is torn down without draining us, the
            # transient shell is still closed when the action completes.
            action.add_done_callback(lambda _action, sh=transient: sh.close())
            shell = transient

        try:
            async for line in self._process_output(
                shell.execute(command, timeout=timeout)
            ):
                yield line
        finally:
            if transient is not None:
                await transient.close()


_SHELL_MANAGER: ShellManager | None = None


def get_shell_manager() -> ShellManager:
    """Return the process-global :class:`ShellManager`, made on first use.

    Shells outlive the action that starts them, so the manager that tracks them
    must be a single process-wide instance shared across every shell action.
    """
    global _SHELL_MANAGER
    if _SHELL_MANAGER is None:
        _SHELL_MANAGER = ShellManager()
    return _SHELL_MANAGER
