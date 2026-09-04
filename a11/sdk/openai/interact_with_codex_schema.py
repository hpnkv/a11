# Copyright 2026 The A11 Authors.

"""Describe one non-interactive Codex CLI session turn."""

from typing import Any, ClassVar, Literal

import a11
from pydantic import BaseModel, Field

from a11.data import serial_tags
from a11.sdk.llm import Interaction, InteractionAdapter, LlmHeaders, Role
from a11.status import Status, StatusCode


class CreateCodexSessionConfig(BaseModel):
    """Options for a `codex exec --json` session."""

    A11_SERIAL_TAG: ClassVar[str] = serial_tags.INTERACT_WITH_CODEX_CONFIG

    cwd: str | None = Field(
        default=None,
        description="Working directory exposed as Codex's primary workspace.",
        exclude_if=lambda value: value is None,
    )
    add_dirs: list[str] = Field(
        default_factory=list,
        description="Additional writable directories.",
        exclude_if=lambda value: not value,
    )
    sandbox: Literal["read-only", "workspace-write", "danger-full-access"] = (
        Field(
            default="read-only",
            description="Sandbox policy for Codex's built-in tools.",
        )
    )
    profile: str | None = Field(
        default=None,
        description="Codex configuration profile.",
        exclude_if=lambda value: value is None,
    )
    reasoning_effort: Literal["low", "medium", "high", "xhigh"] | None = Field(
        default=None,
        description="Model reasoning effort.",
        exclude_if=lambda value: value is None,
    )
    output_schema: dict[str, Any] | None = Field(
        default=None,
        description="JSON Schema for the final response.",
        exclude_if=lambda value: value is None,
    )
    resume: str | None = Field(
        default=None,
        description=(
            "Codex thread id to resume. The latest Codex interaction's"
            " metadata supplies it when omitted."
        ),
        exclude_if=lambda value: value is None,
    )
    ephemeral: bool = Field(
        default=False,
        description="Do not persist the Codex thread on disk.",
        exclude_if=lambda value: not value,
    )
    skip_git_repo_check: bool = Field(
        default=False,
        description="Allow Codex to run outside a Git repository.",
        exclude_if=lambda value: not value,
    )
    ignore_user_config: bool = Field(
        default=False,
        description=(
            "Ignore the user's config.toml while retaining authentication."
        ),
        exclude_if=lambda value: not value,
    )
    ignore_rules: bool = Field(
        default=False,
        description="Do not load user or project execpolicy rule files.",
        exclude_if=lambda value: not value,
    )
    config_overrides: dict[str, Any] = Field(
        default_factory=dict,
        description="Additional Codex `-c key=value` settings.",
        exclude_if=lambda value: not value,
    )
    cli_path: str = Field(
        default="codex",
        description="Path to the Codex CLI executable.",
    )


DEFAULT_MODEL = ""
THREAD_ID_METADATA_KEY = "thread_id"


INTERACT_WITH_CODEX_SCHEMA = a11.ActionSchema(
    name="interact_with_codex",
    description="Run a conversational turn through the Codex CLI.",
    inputs={
        "interactions": a11.ActionPortSchema(
            "interactions",
            "application/json",
            typeinfo=Interaction,
            required=True,
        ),
        "tools": a11.ActionPortSchema(
            "tools", "application/json", typeinfo=dict, required=False
        ),
        "config": a11.ActionPortSchema(
            "config",
            "application/json",
            typeinfo=CreateCodexSessionConfig,
            unary=True,
            required=True,
        ),
    },
    outputs={
        "event_stream": a11.ActionPortSchema(
            "event_stream", "application/json", typeinfo=dict, required=False
        ),
        "thoughts": a11.ActionPortSchema(
            "thoughts", "text/plain", required=False
        ),
        "text_output": a11.ActionPortSchema(
            "text_output", "text/plain", required=False
        ),
        "new_interactions": a11.ActionPortSchema(
            "new_interactions",
            "application/json",
            typeinfo=Interaction,
            required=True,
        ),
    },
    headers=a11.DEFAULT_ACTION_HEADERS
    | {
        LlmHeaders.API_KEY: a11.ActionHeaderSchema(
            LlmHeaders.API_KEY,
            "Codex API key override; otherwise the CLI's login is used.",
        ),
        LlmHeaders.MODEL: a11.ActionHeaderSchema(
            LlmHeaders.MODEL,
            "Model to run. Empty keeps the CLI's configured model.",
        ),
        LlmHeaders.ALLOWED_LLM_ACTIONS: a11.ActionHeaderSchema(
            LlmHeaders.ALLOWED_LLM_ACTIONS,
            "Allowed action (tool) name patterns, comma-separated.",
        ),
    },
)


def make_text_message_interaction(
    text: str, system_prompt: str = "", role: Role = Role.USER
) -> Interaction:
    """Build a portable text interaction for a Codex session."""
    if role == Role.SYSTEM:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Codex takes system instructions separately from messages.",
        ).to_exception()
    interaction = Interaction(
        role=role,
        content=[
            a11.to_chunk(
                {
                    "role": "assistant" if role == Role.ASSISTANT else "user",
                    "content": text,
                }
            )
        ],
    )
    if system_prompt:
        interaction.system_instructions = [a11.to_chunk(system_prompt)]
    return interaction


def get_output_text_from_interaction(interaction: Interaction) -> str:
    """Extract the text stored in a Codex interaction."""
    if len(interaction.content) != 1:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Expected exactly one content item.",
        ).to_exception()
    value = a11.from_chunk(interaction.content[0])
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        content = value.get("content", "")
        return content if isinstance(content, str) else ""
    return ""


class CodexInteractionAdapter(InteractionAdapter):
    """Idiomatic helpers for Codex-native interactions."""

    def __init__(self, interaction: Interaction):
        self._interaction = interaction

    @staticmethod
    def make_text_message_interaction(
        text: str, system_prompt: str = "", role: Role = Role.USER
    ) -> Interaction:
        return make_text_message_interaction(text, system_prompt, role)

    def get_message_text(self) -> str:
        return get_output_text_from_interaction(self._interaction)
