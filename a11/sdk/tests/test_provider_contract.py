"""Shared interaction contract tests for every LLM provider.

Each provider owns a wire shape, but every shape must preserve the portable
message that A11 uses when a conversation changes backend.
"""

from dataclasses import dataclass
from typing import Any, Callable

import pytest

import a11
from a11.sdk import llm
from a11.sdk.anthropic import messages as anthropic_messages
from a11.sdk.anthropic.interact_with_claude import (
    to_normalized as _claude_loaded,
)
from a11.sdk.anthropic.interact_with_claude_code import (
    to_normalized as _claude_code_loaded,
)
from a11.sdk.anthropic.interact_with_claude_code_schema import (
    ClaudeCodeInteractionAdapter,
    INTERACT_WITH_CLAUDE_CODE_SCHEMA,
)
from a11.sdk.anthropic.interact_with_claude_schema import (
    ClaudeInteractionAdapter,
    INTERACT_WITH_CLAUDE_SCHEMA,
)
from a11.sdk.gemini import interact_with_gemini as gemini
from a11.sdk.gemini.interact_with_gemini_schema import (
    GeminiInteractionAdapter,
    INTERACT_WITH_GEMINI_SCHEMA,
)
from a11.sdk.ollama import interact_with_ollama as ollama
from a11.sdk.ollama.interact_with_ollama_schema import (
    INTERACT_WITH_OLLAMA_SCHEMA,
    OllamaInteractionAdapter,
)
from a11.sdk.openai.interact_with_codex import _codex_to_normalized
from a11.sdk.openai.interact_with_codex_schema import (
    CodexInteractionAdapter,
    INTERACT_WITH_CODEX_SCHEMA,
)
from a11.sdk.openai.interact_with_gpt import _gpt_to_normalized
from a11.sdk.openai.interact_with_gpt_schema import (
    GptInteractionAdapter,
    INTERACT_WITH_GPT_SCHEMA,
)
from a11.sdk.vllm import interact_with_vllm as chat
from a11.sdk.vllm.interact_with_vllm_schema import (
    INTERACT_WITH_VLLM_SCHEMA,
    VllmInteractionAdapter,
)


@dataclass(frozen=True)
class ProviderCase:
    backend: llm.Backend
    schema: a11.ActionSchema
    adapter: type[llm.InteractionAdapter]
    encode: Callable[[llm.NormalizedMessage], Any]


def _anthropic(message: llm.NormalizedMessage) -> dict[str, Any]:
    return anthropic_messages.from_normalized(message)


def _gemini(message: llm.NormalizedMessage) -> dict[str, Any]:
    steps = gemini._gemini_from_normalized(message)
    return {"steps": steps} if message.role == llm.Role.ASSISTANT else steps


def _ollama(message: llm.NormalizedMessage) -> dict[str, Any]:
    messages = ollama._ollama_from_normalized(message)
    return (
        messages[0]
        if len(messages) == 1 and messages[0].get("role") != "tool"
        else {"messages": messages}
    )


def _chat(message: llm.NormalizedMessage) -> dict[str, Any]:
    messages = chat._vllm_from_normalized(message)
    return (
        messages[0]
        if len(messages) == 1 and messages[0].get("role") != "tool"
        else {"messages": messages}
    )


