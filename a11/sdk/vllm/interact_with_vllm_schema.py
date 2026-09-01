# Copyright 2026 The A11 Authors.

"""Describe one vLLM chat turn: its request config and its action schema."""

from typing import Any, ClassVar

import a11
from pydantic import BaseModel, Field

from a11.data import serial_tags
from a11.sdk.llm import Interaction, InteractionAdapter, LlmHeaders, Role
from a11.status import Status, StatusCode


class CreateChatCompletionConfig(BaseModel):
    """Parameters for a single vLLM chat completion.

    vLLM serves the OpenAI ``/v1/chat/completions`` route, which is stateless:
    the whole transcript is replayed on every request and these knobs are
    re-sent each time. `temperature` through `stop` are the OpenAI-compatible
    parameters; `top_k`, `min_p`, `repetition_penalty` and
    `chat_template_kwargs` are vLLM's own sampling and templating extensions,
    sent in the request's ``extra_body``. vLLM hosts no server-side tools,
    so registry-backed A11 actions surfaced through the `tools` input port are
    the only tools available.
    """

    A11_SERIAL_TAG: ClassVar[str] = serial_tags.INTERACT_WITH_VLLM_CONFIG

    max_tokens: int = Field(
        default=-1,
        description=(
            "Maximum number of tokens to generate. -1 lets the model run until"
            " it stops on its own or reaches the deployment's context limit."
        ),
    )
    temperature: float | None = Field(
        default=None,
        description="Sampling temperature.",
        exclude_if=lambda x: x is None,
    )
    top_p: float | None = Field(
        default=None,
        description="Nucleus-sampling probability.",
        exclude_if=lambda x: x is None,
    )
    presence_penalty: float | None = Field(
        default=None,
        description="Penalty applied to tokens that already appeared.",
        exclude_if=lambda x: x is None,
    )
    frequency_penalty: float | None = Field(
        default=None,
        description="Penalty scaled by how often a token already appeared.",
        exclude_if=lambda x: x is None,
    )
    seed: int | None = Field(
        default=None,
        description="Sampling seed for reproducible output.",
        exclude_if=lambda x: x is None,
    )
    stop: list[str] = Field(
        default_factory=list,
        description="Strings that end the generation when produced.",
        exclude_if=lambda x: not x,
    )
    top_k: int | None = Field(
        default=None,
        description="Top-k sampling cutoff (vLLM `extra_body.top_k`).",
        exclude_if=lambda x: x is None,
    )
    min_p: float | None = Field(
        default=None,
        description=(
            "Minimum token probability, relative to the most likely token"
            " (vLLM `extra_body.min_p`)."
        ),
        exclude_if=lambda x: x is None,
    )
    repetition_penalty: float | None = Field(
        default=None,
        description=(
            "Penalty applied to tokens from the prompt and the output so far"
            " (vLLM `extra_body.repetition_penalty`)."
        ),
        exclude_if=lambda x: x is None,
    )
    json_output: bool = Field(
        default=False,
        description=(
            "Constrain the model to emit valid JSON"
            ' (`response_format={"type": "json_object"}`).'
        ),
        exclude_if=lambda x: not x,
    )
    json_schema: dict[str, Any] | None = Field(
        default=None,
        description=(
            "A JSON Schema the output has to satisfy, enforced by vLLM's"
            " structured decoding. Takes precedence over `json_output`."
        ),
        exclude_if=lambda x: x is None,
    )
    chat_template_kwargs: dict[str, Any] = Field(
        default_factory=dict,
        description=(
            "Values passed to the model's chat template, such as"
            ' `{"enable_thinking": true}` on models whose template gates'
            " reasoning (vLLM `extra_body.chat_template_kwargs`)."
        ),
        exclude_if=lambda x: not x,
    )
    extra_body: dict[str, Any] = Field(
        default_factory=dict,
        description=(
            "Additional request fields, merged into the request body last."
            " Covers deployment-specific sampling parameters this config does"
            " not name."
        ),
        exclude_if=lambda x: not x,
    )


#: No model id: a vLLM deployment serves the models it was started with, and the
#: handler asks it for the first of them when no model header is set.
DEFAULT_MODEL = ""


INTERACT_WITH_VLLM_SCHEMA = a11.ActionSchema(
    name="interact_with_vllm",
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
            typeinfo=CreateChatCompletionConfig,
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
            "vLLM API key (only needed for a server started with --api-key).",
        ),
        LlmHeaders.BASE_URL: a11.ActionHeaderSchema(
            LlmHeaders.BASE_URL,
            "vLLM server URL; defaults to VLLM_BASE_URL or the local server.",
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

    The content is the native OpenAI ``{"role", "content"}`` message shape,
    which the handler reads without a normalisation round-trip.
    """
    if role == Role.SYSTEM:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                "vLLM takes a system prompt as system instructions, not as"
                " message content."
            ),
        ).to_exception()

    role_str = "assistant" if role == Role.ASSISTANT else "user"
    system_instructions = [a11.to_chunk(system_prompt)] if system_prompt else []
    return Interaction(
        role=role,
        content=[
            a11.to_chunk({
                "role": role_str,
                "content": text,
            })
        ],
        system_instructions=system_instructions,
    )


def get_interaction_content(interaction: Interaction) -> dict[str, Any]:
    """The single content chunk of an interaction, decoded as a mapping."""
    if not interaction.content or len(interaction.content) > 1:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Expected exactly one content item.",
        ).to_exception()

    return a11.from_chunk(interaction.content[0], obj_type=dict)


def get_output_text_from_interaction(interaction: Interaction) -> str:
    """Extract the model's text from an assistant interaction's content.

    An assistant interaction stores the reconstructed chat message; its text
    lives in the `content` field, either as a string or as a list of content
    parts.
    """
    content = get_interaction_content(interaction).get("content")
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "".join(
            part.get("text", "")
            for part in content
            if isinstance(part, dict) and part.get("type") == "text"
        )
    return ""


class VllmInteractionAdapter(InteractionAdapter):
    def __init__(self, interaction: Interaction):
        self._interaction = interaction

    @staticmethod
    def make_text_message_interaction(
        text: str, system_prompt: str = "", role: Role = Role.USER
    ) -> Interaction:
        return make_text_message_interaction(text, system_prompt, role)

    def get_message_text(self) -> str:
        return get_output_text_from_interaction(self._interaction)
