# Copyright 2026 The A11 Authors.

import enum
from typing import Any, ClassVar, Literal

import a11
from pydantic import BaseModel, Field

from a11.data import serial_tags
from a11.sdk.llm import Interaction, InteractionAdapter, LlmHeaders, Role
from a11.status import Status, StatusCode


class ClaudeCodeHeaders(enum.StrEnum):
    """Headers this provider reads beyond the shared `LlmHeaders`."""

    SYSTEM_PRESET = "x-a11-claude-code-system-preset"


#: `ClaudeCodeHeaders.SYSTEM_PRESET` value that selects Claude Code's own
#: system prompt, with the interaction's system instructions appended.
CLAUDE_CODE_PRESET = "claude_code"


BuiltinTool = Literal[
    "Bash",
    "Edit",
    "Glob",
    "Grep",
    "NotebookEdit",
    "Read",
    "Task",
    "TodoWrite",
    "WebFetch",
    "WebSearch",
    "Write",
]

PermissionMode = Literal[
    "default",
    "acceptEdits",
    "plan",
    "dontAsk",
    "bypassPermissions",
]


class CreateSessionConfig(BaseModel):
    """Parameters for one Claude Code session.

    These map onto `claude_agent_sdk.ClaudeAgentOptions`. Registry-backed A11
    actions are surfaced separately through the `tools` input port and are
    always available to the model; the fields here govern Claude Code's own
    behaviour around them.
    """

    A11_SERIAL_TAG: ClassVar[str] = serial_tags.INTERACT_WITH_CLAUDE_CODE_CONFIG

    builtin_tools: bool | list[BuiltinTool] = Field(
        default=False,
        description=(
            "Claude Code's own tools. `false` offers the model A11 actions"
            " alone; `true` offers the full Claude Code toolset, which reads"
            " and writes the filesystem and runs commands; a list offers the"
            " named subset. Enabling a tool also permits it — a session driven"
            " through A11 answers no permission prompt — so name only what the"
            " turn should be able to do, and use `disallowed_tools` to carve"
            " back a command shape."
        ),
        exclude_if=lambda x: x is False,
    )
    permission_mode: PermissionMode | None = Field(
        default=None,
        description=(
            "Claude Code's permission mode, such as `plan` for a read-only"
            " session or `acceptEdits` for unattended file edits."
        ),
        exclude_if=lambda x: x is None,
    )
    disallowed_tools: list[str] = Field(
        default_factory=list,
        description=(
            "Tool names or scoped rules the model may never use, such as"
            " `Bash(rm *)`. A scoped rule is refused in every permission mode."
        ),
        exclude_if=lambda x: not x,
    )
    max_turns: int | None = Field(
        default=None,
        description="Maximum agent turns before the session stops.",
        exclude_if=lambda x: x is None,
    )
    max_budget_usd: float | None = Field(
        default=None,
        description="Stop the session once the estimated cost reaches this.",
        exclude_if=lambda x: x is None,
    )
    cwd: str | None = Field(
        default=None,
        description="Working directory for the session's tools.",
        exclude_if=lambda x: x is None,
    )
    add_dirs: list[str] = Field(
        default_factory=list,
        description="Extra directories the session's tools may reach.",
        exclude_if=lambda x: not x,
    )
    setting_sources: list[Literal["user", "project", "local"]] | None = Field(
        default=None,
        description=(
            "Which on-disk Claude Code settings to load. Omitted loads none,"
            " which keeps a session's behaviour independent of the host's"
            " configuration; include `project` to load CLAUDE.md."
        ),
        exclude_if=lambda x: x is None,
    )
    skills: list[str] | Literal["all"] | None = Field(
        default=None,
        description="Skills to make available, or `all`.",
        exclude_if=lambda x: x is None,
    )
    thinking: bool = Field(
        default=False,
        description=(
            "Enable adaptive thinking so the model decides when and how much"
            " internal reasoning to spend."
        ),
        exclude_if=lambda x: not x,
    )
    thinking_summaries: bool = Field(
        default=False,
        description="Stream summaries of the model's reasoning as it thinks.",
        exclude_if=lambda x: not x,
    )
    effort: Literal["low", "medium", "high", "xhigh", "max"] | None = Field(
        default=None,
        description="Overall thinking depth and token spend.",
        exclude_if=lambda x: x is None,
    )
    fallback_model: str | None = Field(
        default=None,
        description="Model to fall back to when the primary is unavailable.",
        exclude_if=lambda x: x is None,
    )
    resume: str | None = Field(
        default=None,
        description=(
            "Claude Code session id to continue. Also read from the newest"
            " assistant interaction's metadata when unset."
        ),
        exclude_if=lambda x: x is None,
    )
    fork_session: bool = Field(
        default=False,
        description="Branch a resumed session instead of extending it.",
        exclude_if=lambda x: not x,
    )
    cli_path: str | None = Field(
        default=None,
        description="Path to the `claude` executable.",
        exclude_if=lambda x: x is None,
    )


