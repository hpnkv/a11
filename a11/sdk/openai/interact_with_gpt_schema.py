# Copyright 2026 The A11 Authors.

"""Describe one streamed interaction with the OpenAI API."""

from typing import Any, ClassVar, Literal

import a11
from pydantic import BaseModel, Field

from a11.data import serial_tags
from a11.sdk.llm import Interaction, InteractionAdapter, LlmHeaders, Role
from a11.status import Status, StatusCode


class CreateChatCompletionConfig(BaseModel):
    """Parameters for one OpenAI chat completion."""

    A11_SERIAL_TAG: ClassVar[str] = serial_tags.INTERACT_WITH_GPT_CONFIG

    max_completion_tokens: int | None = Field(
        default=None,
        description="Maximum generated tokens, including reasoning tokens.",
        exclude_if=lambda value: value is None,
    )
    temperature: float | None = Field(
        default=None,
        description="Sampling temperature.",
        exclude_if=lambda value: value is None,
    )
    top_p: float | None = Field(
        default=None,
        description="Nucleus-sampling probability.",
        exclude_if=lambda value: value is None,
    )
    presence_penalty: float | None = Field(
        default=None,
        description="Penalty applied when a token already appeared.",
        exclude_if=lambda value: value is None,
    )
    frequency_penalty: float | None = Field(
        default=None,
        description="Penalty scaled by a token's prior frequency.",
        exclude_if=lambda value: value is None,
    )
    seed: int | None = Field(
        default=None,
        description="Best-effort sampling seed.",
        exclude_if=lambda value: value is None,
    )
    stop: list[str] = Field(
        default_factory=list,
        description="Strings that end generation.",
        exclude_if=lambda value: not value,
    )
    reasoning_effort: (
        Literal["none", "minimal", "low", "medium", "high", "xhigh"] | None
    ) = Field(
        default=None,
        description="Reasoning effort for models that support it.",
        exclude_if=lambda value: value is None,
    )
    json_output: bool = Field(
        default=False,
        description="Constrain the response to a JSON object.",
        exclude_if=lambda value: not value,
    )
    json_schema: dict[str, Any] | None = Field(
        default=None,
        description="JSON Schema for a structured response.",
        exclude_if=lambda value: value is None,
    )
    service_tier: Literal["auto", "default", "flex", "priority"] | None = Field(
        default=None,
        description="OpenAI processing tier.",
        exclude_if=lambda value: value is None,
    )
    extra_body: dict[str, Any] = Field(
        default_factory=dict,
        description="Additional OpenAI request fields.",
        exclude_if=lambda value: not value,
    )


DEFAULT_MODEL = "gpt-6-astra"


INTERACT_WITH_GPT_SCHEMA = a11.ActionSchema(
    name="interact_with_gpt",
    description="Run a streamed conversational turn through the OpenAI API.",
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
            typeinfo=CreateChatCompletionConfig,
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
            LlmHeaders.API_KEY, "OpenAI API key; defaults to OPENAI_API_KEY."
        ),
        LlmHeaders.BASE_URL: a11.ActionHeaderSchema(
            LlmHeaders.BASE_URL, "OpenAI-compatible API root override."
        ),
        LlmHeaders.MODEL: a11.ActionHeaderSchema(
            LlmHeaders.MODEL, f"Model to run; defaults to {DEFAULT_MODEL}."
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
    """Build an interaction carrying one OpenAI-compatible text message."""
    if role == Role.SYSTEM:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                "OpenAI takes system instructions separately from messages."
            ),
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


def get_interaction_content(interaction: Interaction) -> dict[str, Any]:
    """Decode an interaction's single OpenAI message."""
    if len(interaction.content) != 1:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Expected exactly one content item.",
        ).to_exception()
    return a11.from_chunk(interaction.content[0], obj_type=dict)


def get_output_text_from_interaction(interaction: Interaction) -> str:
    """Extract assistant text from an OpenAI interaction."""
    content = get_interaction_content(interaction).get("content")
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "".join(
            str(part.get("text", ""))
            for part in content
            if isinstance(part, dict)
            and part.get("type") in {"text", "output_text"}
        )
    return ""


class GptInteractionAdapter(InteractionAdapter):
    """Idiomatic helpers for OpenAI-native interactions."""

    def __init__(self, interaction: Interaction):
        self._interaction = interaction

    @staticmethod
    def make_text_message_interaction(
        text: str, system_prompt: str = "", role: Role = Role.USER
    ) -> Interaction:
        return make_text_message_interaction(text, system_prompt, role)

    def get_message_text(self) -> str:
        return get_output_text_from_interaction(self._interaction)
