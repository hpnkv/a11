# Copyright 2026 The A11 Authors.

"""Manager-level tests using a fake shell -- no real processes.

These pin the policy the ``ShellManager`` owns: scope caps, id lookup, session
and transient-action reaping, the validator/processor extension points, and the
timeout/deadline math -- without paying for real ``bash`` subprocesses.
"""

import asyncio
from collections.abc import AsyncIterator

import pytest

import a11
from a11.actions import Action, ActionSchema
from a11.service.session import Session, SessionOptions
from a11 import timing
from a11.sdk.bash.manager import (
    MAX_GLOBAL_SHELLS,
    MAX_SHELLS_PER_SESSION,
    ShellManager,
)
from a11.sdk.bash.schemas import A11ShellExecuteParameters
from a11.status import StatusCode, StatusException


class FakeShell:
    """A stand-in for ``BashShell`` recording how the manager drives it."""

    def __init__(self) -> None:
        self.started = False
        self.closed = False
        self.terminated = False
        self.killed = False
        self.executed: list[tuple[str, float | None]] = []
        self.output_lines: list[str] = ["line-1", "line-2"]
        self.last_exit_code: int | None = None

    @property
    def is_alive(self) -> bool:
        return self.started and not self.closed

    async def start(self) -> None:
        self.started = True

    async def execute(
        self, command: str, timeout: float | None = None
    ) -> AsyncIterator[str]:
        self.executed.append((command, timeout))
        for line in self.output_lines:
            yield line
        self.last_exit_code = 0

    async def close(self) -> None:
        self.closed = True

    def terminate(self) -> None:
        self.terminated = True

    def kill(self) -> None:
        self.killed = True


def _manager() -> tuple[ShellManager, list[FakeShell]]:
    made: list[FakeShell] = []

    def factory() -> FakeShell:
        shell = FakeShell()
        made.append(shell)
        return shell

    return ShellManager(shell_factory=factory), made


def _session() -> Session:
    return Session(
        options=SessionOptions(no_stream_timeout=timing.infinite_duration())
    )


def _running_action(
    deadline: timing.Time | None = None,
) -> tuple[Action, asyncio.Event]:
    """A started action whose handler blocks until the returned event is set.

    Mirrors reality: ``run_command`` is driven from inside a handler while the
    action is still running, so its completion callbacks do not fire early.
    """
    release = asyncio.Event()

    async def handler(_action: Action) -> None:
        await release.wait()

    action = Action(ActionSchema(name="shell_execute"), handler=handler)
    if deadline is not None:
        # Write the header through the canonical producer so it uses the
        # standard representation (bare milliseconds since the epoch).
        a11.set_deadline_header(action, deadline)
    action.run()
    return action, release


async def _finish(action: Action, release: asyncio.Event) -> None:
    """Release a running action and let its completion callbacks settle."""
    release.set()
    await action.wait()
    await asyncio.sleep(0)


@pytest.mark.asyncio
async def test_session_scope_cap_rejects_the_fifth_shell():
    manager, _ = _manager()
    session = _session()
    ids = [
        await manager.start_shell(session)
        for _ in range(MAX_SHELLS_PER_SESSION)
    ]
    assert len(set(ids)) == MAX_SHELLS_PER_SESSION

    with pytest.raises(StatusException) as raised:
        await manager.start_shell(session)
    assert raised.value.status.code == StatusCode.RESOURCE_EXHAUSTED


@pytest.mark.asyncio
async def test_global_scope_cap_rejects_the_eleventh_shell():
    manager, _ = _manager()
    for _ in range(MAX_GLOBAL_SHELLS):
        await manager.start_shell(None)

    with pytest.raises(StatusException) as raised:
        await manager.start_shell(None)
    assert raised.value.status.code == StatusCode.RESOURCE_EXHAUSTED


@pytest.mark.asyncio
async def test_scopes_are_independent():
    manager, _ = _manager()
    first, second = _session(), _session()
    # Filling one session's scope leaves the other and the global scope free.
    for _ in range(MAX_SHELLS_PER_SESSION):
        await manager.start_shell(first)
    assert len(manager.list_shells(first)) == MAX_SHELLS_PER_SESSION
    assert manager.list_shells(second) == []
    await manager.start_shell(second)
    await manager.start_shell(None)
    assert len(manager.list_shells(second)) == 1
    assert len(manager.list_shells(None)) == 1


@pytest.mark.asyncio
async def test_exit_unknown_shell_is_not_found():
    manager, _ = _manager()
    with pytest.raises(StatusException) as raised:
        await manager.exit_shell("does-not-exist")
    assert raised.value.status.code == StatusCode.NOT_FOUND


@pytest.mark.asyncio
async def test_exit_closes_and_forgets_the_shell():
    manager, made = _manager()
    shell_id = await manager.start_shell(None)
    await manager.exit_shell(shell_id)
    assert made[0].closed is True
    assert manager.list_shells(None) == []


@pytest.mark.asyncio
async def test_session_completion_reaps_its_shells():
    manager, made = _manager()
    session = _session()
    await manager.start_shell(session)
    await manager.start_shell(session)
    assert len(manager.list_shells(session)) == 2

    session.half_close()
    await asyncio.wait_for(session.done.wait(), timeout=1)
    for _ in range(100):
        if not manager.list_shells(session):
            break
        await asyncio.sleep(0.01)

    assert manager.list_shells(session) == []
    assert all(shell.closed for shell in made)


