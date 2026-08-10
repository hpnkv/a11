# Copyright 2026 The A11 Authors.

"""Handlers for the four Bash-tool Actions.

Each handler is a thin adapter over :class:`~a11.sdk.bash.manager.ShellManager`:
it reads the action's inputs and headers, calls the manager, and streams results
back onto the action's output ports. All policy (scope caps, timeouts,
validation, output processing, cleanup) lives in the manager.

Each also narrates its run on the ``user_facing_log`` port, for whoever is
showing the tool call to a person: the lifecycle handlers name the shell they
acted on, and ``shell_execute`` names the command and how much came back, which
is what a reader watching a command run is actually looking at. That port is not
part of the result the model is given; see
:data:`a11.sdk.llm.USER_FACING_LOG_PORT`.
"""

from __future__ import annotations

from absl import logging

import a11
from a11.sdk.bash.manager import ShellManager, get_shell_manager
from a11.sdk.bash.schemas import SHELL_ID_HEADER, A11ShellExecuteParameters
from a11.sdk.llm import USER_FACING_LOG_PORT
from a11.status import Status, StatusCode

#: How much of a command is quoted in a run log's summary line.
_COMMAND_SUMMARY_LIMIT = 72
#: How many output lines a run log's detail reproduces, at each end.
_LOG_HEAD_LINES = 12
_LOG_TAIL_LINES = 4


async def _write_log(action: a11.Action, log: str) -> None:
    """Narrate this run on the user-facing log port, then close it.

    The log is one whole value, so it is written as one *final* fragment: its
    readers -- the LLM tool runner, a UI showing the run -- learn from the
    marker that they have the complete narration rather than a prefix. (Ending
    the stream no longer depends on it: a drained writer tees a closure marker,
    so a reader across a wire ends either way.) An empty log is terminated the
    only way an empty stream can be, with a null final; both readers skip null
    chunks.

    Best effort: a run log is worth nothing next to the tool's actual result, so
    a port that will not take it (a caller that aborted it, say) does not fail
    the call.
    """
    node = action[USER_FACING_LOG_PORT]
    try:
        if log:
            await node.put(log, final=True)
        else:
            await node.put_null_final()
        await node.drain_and_close()
    except Exception:
        logging.warning("failed to write the run log", exc_info=True)


def _quote_command(command: str) -> str:
    """One line of a command, short enough for a summary line."""
    first = command.strip().splitlines()[0] if command.strip() else ""
    if len(first) > _COMMAND_SUMMARY_LIMIT:
        first = first[: _COMMAND_SUMMARY_LIMIT - 1] + "…"
    elif command.strip().count("\n"):
        first += " …"
    return first


def _output_excerpt(lines: list[str]) -> str:
    """A fenced excerpt of the output: the head, and the tail if it was long."""
    if not lines:
        return ""
    if len(lines) <= _LOG_HEAD_LINES + _LOG_TAIL_LINES:
        shown = list(lines)
    else:
        elided = len(lines) - _LOG_HEAD_LINES - _LOG_TAIL_LINES
        shown = [
            *lines[:_LOG_HEAD_LINES],
            f"… {elided} more line{'s' if elided != 1 else ''} …",
            *lines[-_LOG_TAIL_LINES:],
        ]
    body = "\n".join(line.rstrip("\n") for line in shown)
    return f"```\n{body}\n```"


async def _write_all_final(node: a11.AsyncNode, values: list[str]) -> None:
    """Write a known-complete sequence, marking its last value final.

    For a producer that has its whole sequence in hand. Finality is what tells
    a whole-value consumer that the sequence ran to its end rather than being
    cut short; a ``put_null_final`` terminator would say the same thing, but it
    is deliberately avoided on these ports -- the LLM-tool runner collects
    every emitted fragment, and a trailing null (octet-stream) chunk would then
    reach output decoders that cannot deserialize it.
    """
    for index, value in enumerate(values):
        await node.put(value, final=index == len(values) - 1)
    await _close_output(node)


async def _close_output(node: a11.AsyncNode) -> None:
    """Flush and close a streaming output node with no final marker of its own.

    For the two cases that have no last fragment to mark: an empty sequence,
    and a run that failed part way and so did not reach an end worth
    declaring. Closing still ends a reader on either side of a wire — a drained
    writer tees a closure marker to its peers.
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
    in_session = action.get_session() is not None
    scope = "this session" if in_session else "the global scope"
    await _write_log(
        action,
        f"Started a shell.\n\nShell `{shell_id}`, scoped to {scope}. It keeps"
        " its working directory and environment until it is exited.",
    )


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

    lines: list[str] = []
    failure: Exception | None = None
    output = action["output_lines"]
    # Output stays a stream, one line behind: a line is written when the next
    # one arrives, so the last line is still in hand at the end and can carry
    # the final marker that says the command ran to completion. Being a line
    # behind costs nothing visible -- this port is harvested once the action
    # finishes, and what a person watches live is the user-facing log.
    held: str | None = None
    try:
        async for line in manager.run_command(
            command, shell_id, parameters, action
        ):
            lines.append(line)
            if held is not None:
                await output.put(held)
            held = line
    except Exception as error:
        # Held rather than swallowed: the log is written first (a call that died
        # is exactly the one worth narrating) and the failure then re-raised so
        # the action still fails.
        failure = error
    finally:
        if held is not None:
            # Only a run that finished claims finality; a failed one stops
            # where it stopped.
            await output.put(held, final=failure is None)
        await _close_output(output)

    count = len(lines)
    if failure is not None:
        summary = f"Error: {failure}"
    else:
        summary = (
            f"`{_quote_command(command)}` —"
            f" {count} line{'s' if count != 1 else ''} of output."
        )
    detail = "\n\n".join(
        part
        for part in (
            f"```sh\n{command.strip()}\n```" if command.strip() else "",
            _output_excerpt(lines),
        )
        if part
    )
    await _write_log(action, f"{summary}\n\n{detail}" if detail else summary)

    if failure is not None:
        raise failure


async def shell_list(
    action: a11.Action, manager: ShellManager | None = None
) -> None:
    """Emit the ids of shells running in the action's scope."""
    manager = manager or get_shell_manager()
    shell_ids = manager.list_shells(action.get_session())
    await _write_all_final(action["shell_ids"], list(shell_ids))

    count = len(shell_ids)
    summary = f"Listed running shells — {count} of them."
    detail = "\n".join(f"- `{shell_id}`" for shell_id in shell_ids)
    await _write_log(action, f"{summary}\n\n{detail}" if detail else summary)


async def shell_exit(
    action: a11.Action, manager: ShellManager | None = None
) -> None:
    """Terminate the shell named by the required ``x-a11-shell-id`` header."""
    manager = manager or get_shell_manager()
    shell_id = _required_shell_id(action)
    await manager.exit_shell(shell_id)
    await _write_log(
        action,
        f"Exited shell `{shell_id}`.\n\nIts process is gone; anything it held"
        " (working directory, environment, background jobs) is released.",
    )
