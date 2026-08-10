# Copyright 2026 The A11 Authors.

"""Schemas for the Bash tool's four Actions, kept separate from the handlers.

The four Actions form a small stateful protocol an LLM can drive as tools:

* ``shell_start`` opens a shell and returns its id;
* ``shell_execute`` runs a command (in a named shell, or a throwaway one);
* ``shell_list`` lists the shells running in the caller's scope;
* ``shell_exit`` closes a shell.

Shells are scoped to the ``Session`` they are started in, or to a process-global
scope when there is none; the per-scope caps are documented on
``SHELL_START_SCHEMA`` because that is where the LLM meets them.
"""

from __future__ import annotations

from typing import ClassVar

from pydantic import BaseModel, Field, field_validator

import a11
from a11.sdk.llm import USER_FACING_LOG_PORT

#: Header naming the shell an action should act on.
SHELL_ID_HEADER = "x-a11-shell-id"


def _user_facing_log_port() -> a11.ActionPortSchema:
    """The port each shell Action narrates its run on, for the user's eyes.

    Every one of the four declares it, so a UI driving these tools can show what
    a call did -- which shell, which command, what came back -- without parsing
    the result the model was given. The LLM tool runner drains this port and
    keeps it out of that result; see
    [USER_FACING_LOG_PORT][a11.sdk.llm.USER_FACING_LOG_PORT].
    """
    return a11.ActionPortSchema(
        USER_FACING_LOG_PORT,
        "text/plain",
        description=(
            "Narration of this call for the person watching: first line a"
            " one-sentence summary, the rest markdown detail. Not part of the"
            " tool result."
        ),
        required=False,
    )


class A11ShellExecuteParameters(BaseModel):
    """Tunables for a single ``shell_execute`` call."""

    #: Absolute ceiling for :attr:`timeout_seconds`, regardless of request.
    MAX: ClassVar[int] = 600
    #: Default hard timeout applied when no parameters are supplied.
    DEFAULT: ClassVar[int] = 30

    timeout_seconds: int = Field(
        default=30,
        description=(
            "Hard timeout, in seconds, for the command to run before it is"
            " terminated. Defaults to 30; capped at an absolute maximum of 600."
        ),
    )

    @field_validator("timeout_seconds")
    @classmethod
    def _bound_timeout(cls, value: int) -> int:
        if value <= 0:
            raise ValueError("timeout_seconds must be positive.")
        return min(value, 600)


SHELL_START_SCHEMA = a11.ActionSchema(
    name="shell_start",
    description=(
        "Start a new persistent shell and return its id, which later"
        " shell_execute calls can target via the x-a11-shell-id header. The"
        " shell keeps its state (working directory, environment variables,"
        " shell functions) across commands until it is exited. A shell is"
        " scoped to the current Session, or globally scoped when started"
        " outside one. Each Session may keep at most 4 running shells and the"
        " global scope at most 10; starting one beyond the limit fails with"
        " RESOURCE_EXHAUSTED. Exit shells you no longer need with shell_exit."
    ),
    outputs={
        "shell_id": a11.ActionPortSchema(
            "shell_id",
            "text/plain",
            description="Id of the newly started shell.",
            unary=True,
            required=True,
        ),
        USER_FACING_LOG_PORT: _user_facing_log_port(),
    },
)


SHELL_EXECUTE_SCHEMA = a11.ActionSchema(
    name="shell_execute",
    description=(
        "Run a shell command and stream back its output lines (stdout and"
        " stderr, interleaved). Target a shell started with shell_start via the"
        " x-a11-shell-id header to reuse its state; omit the header to run the"
        " command in a throwaway shell that is discarded afterwards. The"
        " command is terminated if it exceeds the timeout or the action's"
        " deadline."
    ),
    inputs={
        "command": a11.ActionPortSchema(
            "command",
            "text/plain",
            description="The command to execute.",
            typeinfo=str,
            unary=True,
            required=False,
        ),
        "parameters": a11.ActionPortSchema(
            "parameters",
            "application/json",
            description="Execution parameters; defaults are used if omitted.",
            typeinfo=A11ShellExecuteParameters,
            unary=True,
            required=False,
        ),
    },
    outputs={
        "output_lines": a11.ActionPortSchema(
            "output_lines",
            "text/plain",
            description="Output lines produced by the command, if any.",
            required=False,
        ),
        USER_FACING_LOG_PORT: _user_facing_log_port(),
    },
    headers=a11.DEFAULT_ACTION_HEADERS
    | {
        SHELL_ID_HEADER: a11.ActionHeaderSchema(
            SHELL_ID_HEADER,
            "Id of the shell to run the command in. If absent, a transient"
            " shell is started for this command and terminated when it"
            " completes.",
        ),
    },
)


SHELL_LIST_SCHEMA = a11.ActionSchema(
    name="shell_list",
    description=(
        "List the ids of shells currently running in the caller's scope (the"
        " current Session, or the global scope when outside one)."
    ),
    outputs={
        "shell_ids": a11.ActionPortSchema(
            "shell_ids",
            "text/plain",
            description="Id of each running shell in the caller's scope.",
            required=False,
        ),
        USER_FACING_LOG_PORT: _user_facing_log_port(),
    },
)


SHELL_EXIT_SCHEMA = a11.ActionSchema(
    name="shell_exit",
    description=(
        "Terminate a shell started with shell_start, releasing its resources."
        " Fails with NOT_FOUND if no shell with the given id is running."
    ),
    outputs={
        USER_FACING_LOG_PORT: _user_facing_log_port(),
    },
    headers={
        SHELL_ID_HEADER: a11.ActionHeaderSchema(
            SHELL_ID_HEADER,
            "Id of the shell to terminate (required).",
        ),
    },
)