@pytest.mark.asyncio
async def test_transient_shell_is_created_and_torn_down():
    manager, made = _manager()
    action, release = _running_action()
    lines = [
        line
        async for line in manager.run_command(
            "echo hi", None, A11ShellExecuteParameters(), action
        )
    ]
    assert lines == ["line-1", "line-2"]
    assert len(made) == 1
    assert made[0].executed[0][0] == "echo hi"
    assert made[0].closed is True
    # No transient shell is left registered in any scope.
    assert manager.list_shells(None) == []
    await _finish(action, release)


@pytest.mark.asyncio
async def test_transient_shell_closed_when_generator_abandoned():
    manager, made = _manager()
    action, release = _running_action()
    # Start the command but abandon the generator without draining it.
    gen = manager.run_command(
        "sleep 1", None, A11ShellExecuteParameters(), action
    )
    assert await gen.__anext__() == "line-1"
    await gen.aclose()
    # The generator's own cleanup closes the transient shell immediately...
    assert made[0].closed is True
    # ...and the action-completion safety net also runs without error.
    await _finish(action, release)


@pytest.mark.asyncio
async def test_persistent_shell_is_reused_not_closed():
    manager, made = _manager()
    shell_id = await manager.start_shell(None)
    action, release = _running_action()
    async for _ in manager.run_command(
        "echo hi", shell_id, A11ShellExecuteParameters(), action
    ):
        pass
    assert made[0].closed is False
    assert manager.get_shell(shell_id) is made[0]
    await _finish(action, release)


@pytest.mark.asyncio
async def test_input_validator_can_reject_a_command():
    manager, _ = _manager()

    def reject(command: str) -> str:
        raise a11.Status(
            code=StatusCode.PERMISSION_DENIED, message="nope"
        ).to_exception()

    manager.input_validators.append(reject)
    action, release = _running_action()
    with pytest.raises(StatusException) as raised:
        async for _ in manager.run_command(
            "rm -rf /", None, A11ShellExecuteParameters(), action
        ):
            pass
    assert raised.value.status.code == StatusCode.PERMISSION_DENIED
    await _finish(action, release)


@pytest.mark.asyncio
async def test_input_validator_can_edit_a_command():
    manager, made = _manager()
    manager.input_validators.append(lambda command: command + " --safe")
    action, release = _running_action()
    async for _ in manager.run_command(
        "ls", None, A11ShellExecuteParameters(), action
    ):
        pass
    assert made[0].executed[0][0] == "ls --safe"
    await _finish(action, release)


@pytest.mark.asyncio
async def test_output_processor_transforms_adds_and_drops_lines():
    manager, made = _manager()

    async def loud(lines: AsyncIterator[str]) -> AsyncIterator[str]:
        yield "HEADER"
        async for line in lines:
            if line == "line-2":
                continue  # drop
            yield line.upper()  # transform

    manager.output_processors.append(loud)
    action, release = _running_action()
    out = [
        line
        async for line in manager.run_command(
            "cmd", None, A11ShellExecuteParameters(), action
        )
    ]
    assert out == ["HEADER", "LINE-1"]
    await _finish(action, release)


@pytest.mark.asyncio
async def test_expired_deadline_raises_before_running():
    manager, made = _manager()
    action, release = _running_action(
        deadline=timing.now() - timing.Duration.seconds(1)
    )
    with pytest.raises(StatusException) as raised:
        async for _ in manager.run_command(
            "cmd", None, A11ShellExecuteParameters(), action
        ):
            pass
    assert raised.value.status.code == StatusCode.DEADLINE_EXCEEDED
    # Nothing was run.
    assert made == [] or made[0].executed == []
    await _finish(action, release)


@pytest.mark.asyncio
async def test_effective_timeout_is_min_of_param_and_deadline():
    manager, made = _manager()
    # Deadline ~2s away, requested timeout 30s -> effective ~2s.
    action, release = _running_action(
        deadline=timing.now() + timing.Duration.seconds(2)
    )
    async for _ in manager.run_command(
        "cmd", None, A11ShellExecuteParameters(timeout_seconds=30), action
    ):
        pass
    _, timeout = made[0].executed[0]
    assert 0 < timeout <= 2.0
    await _finish(action, release)


@pytest.mark.asyncio
async def test_requested_timeout_is_capped_at_maximum():
    manager, made = _manager()
    action, release = _running_action()  # no deadline
    async for _ in manager.run_command(
        "cmd", None, A11ShellExecuteParameters(timeout_seconds=10_000), action
    ):
        pass
    _, timeout = made[0].executed[0]
    assert timeout == A11ShellExecuteParameters.MAX
    await _finish(action, release)


def test_parameters_reject_non_positive_timeout():
    with pytest.raises(Exception):
        A11ShellExecuteParameters(timeout_seconds=0)


def test_parameters_clamp_timeout_to_maximum():
    params = A11ShellExecuteParameters(timeout_seconds=10_000)
    assert params.timeout_seconds == A11ShellExecuteParameters.MAX