INTERACT_WITH_CLAUDE_CODE_SCHEMA = a11.ActionSchema(
    name="interact_with_claude_code",
    description=(
        "Drive a conversational turn through a Claude Code subscription rather"
        " than an API key."
    ),
    inputs={
        "interactions": a11.ActionPortSchema(
            "interactions",
            "application/json",
            typeinfo=Interaction,
            required=True,
        ),
        "tools": a11.ActionPortSchema(
            "tools",
            "application/json",
            typeinfo=dict,
            required=False,
        ),
        "config": a11.ActionPortSchema(
            "config",
            "application/json",
            unary=True,
            required=True,
            typeinfo=CreateSessionConfig,
        ),
    },
    outputs={
        "event_stream": a11.ActionPortSchema(
            "event_stream",
            "application/json",
            typeinfo=dict,
            required=False,
        ),
        "thoughts": a11.ActionPortSchema(
            "thoughts",
            "text/plain",
            required=False,
        ),
        "text_output": a11.ActionPortSchema(
            "text_output",
            "text/plain",
            required=False,
        ),
        "new_interactions": a11.ActionPortSchema(
            "new_interactions",
            "application/json",
            typeinfo=Interaction,
            required=True,
        ),
    },
    # No API key: the credential is the subscription the `claude` CLI holds.
    headers=a11.DEFAULT_ACTION_HEADERS
    | {
        LlmHeaders.MODEL: a11.ActionHeaderSchema(
            LlmHeaders.MODEL,
            "Model to run. Empty leaves the CLI's configured model in place.",
        ),
        LlmHeaders.ALLOWED_LLM_ACTIONS: a11.ActionHeaderSchema(
            LlmHeaders.ALLOWED_LLM_ACTIONS,
            "The allowed action (tool) name patterns, comma-separated.",
        ),
        ClaudeCodeHeaders.SYSTEM_PRESET: a11.ActionHeaderSchema(
            ClaudeCodeHeaders.SYSTEM_PRESET,
            f"Set to `{CLAUDE_CODE_PRESET}` to keep Claude Code's own system"
            " prompt and append the interaction's system instructions to it.",
        ),
    },
)


def _get_message_from_interaction(interaction: Interaction) -> dict[str, Any]:
    if not interaction.content or len(interaction.content) > 1:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Expected exactly one content item.",
        ).to_exception()

    message = a11.from_chunk(interaction.content[0], obj_type=dict)
    return message


class ClaudeCodeInteractionAdapter(InteractionAdapter):
    def __init__(self, interaction: Interaction):
        self._interaction = interaction

    @staticmethod
    def make_text_message_interaction(
        text: str, system_prompt: str, role: Role
    ) -> Interaction:
        if role == Role.SYSTEM:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Claude Code does not support a system role.",
            ).to_exception()

        role_str = "user"
        if role == role.ASSISTANT:
            role_str = "assistant"

        system_instructions = (
            [a11.to_chunk(system_prompt)] if system_prompt else []
        )
        return Interaction(
            role=Role.USER,
            content=[
                a11.to_chunk(
                    {
                        "role": role_str,
                        "content": [{"type": "text", "text": text}],
                    }
                )
            ],
            system_instructions=system_instructions,
        )

    def get_message_text(self) -> str:
        message = _get_message_from_interaction(self._interaction)
        return "".join(
            content["text"]
            for content in message["content"]
            if "text" in content
        )


__all__ = [
    "CLAUDE_CODE_PRESET",
    "ClaudeCodeHeaders",
    "ClaudeCodeInteractionAdapter",
    "CreateSessionConfig",
    "INTERACT_WITH_CLAUDE_CODE_SCHEMA",
]
