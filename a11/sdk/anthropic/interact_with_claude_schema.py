# Copyright 2026 The A11 Authors.

from typing import Any

import a11
from pydantic import BaseModel, Field

from a11.sdk.llm import Interaction, InteractionAdapter, LlmHeaders, Role
from a11.status import Status, StatusCode


class CreateMessageConfig(BaseModel):
    max_tokens: int = Field(
        default=10240,
        description="Maximum number of tokens to generate",
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
