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
async def test_a_completed_run_declares_where_its_output_ends(manager):
    """A run that finished says so, so a reader can tell it was not cut short.

    Finality is carried by the last line rather than a trailing null chunk: the
    LLM tool runner collects every fragment, and a null (octet-stream) one would
    reach decoders that cannot deserialize it.
    """
    registry = _registry(manager)
    result = await _drive(
        registry, "shell_execute", command="echo one; echo two"
    )

    output = result["output_lines"]
    fragments = []
    while (fragment := await output.next_fragment()) is not None:
        fragments.append(fragment)

    assert [a11.from_chunk(f.data) for f in fragments] == ["one", "two"]
    assert [f.continued for f in fragments] == [True, False]
    assert not any(f.data.is_null() for f in fragments)


@pytest.mark.asyncio
async def test_an_empty_output_is_closed_without_claiming_finality(manager):
    """Nothing was produced, so there is no last fragment to mark.

    Closing is enough to end the reader -- including one across a wire, which
    takes the writer's closure marker as end-of-stream.
    """
    registry = _registry(manager)
    result = await _drive(registry, "shell_execute", command="true")

    output = result["output_lines"]
    assert await _lines(result, "output_lines") == []
    assert await output.get_chunk_store().get_final_seq() is None
    assert not await output.is_writable()


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
    result = await _drive(registry, "shell_execute", command="echo transient")
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


@pytest.mark.asyncio
async def test_each_action_narrates_its_run_for_the_user(manager):
    """Every shell action says what it did on its user-facing log port.

    The log is for the person watching, so what is asserted is that it names the
    things a reader needs to identify the run rather than any particular
    wording. Which things depends on the action: the lifecycle ones name the
    shell, because that is all they did, and `shell_execute` names the command
    and its output, which is what a reader is actually looking at.
    """
    registry = _registry(manager)

    started = await _drive(registry, "shell_start")
    shell_id = await started["shell_id"].next_object(str)
    start_log = "".join(await _lines(started, bash.USER_FACING_LOG_PORT))
    assert shell_id in start_log

    executed = await _drive(
        registry,
        "shell_execute",
        headers={bash.SHELL_ID_HEADER: shell_id},
        command="echo hello",
    )
    assert await _lines(executed, "output_lines") == ["hello"]
    execute_log = "".join(await _lines(executed, bash.USER_FACING_LOG_PORT))
    assert "echo hello" in execute_log
    assert "1 line of output" in execute_log
    assert "hello" in execute_log.rsplit("```", 2)[-2]

    listed = await _drive(registry, "shell_list")
    assert shell_id in "".join(await _lines(listed, bash.USER_FACING_LOG_PORT))

    exited = await _drive(
        registry, "shell_exit", headers={bash.SHELL_ID_HEADER: shell_id}
    )
    assert shell_id in "".join(await _lines(exited, bash.USER_FACING_LOG_PORT))


@pytest.mark.asyncio
async def test_a_failed_command_is_narrated_and_still_fails(manager):
    registry = _registry(manager)
    action = registry.make_action("shell_execute")
    action.set_header(bash.SHELL_ID_HEADER, b"missing")
    action.run()
    await action["command"].put("echo hi", final=True)
    for input_name in action.get_schema().inputs:
        await action[input_name].drain_and_close()

    log = "".join(await _lines(action, bash.USER_FACING_LOG_PORT))
    with pytest.raises(StatusException) as raised:
        await asyncio.wait_for(action.wait(), timeout=30)
    assert raised.value.status.code == StatusCode.NOT_FOUND
    # A run that died is the one most worth narrating, so the log leads with
    # the reason rather than the output summary it never got to write.
    assert log.casefold().startswith("error:")
    assert "missing" in log
    # And the command still shows, so a reader knows what died.
    assert "echo hi" in log
