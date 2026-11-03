# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""OpenRouter chat request options and the A11 action contract."""

from typing import Any, ClassVar

from pydantic import BaseModel, Field

import a11
from a11.data import serial_tags
from a11.sdk.llm import Interaction, InteractionAdapter, LlmHeaders, Role
from a11.sdk.llm_context.config import LLM_CONTEXT_HEADERS
from a11.status import Status, StatusCode


class CreateChatCompletionConfig(BaseModel):
    """Parameters for one OpenRouter chat completion.

    The transcript is replayed on every request. OpenRouter routing and
    reasoning options are sent in the request body. `metadata` is sent only
    when the caller supplies it; A11 interaction metadata stays local.
    """

    A11_SERIAL_TAG: ClassVar[str] = serial_tags.INTERACT_WITH_OPENROUTER_CONFIG

    max_completion_tokens: int | None = Field(default=None, ge=1)
    temperature: float | None = None
    top_p: float | None = None
    top_k: int | None = None
    min_p: float | None = None
    presence_penalty: float | None = None
    frequency_penalty: float | None = None
    seed: int | None = None
    stop: list[str] = Field(default_factory=list)
    reasoning: dict[str, Any] | None = Field(
        default=None,
        description=(
            "OpenRouter reasoning options, such as effort or max_tokens."
        ),
    )
    provider: dict[str, Any] | None = Field(
        default=None, description="OpenRouter provider routing preferences."
    )
    models: list[str] = Field(
        default_factory=list, description="Ordered fallback model slugs."
    )
    modalities: list[str] = Field(
        default_factory=list, description="Requested output modalities."
    )
    session_id: str | None = Field(
        default=None, description="Sticky OpenRouter routing key."
    )
    user: str | None = Field(
        default=None, description="OpenRouter end-user ID."
    )
    metadata: dict[str, str] = Field(
        default_factory=dict, description="Caller-supplied OpenRouter metadata."
    )
    tool_choice: str | dict[str, Any] = Field(
        default="auto", description="Choice of available A11 tools."
    )
    parallel_tool_calls: bool | None = None
    json_output: bool = False
    json_schema: dict[str, Any] | None = None
    extra_body: dict[str, Any] = Field(
        default_factory=dict,
        description="Additional OpenRouter request fields.",
    )


DEFAULT_MODEL = "openrouter/auto"

INTERACT_WITH_OPENROUTER_SCHEMA = a11.ActionSchema(
    name="interact_with_openrouter",
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
            unary=True,
            required=False,
            typeinfo=CreateChatCompletionConfig,
        ),
    },
    outputs={
        "usage_sources": a11.ActionPortSchema(
            "usage_sources",
            "application/json",
            typeinfo=dict,
            description="References to direct and nested usage producers.",
            required=False,
        ),
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
    | LLM_CONTEXT_HEADERS
    | {
        LlmHeaders.USAGE_TRACKING: a11.ActionHeaderSchema(
            LlmHeaders.USAGE_TRACKING,
            "Request dedicated usage streams with 'on'; inherited 'off'"
            " suppresses them.",
        ),
        LlmHeaders.API_KEY: a11.ActionHeaderSchema(
            LlmHeaders.API_KEY, "OpenRouter API key."
        ),
        LlmHeaders.BASE_URL: a11.ActionHeaderSchema(
            LlmHeaders.BASE_URL, "OpenRouter API root."
        ),
        LlmHeaders.ALLOWED_LLM_ACTIONS: a11.ActionHeaderSchema(
            LlmHeaders.ALLOWED_LLM_ACTIONS,
            "Allowed LLM action name patterns, comma-separated.",
        ),
    },
)


def make_text_message_interaction(
    text: str, system_prompt: str = "", role: Role = Role.USER
) -> Interaction:
    """Create a chat message, with initial system instructions if supplied."""
    if role == Role.SYSTEM:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Use system_instructions for an OpenRouter system prompt.",
        ).to_exception()
    return Interaction(
        role=role,
        content=[
            a11.to_chunk(
                {
                    "role": "assistant" if role == Role.ASSISTANT else "user",
                    "content": text,
                }
            )
        ],
        system_instructions=(
            [a11.to_chunk(system_prompt)] if system_prompt else []
        ),
    )


def get_interaction_content(interaction: Interaction) -> dict[str, Any]:
    """Decode an OpenRouter-native chat message from an interaction."""
    if len(interaction.content) != 1:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Expected exactly one content item.",
        ).to_exception()
    return a11.from_chunk(interaction.content[0], obj_type=dict)


def get_output_text_from_interaction(interaction: Interaction) -> str:
    """Read the assistant's visible text, excluding its reasoning details."""
    value = get_interaction_content(interaction).get("content")
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return "".join(
            part.get("text", "")
            for part in value
            if isinstance(part, dict) and part.get("type") == "text"
        )
    return ""


class OpenRouterInteractionAdapter(InteractionAdapter):
    def __init__(self, interaction: Interaction):
        self._interaction = interaction

    @staticmethod
    def make_text_message_interaction(
        text: str, system_prompt: str = "", role: Role = Role.USER
    ) -> Interaction:
        return make_text_message_interaction(text, system_prompt, role)

    def get_message_text(self) -> str:
        return get_output_text_from_interaction(self._interaction)
