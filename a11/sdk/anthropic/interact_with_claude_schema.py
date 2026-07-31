# Copyright 2026 The A11 Authors.

from typing import Any, Literal

import a11
from pydantic import BaseModel, Field

from a11.sdk.llm import Interaction, InteractionAdapter, LlmHeaders, Role
from a11.status import Status, StatusCode


class CreateMessageConfig(BaseModel):
    """Parameters for creating a Claude message.

    These are the request-scoped knobs the Messages API expects on every
    `messages.create` call (Claude does not carry them across turns), plus
    toggles for the model's built-in, server-side tools. Registry-backed A11
    actions are surfaced separately through the `tools` input port.
    """

    max_tokens: int = Field(
        default=10240,
        description="Maximum number of tokens to generate.",
    )
    thinking: bool = Field(
        default=False,
        description=(
            "Enable adaptive thinking so the model decides when and how much"
            " internal reasoning to spend. Unsupported alongside tools and on"
            " Haiku models."
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
        description=(
            "Overall thinking depth and token spend. Only honoured on models"
            " that support the effort parameter."
        ),
        exclude_if=lambda x: x is None,
    )
    web_search: bool = Field(
        default=False,
        description="Enable the built-in web search tool.",
        exclude_if=lambda x: not x,
    )
    web_fetch: bool = Field(
        default=False,
        description="Enable the built-in web fetch tool.",
        exclude_if=lambda x: not x,
    )
    code_execution: bool = Field(
        default=False,
        description="Enable the built-in code execution tool.",
        exclude_if=lambda x: not x,
    )


DEFAULT_MODEL = "claude-sonnet-4-6"


INTERACT_WITH_CLAUDE_SCHEMA = a11.ActionSchema(
    name="interact_with_claude",
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
            typeinfo=CreateMessageConfig,
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
    headers=a11.DEFAULT_ACTION_HEADERS
    | {
        LlmHeaders.API_KEY: a11.ActionHeaderSchema(
            LlmHeaders.API_KEY, "Anthropic API key."
        ),
        LlmHeaders.ALLOWED_LLM_ACTIONS: a11.ActionHeaderSchema(
            LlmHeaders.ALLOWED_LLM_ACTIONS,
            "The allowed action (tool) name patterns, comma-separated.",
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


class ClaudeInteractionAdapter(InteractionAdapter):
    def __init__(self, interaction: Interaction):
        self._interaction = interaction

    @staticmethod
    def make_text_message_interaction(
        text: str, system_prompt: str, role: Role
    ) -> Interaction:
        if role == Role.SYSTEM:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Claude does not support a system role.",
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
