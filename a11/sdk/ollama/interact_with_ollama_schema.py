# Copyright 2026 The A11 Authors.

from typing import Any, ClassVar, Literal

import a11
from pydantic import BaseModel, Field

from a11.data import serial_tags
from a11.sdk.llm import Interaction, InteractionAdapter, LlmHeaders, Role
from a11.status import Status, StatusCode


class CreateChatConfig(BaseModel):
    """Parameters for a single Ollama chat turn.

    Ollama's `chat` API is stateless — nothing is carried across turns — so the
    whole transcript is replayed on every request and these knobs are re-sent
    each time. Sampling parameters map onto Ollama's `options` bag; Ollama has
    no built-in server-side tools, so registry-backed A11 actions surfaced
    through the `tools` input port are the only tools available.
    """

    A11_SERIAL_TAG: ClassVar[str] = serial_tags.INTERACT_WITH_OLLAMA_CONFIG

    num_predict: int = Field(
        default=-1,
        description=(
            "Maximum number of tokens to generate. -1 lets the model run until"
            " it stops on its own (Ollama `options.num_predict`)."
        ),
    )
    think: bool | Literal["low", "medium", "high"] | None = Field(
        default=None,
        description=(
            "Enable the model's thinking, optionally at a given effort level."
            " Only honoured by models that support it."
        ),
        exclude_if=lambda x: x is None,
    )
    temperature: float | None = Field(
        default=None,
        description="Sampling temperature (Ollama `options.temperature`).",
        exclude_if=lambda x: x is None,
    )
    top_p: float | None = Field(
        default=None,
        description="Nucleus-sampling probability (Ollama `options.top_p`).",
        exclude_if=lambda x: x is None,
    )
    top_k: int | None = Field(
        default=None,
        description="Top-k sampling cutoff (Ollama `options.top_k`).",
        exclude_if=lambda x: x is None,
    )
    seed: int | None = Field(
        default=None,
        description="Sampling seed for reproducible output (Ollama"
        " `options.seed`).",
        exclude_if=lambda x: x is None,
    )
    keep_alive: str | float | None = Field(
        default=None,
        description=(
            "How long to keep the model loaded in memory after the request"
            " (e.g. `5m`, or seconds as a number)."
        ),
        exclude_if=lambda x: x is None,
    )
    json_output: bool = Field(
        default=False,
        description="Constrain the model to emit valid JSON (Ollama"
        " `format=\"json\"`).",
        exclude_if=lambda x: not x,
    )


DEFAULT_MODEL = "llama3.2"


INTERACT_WITH_OLLAMA_SCHEMA = a11.ActionSchema(
    name="interact_with_ollama",
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
            typeinfo=CreateChatConfig,
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
            LlmHeaders.API_KEY,
            "Ollama API key (only needed for the hosted service).",
        ),
        LlmHeaders.BASE_URL: a11.ActionHeaderSchema(
            LlmHeaders.BASE_URL,
            "Ollama host URL; defaults to OLLAMA_HOST or the local server.",
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
    """Build an `Interaction` carrying a single text message.

    The content is Ollama's native ``{"role", "content"}`` message shape, which
    the handler reads without a normalisation round-trip.
    """
    if role == Role.SYSTEM:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Ollama does not support a system role as message content.",
        ).to_exception()

    role_str = "assistant" if role == Role.ASSISTANT else "user"
    system_instructions = [a11.to_chunk(system_prompt)] if system_prompt else []
    return Interaction(
        role=role,
        content=[
            a11.to_chunk(
                {
                    "role": role_str,
                    "content": text,
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

    An assistant interaction stores the reconstructed Ollama message; its text
    lives in the `content` field.
    """
    content = get_interaction_content(interaction)
    text = content.get("content")
    return text if isinstance(text, str) else ""


class OllamaInteractionAdapter(InteractionAdapter):
    def __init__(self, interaction: Interaction):
        self._interaction = interaction

    @staticmethod
    def make_text_message_interaction(
        text: str, system_prompt: str = "", role: Role = Role.USER
    ) -> Interaction:
        return make_text_message_interaction(text, system_prompt, role)

    def get_message_text(self) -> str:
        return get_output_text_from_interaction(self._interaction)
