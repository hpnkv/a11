# Copyright 2026 The A11 Authors.

from typing import Any, ClassVar, Literal

import a11
from pydantic import BaseModel, Field

from a11.data import serial_tags
from a11.sdk.llm import Interaction, InteractionAdapter, LlmHeaders, Role
from a11.status import Status, StatusCode

# How the handler carries conversation state across turns of the (stateful)
# Interactions API.
#   * "last-id"      — only ever send the newest input plus a
#                      `previous_interaction_id`; rely on the server-stored
#                      transcript. Cheapest, but breaks if the server does not
#                      retain the interaction (e.g. storage disabled).
#   * "full-history" — never chain by id; replay the entire conversation as a
#                      list of steps on every turn. Robust but larger requests.
#   * "auto"         — try "last-id" and, if the id cannot be resolved, fall
#                      back to "full-history" for the rest of the session
#                      (logged, no confirmation required).
StateMode = Literal["full-history", "last-id", "auto"]


class CreateInteractionConfig(BaseModel):
    """Parameters for starting a Gemini interaction.

    These are the interaction-scoped knobs the Interactions API expects on
    every `interactions.create` call (Gemini does not carry them across turns),
    plus toggles for the model's built-in, server-side tools. Registry-backed
    A11 actions are surfaced separately through the `tools` input port.
    """

    A11_SERIAL_TAG: ClassVar[str] = serial_tags.INTERACT_WITH_GEMINI_CONFIG

    max_output_tokens: int = Field(
        default=10240,
        description="Maximum number of tokens to generate per step.",
    )
    state_mode: StateMode = Field(
        default="auto",
        description=(
            "How to carry conversation state across turns: resume by"
            " `previous_interaction_id` (`last-id`), replay the whole"
            " transcript every turn (`full-history`), or try the former and"
            " fall back to the latter (`auto`)."
        ),
        exclude_if=lambda x: x == "auto",
    )
    thinking_level: Literal["minimal", "low", "medium", "high"] | None = Field(
        default=None,
        description="How much internal reasoning the model may spend.",
        exclude_if=lambda x: x is None,
    )
    thinking_summaries: bool = Field(
        default=False,
        description="Stream summaries of the model's reasoning as it thinks.",
        exclude_if=lambda x: not x,
    )
    google_search: bool = Field(
        default=False,
        description="Enable the built-in Google Search grounding tool.",
        exclude_if=lambda x: not x,
    )
    code_execution: bool = Field(
        default=False,
        description="Enable the built-in code execution tool.",
        exclude_if=lambda x: not x,
    )
    url_context: bool = Field(
        default=False,
        description="Enable the built-in URL context tool.",
        exclude_if=lambda x: not x,
    )


DEFAULT_MODEL = "gemini-3.5-flash"


INTERACT_WITH_GEMINI_SCHEMA = a11.ActionSchema(
    name="interact_with_gemini",
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
            typeinfo=CreateInteractionConfig,
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
            LlmHeaders.API_KEY, "Gemini API key."
        ),
        LlmHeaders.ALLOWED_LLM_ACTIONS: a11.ActionHeaderSchema(
            LlmHeaders.ALLOWED_LLM_ACTIONS,
            "The allowed LLM action (tool) name patterns, comma-separated.",
        ),
    },
)


def make_text_message_interaction(
    text: str, system_prompt: str = "", role: Role = Role.USER
) -> Interaction:
    """Build an `Interaction` carrying a single text part.

    The content is the backend-neutral ``{"role", "content": [text part]}``
    envelope, which both backends read without a normalisation round-trip, so
    plain text messages stay portable across a mid-conversation model switch.
    """
    if role == Role.SYSTEM:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Gemini does not support a system role as message content.",
        ).to_exception()

    role_str = "model" if role == Role.ASSISTANT else "user"
    system_instructions = [a11.to_chunk(system_prompt)] if system_prompt else []
    return Interaction(
        role=role,
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


def get_interaction_content(interaction: Interaction) -> dict[str, Any]:
    if not interaction.content or len(interaction.content) > 1:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Expected exactly one content item.",
        ).to_exception()

    return a11.from_chunk(interaction.content[0], obj_type=dict)


def get_output_text_from_interaction(interaction: Interaction) -> str:
    """Extract the model's text from an assistant interaction's content.

    An assistant interaction stores the completed Gemini interaction dump; the
    text lives in `output_text` (when present) or in the `model_output` step's
    text content parts.
    """
    content = get_interaction_content(interaction)

    if content.get("output_text"):
        return content["output_text"]

    if isinstance(content.get("content"), str):
        return content["content"]
    if isinstance(content.get("content"), list):
        return "".join(
            part.get("text", "")
            for part in content["content"]
            if isinstance(part, dict) and part.get("type") == "text"
        )

    texts: list[str] = []
    for step in content.get("steps") or []:
        if step.get("type") != "model_output":
            continue
        for part in step.get("content") or []:
            if part.get("type") == "text":
                texts.append(part.get("text", ""))
    return "".join(texts)


class GeminiInteractionAdapter(InteractionAdapter):
    def __init__(self, interaction: Interaction):
        self._interaction = interaction

    @staticmethod
    def make_text_message_interaction(
        text: str, system_prompt: str = "", role: Role = Role.USER
    ) -> Interaction:
        return make_text_message_interaction(text, system_prompt, role)

    def get_message_text(self) -> str:
        return get_output_text_from_interaction(self._interaction)
