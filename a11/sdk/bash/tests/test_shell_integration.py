# Copyright 2026 The A11 Authors.

"""End-to-end tests driving the four Actions against a real ``bash`` process.

``bash`` is present on macOS and Linux, so these always run. Each test uses its
own :class:`ShellManager` (bound onto the handlers with ``functools.partial`` so
the process-global manager is left untouched), and a fixture closes any
surviving shells so no ``bash`` processes leak between tests.
"""

import asyncio
import functools

import pytest

import a11
from a11.actions import Action, ActionSchema, ActionRegistry
from a11.sdk import bash
from a11.sdk.bash.manager import ShellManager
from a11.status import StatusCode, StatusException


@pytest.fixture
def manager():
    mgr = ShellManager()
    yield mgr
    # Reap anything a test left running so no bash processes leak.
    for entry in list(mgr._shells.values()):
        entry.shell.terminate()
        entry.shell.kill()
    mgr._shells.clear()


def _registry(manager: ShellManager) -> ActionRegistry:
    """A registry whose handlers use ``manager`` instead of the global one."""
    registry = ActionRegistry()
    for schema, handler in bash.SHELL_ACTIONS:
        registry.register(
            schema.name, schema, functools.partial(handler, manager=manager)
        )
    return registry


async def _drive(
    registry: ActionRegistry,
    name: str,
    *,
    headers: dict[str, str] | None = None,
    command: str | None = None,
) -> Action:
    """Run one action to completion, closing inputs the way the runner does."""
    action = registry.make_action(name)
    for key, value in (headers or {}).items():
        action.set_header(key, value.encode())
    action.run()
    if command is not None:
        await action["command"].put(command, final=True)
    for input_name in action.get_schema().inputs:
        await action[input_name].drain_and_close()
    await asyncio.wait_for(action.wait(), timeout=30)
    return action


async def _lines(action: Action, port: str) -> list[str]:
    out: list[str] = []
    while (value := await action[port].next_object(str)) is not None:
        out.append(value)
    return out


@pytest.mark.asyncio
async def test_start_execute_list_exit_roundtrip(manager):
    registry = _registry(manager)

    started = await _drive(registry, "shell_start")
    shell_id = await started["shell_id"].next_object(str)
    assert shell_id

    executed = await _drive(
        registry,
        "shell_execute",
        headers={bash.SHELL_ID_HEADER: shell_id},
        command="echo hello",
    )
    assert await _lines(executed, "output_lines") == ["hello"]

    listed = await _drive(registry, "shell_list")
    assert await _lines(listed, "shell_ids") == [shell_id]

    await _drive(
        registry, "shell_exit", headers={bash.SHELL_ID_HEADER: shell_id}
    )
    listed = await _drive(registry, "shell_list")
    assert await _lines(listed, "shell_ids") == []


@pytest.mark.asyncio
async def test_state_persists_across_executions(manager):
    registry = _registry(manager)
    started = await _drive(registry, "shell_start")
    shell_id = await started["shell_id"].next_object(str)

    await _drive(
        registry,
        "shell_execute",
        headers={bash.SHELL_ID_HEADER: shell_id},
        command="export GREETING=hi; cd /tmp",
    )
    result = await _drive(
        registry,
        "shell_execute",
        headers={bash.SHELL_ID_HEADER: shell_id},
        command="echo $GREETING; pwd",
    )
    assert await _lines(result, "output_lines") == ["hi", "/tmp"]


@pytest.mark.asyncio
async def test_stderr_is_interleaved_into_output(manager):
    registry = _registry(manager)
    result = await _drive(
        registry,
        "shell_execute",
        command="echo out; echo err 1>&2; echo out2",
    )
    assert set(await _lines(result, "output_lines")) == {"out", "err", "out2"}


@pytest.mark.asyncio
async def test_transient_shell_leaves_no_shell_behind(manager):
    registry = _registry(manager)
    result = await _drive(
        registry, "shell_execute", command="echo transient"
    )
    assert await _lines(result, "output_lines") == ["transient"]
    listed = await _drive(registry, "shell_list")
    assert await _lines(listed, "shell_ids") == []


@pytest.mark.asyncio
async def test_timeout_terminates_command_and_keeps_shell(manager):
    registry = _registry(manager)
    started = await _drive(registry, "shell_start")
    shell_id = await started["shell_id"].next_object(str)

    action = registry.make_action("shell_execute")
    action.set_header(bash.SHELL_ID_HEADER, shell_id.encode())
    await action["parameters"].put(
        bash.A11ShellExecuteParameters(timeout_seconds=1), final=True
    )
    action.run()
    await action["command"].put("echo before; sleep 30", final=True)
    await action["parameters"].drain_and_close()
    await action["command"].drain_and_close()

    loop = asyncio.get_running_loop()
    started_at = loop.time()
    with pytest.raises(StatusException) as raised:
        await asyncio.wait_for(action.wait(), timeout=15)
    assert raised.value.status.code == StatusCode.DEADLINE_EXCEEDED
    # Terminated promptly -- not after the full 30s sleep.
    assert loop.time() - started_at < 10

    # The shell survived and remains usable and stateful.
    result = await _drive(
        registry,
        "shell_execute",
        headers={bash.SHELL_ID_HEADER: shell_id},
        command="echo recovered",
    )
    assert await _lines(result, "output_lines") == ["recovered"]


@pytest.mark.asyncio
async def test_cancellation_terminates_running_command(manager):
    registry = _registry(manager)
    started = await _drive(registry, "shell_start")
    shell_id = await started["shell_id"].next_object(str)

    action = registry.make_action("shell_execute")
    action.set_header(bash.SHELL_ID_HEADER, shell_id.encode())
    action.run()
    await action["command"].put("echo started; sleep 30", final=True)
    await action["parameters"].drain_and_close()
    await action["command"].drain_and_close()

    # Wait until the command is actually running, then cancel the action.
    await asyncio.sleep(1.0)
    action.cancel()
    with pytest.raises(StatusException) as raised:
        await asyncio.wait_for(action.wait(), timeout=15)
    assert raised.value.status.code == StatusCode.CANCELLED

    # The shell was kept and the killed command left it usable.
    result = await _drive(
        registry,
        "shell_execute",
        headers={bash.SHELL_ID_HEADER: shell_id},
        command="echo alive",
    )
    assert await _lines(result, "output_lines") == ["alive"]


@pytest.mark.asyncio
async def test_exit_unknown_shell_is_not_found(manager):
    registry = _registry(manager)
    with pytest.raises(StatusException) as raised:
        await _drive(
            registry, "shell_exit", headers={bash.SHELL_ID_HEADER: "missing"}
        )
    assert raised.value.status.code == StatusCode.NOT_FOUND


@pytest.mark.asyncio
async def test_exit_requires_the_shell_id_header(manager):
    registry = _registry(manager)
    with pytest.raises(StatusException) as raised:
        await _drive(registry, "shell_exit")
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT
