# Copyright 2026 The A11 Authors.

"""Handlers for the four Bash-tool Actions.

Each handler is a thin adapter over :class:`~a11.sdk.bash.manager.ShellManager`:
it reads the action's inputs and headers, calls the manager, and streams results
back onto the action's output ports. All policy (scope caps, timeouts,
validation, output processing, cleanup) lives in the manager.
"""

from __future__ import annotations

import a11
from a11.sdk.bash.manager import ShellManager, get_shell_manager
from a11.sdk.bash.schemas import SHELL_ID_HEADER, A11ShellExecuteParameters
from a11.status import Status, StatusCode


async def _close_output(node: a11.AsyncNode) -> None:
    """Flush and close a streaming output node.

    Closing the writer is enough to signal end-of-stream to a direct consumer;
    a ``put_null_final`` terminator is deliberately avoided because the LLM-tool
    runner collects every emitted fragment, and a trailing null (octet-stream)
    chunk would then reach output decoders that cannot deserialize it.
    """
    await node.drain_and_close()


def _required_shell_id(action: a11.Action) -> str:
    """Return the ``x-a11-shell-id`` header, or raise if it is absent."""
    shell_id = action.get_header(SHELL_ID_HEADER, decode=True)
    if not shell_id:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"The {SHELL_ID_HEADER} header is required.",
        ).to_exception()
    return shell_id


async def shell_start(
    action: a11.Action, manager: ShellManager | None = None
) -> None:
    """Start a shell in the action's scope and emit its id."""
    manager = manager or get_shell_manager()
    shell_id = await manager.start_shell(action.get_session())
    await action["shell_id"].put(shell_id, final=True)
    await action["shell_id"].drain_and_close()


async def shell_execute(
    action: a11.Action, manager: ShellManager | None = None
) -> None:
    """Run a command and stream its output lines onto ``output_lines``."""
    manager = manager or get_shell_manager()
    deadline = a11.get_deadline(action)

    def remaining() -> a11.Duration:
        # Bounds each input read so a caller that neither supplies nor closes an
        # optional input cannot hang the handler; the runner always closes
        # declared inputs, so present/absent inputs normally resolve at once.
        return max(deadline - a11.now(), a11.zero_duration())

    parameters = await action["parameters"].consume(
        A11ShellExecuteParameters, timeout=remaining(), allow_none=True
    )
    if parameters is None:
        parameters = A11ShellExecuteParameters()

    command = await action["command"].consume(
        str, timeout=remaining(), allow_none=True
    )
    if command is None:
        command = ""

    shell_id = action.get_header(SHELL_ID_HEADER, decode=True) or None

    try:
        async for line in manager.run_command(
            command, shell_id, parameters, action
        ):
            await action["output_lines"].put(line)
    finally:
        await _close_output(action["output_lines"])


async def shell_list(
    action: a11.Action, manager: ShellManager | None = None
) -> None:
    """Emit the ids of shells running in the action's scope."""
    manager = manager or get_shell_manager()
    for shell_id in manager.list_shells(action.get_session()):
        await action["shell_ids"].put(shell_id)
    await _close_output(action["shell_ids"])


async def shell_exit(
    action: a11.Action, manager: ShellManager | None = None
) -> None:
    """Terminate the shell named by the required ``x-a11-shell-id`` header."""
    manager = manager or get_shell_manager()
    await manager.exit_shell(_required_shell_id(action))