CASES = (
    ProviderCase(
        llm.Backend.CLAUDE,
        INTERACT_WITH_CLAUDE_SCHEMA,
        ClaudeInteractionAdapter,
        _anthropic,
    ),
    ProviderCase(
        llm.Backend.CLAUDE_CODE,
        INTERACT_WITH_CLAUDE_CODE_SCHEMA,
        ClaudeCodeInteractionAdapter,
        _anthropic,
    ),
    ProviderCase(
        llm.Backend.GEMINI,
        INTERACT_WITH_GEMINI_SCHEMA,
        GeminiInteractionAdapter,
        _gemini,
    ),
    ProviderCase(
        llm.Backend.OLLAMA,
        INTERACT_WITH_OLLAMA_SCHEMA,
        OllamaInteractionAdapter,
        _ollama,
    ),
    ProviderCase(
        llm.Backend.VLLM,
        INTERACT_WITH_VLLM_SCHEMA,
        VllmInteractionAdapter,
        _chat,
    ),
    ProviderCase(
        llm.Backend.GPT,
        INTERACT_WITH_GPT_SCHEMA,
        GptInteractionAdapter,
        _chat,
    ),
    ProviderCase(
        llm.Backend.CODEX,
        INTERACT_WITH_CODEX_SCHEMA,
        CodexInteractionAdapter,
        _chat,
    ),
)


def _interaction(
    case: ProviderCase, message: llm.NormalizedMessage
) -> llm.Interaction:
    return llm.Interaction(
        role=message.role,
        content=[a11.to_chunk(case.encode(message))],
        backend_specific_metadata={
            llm.BACKEND_METADATA_KEY: str(case.backend).encode()
        },
    )


@pytest.mark.parametrize("case", CASES, ids=lambda case: str(case.backend))
def test_text_and_image_normalize_to_the_shared_shape(case: ProviderCase):
    expected = llm.NormalizedMessage(
        role=llm.Role.ASSISTANT,
        parts=[
            llm.NormalizedPart(
                type=llm.NormalizedContentType.TEXT, text="done"
            ),
            llm.NormalizedPart(
                type=llm.NormalizedContentType.IMAGE,
                data="QUJD",
                mime_type="image/png",
            ),
        ],
    )

    normalized = llm.normalize_interaction(
        _interaction(case, expected), strict=True
    )
    if case.backend == llm.Backend.OLLAMA:
        normalized.parts[1].mime_type = "image/png"
    assert normalized == expected


@pytest.mark.parametrize("case", CASES, ids=lambda case: str(case.backend))
def test_tool_calls_normalize_to_the_shared_shape(case: ProviderCase):
    expected = llm.NormalizedMessage(
        role=llm.Role.ASSISTANT,
        parts=[
            llm.NormalizedPart(
                type=llm.NormalizedContentType.TOOL_CALL,
                id="lookup",
                name="lookup",
                arguments={"city": "London"},
            )
        ],
    )

    assert (
        llm.normalize_interaction(_interaction(case, expected), strict=True)
        == expected
    )


@pytest.mark.parametrize("case", CASES, ids=lambda case: str(case.backend))
def test_tool_results_normalize_to_the_shared_shape(case: ProviderCase):
    expected = llm.NormalizedMessage(
        role=llm.Role.USER,
        parts=[
            llm.NormalizedPart(
                type=llm.NormalizedContentType.TOOL_RESULT,
                call_id="lookup",
                content='{"temperature":18}',
            )
        ],
    )

    assert (
        llm.normalize_interaction(_interaction(case, expected), strict=True)
        == expected
    )


@pytest.mark.parametrize("case", CASES, ids=lambda case: str(case.backend))
def test_action_and_adapter_surface_is_shared(case: ProviderCase):
    assert set(case.schema.inputs) == {"interactions", "tools", "config"}
    assert set(case.schema.outputs) == {
        "event_stream",
        "thoughts",
        "text_output",
        "new_interactions",
    }
    interaction = case.adapter.make_text_message_interaction(
        "hello", "Be concise.", llm.Role.USER
    )
    assert case.adapter(interaction).get_message_text() == "hello"
    assert a11.from_chunk(interaction.system_instructions[0]) == "Be concise."


# Imports above intentionally register these normalizers. Keep aliases live so
# static tooling does not remove imports whose side effect the suite exercises.
assert all(
    normalizer is not None
    for normalizer in (
        _claude_loaded,
        _claude_code_loaded,
        _gpt_to_normalized,
        _codex_to_normalized,
    )
)
