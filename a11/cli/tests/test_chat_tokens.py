"""Token usage accounting without a provider connection."""

import pytest

from a11.cli.chat_tokens import ChatTokens, format_tokens
from a11.sdk.llm import UsageMetadata
from a11.sdk.presentation import BlockKind, PresentationBlock


@pytest.mark.parametrize(
    ("count", "label"),
    [
        (0, "0 tok"),
        (999, "999 tok"),
        (172100, "172.1K tok"),
        (1234500, "1.2M tok"),
    ],
)
def test_format_tokens(count, label):
    assert format_tokens(count) == label
    assert format_tokens(count, estimated=True) == "~" + label


def test_streaming_output_reconciles_with_usage_across_tool_rounds():
    tokens = ChatTokens()
    assert tokens.context_label == "— tok"
    text = PresentationBlock(kind=BlockKind.TEXT, text="a" * 400)
    tokens.observe(text)
    tokens.observe(text)
    assert tokens.output_label == "~100 tok"
    text.text += "b" * 40
    tokens.observe(text)
    assert tokens.output_label == "~110 tok"
    usage = PresentationBlock(
        kind=BlockKind.USAGE,
        usage=UsageMetadata(
            input_tokens=172000,
            output_tokens=100,
            total_tokens=172100,
            cached_input_tokens=100000,
        ),
    )
    tokens.observe(usage)
    tokens.observe(usage)
    assert tokens.context_label == "172.1K tok"
    assert tokens.output_label == "100 tok"
    thought = PresentationBlock(kind=BlockKind.THOUGHT, text="x" * 200)
    tokens.observe(thought)
    assert tokens.output_label == "~150 tok"
    tool = PresentationBlock(kind=BlockKind.TOOL_RUN, text="x" * 8000)
    tokens.observe(tool)
    assert tokens.output_label == "~150 tok"
    second = PresentationBlock(
        kind=BlockKind.USAGE,
        usage=UsageMetadata(input_tokens=175000, output_tokens=75),
    )
    tokens.observe(second)
    tokens.finish()
    assert tokens.context_label == "175.1K tok"
    assert tokens.output_label == "175 tok"
    tokens.begin_turn()
    assert tokens.context_label == "175.1K tok"
    assert tokens.output_label == "~0 tok"


def test_output_without_provider_usage_stays_estimated():
    tokens = ChatTokens()
    tokens.observe(PresentationBlock(kind=BlockKind.TEXT, text="hello"))
    tokens.finish()
    assert tokens.output_label == "~2 tok"


def test_anthropic_context_includes_cache_without_double_counting():
    tokens = ChatTokens()
    tokens.restore_context(
        UsageMetadata(
            input_tokens=100,
            cached_input_tokens=5000,
            cache_write_tokens=1000,
            output_tokens=100,
            total_tokens=6200,
        )
    )
    assert tokens.context_label == "6.2K tok"
