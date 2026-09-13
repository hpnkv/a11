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

import json

import a11
from pydantic import TypeAdapter

from a11.sdk.anthropic.interact_with_claude_code_schema import (
    CreateSessionConfig as ClaudeCodeConfig,
)
from a11.sdk.anthropic.interact_with_claude_schema import CreateMessageConfig
from a11.sdk.gemini.interact_with_gemini_schema import CreateInteractionConfig
from a11.sdk.llm import Interaction, LlmHeaders
from a11.sdk.ollama.interact_with_ollama_schema import CreateChatConfig
from a11.sdk.openai.interact_with_codex_schema import CreateCodexSessionConfig
from a11.sdk.openai.interact_with_gpt_schema import CreateChatCompletionConfig
from a11.sdk.vllm.interact_with_vllm_schema import (
    CreateChatCompletionConfig as VllmChatConfig,
)

PROVIDER_CONFIG_SCHEMA = TypeAdapter(
    CreateMessageConfig
    | ClaudeCodeConfig
    | CreateCodexSessionConfig
    | CreateChatCompletionConfig
    | CreateInteractionConfig
    | CreateChatConfig
    | VllmChatConfig
).json_schema()

INTERACT_WITH_LLM_SCHEMA = a11.ActionSchema(
    name="interact_with_llm",
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
            description=(
                "Options for the provider selected by x-a11-llm-provider."
                " Choose that provider's object schema."
            ),
            unary=True,
            required=True,
            typeinfo=dict,
            json_schema=json.dumps(PROVIDER_CONFIG_SCHEMA),
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
            LlmHeaders.API_KEY, "API key for the downstream provider."
        ),
        LlmHeaders.PROVIDER: a11.ActionHeaderSchema(
            LlmHeaders.PROVIDER, "The downstream provider."
        ),
        LlmHeaders.MODEL: a11.ActionHeaderSchema(
            LlmHeaders.MODEL, "The downstream model."
        ),
        LlmHeaders.ALLOWED_LLM_ACTIONS: a11.ActionHeaderSchema(
            LlmHeaders.ALLOWED_LLM_ACTIONS,
            "The allowed downstream LLM action (tool) name patterns,"
            " comma-separated.",
        ),
    },
)
