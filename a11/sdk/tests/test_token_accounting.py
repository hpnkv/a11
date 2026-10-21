"""Reconcile recorded provider usage through serialization and presentation."""

import json
from pathlib import Path
from types import SimpleNamespace

import pytest
from anthropic.types import Usage
from openai.types.completion_usage import CompletionUsage

import a11
from a11.cli.chat_tokens import ChatTokens
from a11.cli.presentation_render import _usage_line
from a11.sdk import llm
from a11.sdk.anthropic import interact_with_claude as claude
from a11.sdk.anthropic import interact_with_claude_code as claude_code
from a11.sdk.gemini import interact_with_gemini as gemini
from a11.sdk.ollama import interact_with_ollama as ollama
from a11.sdk.ollama.client import _CacheAwareChatResponse
from a11.sdk.openai import interact_with_codex as codex
from a11.sdk.openai import interact_with_gpt as gpt
from a11.sdk.presentation import (
    BlockKind,
    PresentationBlock,
    PresentationReducer,
)
from a11.sdk.vllm import interact_with_vllm as chat


_FIXTURE = (
    Path(__file__).parents[3] / "testdata/token_accounting/live_usage.json"
)
_CASES = json.loads(_FIXTURE.read_text())["cases"]
_DOWNSTREAM = json.loads((_FIXTURE.parent / "downstream.json").read_text())[
    "runs"
]


def _usages(case):
    state = codex._TurnUsage()
    for event in case["provider_events"]:
        raw = event.get("usage")
        kind = event["kind"]
        if kind == "turn/started":
            state.turn_id = event["turn_id"]
        elif kind == "thread_usage":
            state.update({"turnId": event["turn_id"], "tokenUsage": raw})
        elif kind == "completion":
            yield chat._build_usage_metadata(
                CompletionUsage.model_validate(raw)
            )
        elif kind == "response":
            yield gpt._responses_usage(raw)
        elif kind == "message_delta":
            yield claude._build_usage_metadata(Usage.model_validate(raw))
        elif kind == "interaction":
            yield gemini._build_usage_metadata(SimpleNamespace(**raw))
        elif kind == "result":
            yield claude_code._build_usage_metadata(
                raw, event.get("model_usage")
            )
        elif kind == "ollama":
            yield ollama._build_usage_metadata(
                _CacheAwareChatResponse(
                    model="recorded",
                    done=True,
                    message={"role": "assistant"},
                    **raw,
                )
            )
    if case["provider"] == "codex":
        yield state.metadata()


@pytest.mark.parametrize("case", _CASES, ids=lambda case: case["id"])
def test_recorded_usage_survives_wire_and_presentation(case):
    usages = list(_usages(case))
    assert usages
    expected_output = 0
    reducer = PresentationReducer()
    counter = ChatTokens()
    for usage in usages:
        assert usage is not None
        assert usage.total_tokens == sum(
            value or 0
            for value in (
                usage.input_tokens,
                usage.cached_input_tokens,
                usage.cache_write_tokens,
                usage.output_tokens,
            )
        )
        if usage.reasoning_tokens is not None:
            assert usage.reasoning_tokens <= usage.output_tokens
        expected_output += usage.output_tokens
        interaction = llm.Interaction(
            role=llm.Role.ASSISTANT,
            content=[a11.to_chunk({"role": "assistant", "content": "done"})],
            usage_metadata=usage,
            backend_specific_metadata={
                "provider_usage": llm.encode_backend_value(
                    case["provider_events"]
                ),
            },
        )
        decoded = a11.from_chunk(
            a11.to_chunk(interaction), obj_type=llm.Interaction
        )
        assert decoded.usage_metadata == usage
        assert (
            decoded.backend_specific_metadata
            == interaction.backend_specific_metadata
        )
        reducer.on_interaction(decoded)
        reducer.on_interaction(decoded)
    blocks = [
        block for block in reducer.blocks if block.kind == BlockKind.USAGE
    ]
    assert len(blocks) == len(usages)
    for block in blocks:
        counter.observe(block)
        counter.observe(block.model_copy())
    counter.finish()
    assert counter._output == expected_output
    counter.begin_turn()
    assert counter._output == 0


def test_codex_resumed_tool_turn_uses_total_delta_once():
    state = codex._TurnUsage()
    old = {
        "inputTokens": 100,
        "cachedInputTokens": 40,
        "cacheWriteInputTokens": 10,
        "outputTokens": 20,
        "reasoningOutputTokens": 5,
        "totalTokens": 120,
    }
    state.update({"turnId": "old", "tokenUsage": {"total": old, "last": old}})
    state.turn_id = "new"
    first = {
        "inputTokens": 80,
        "cachedInputTokens": 60,
        "cacheWriteInputTokens": 4,
        "outputTokens": 30,
        "reasoningOutputTokens": 10,
        "totalTokens": 110,
    }
    second = {
        "inputTokens": 90,
        "cachedInputTokens": 70,
        "cacheWriteInputTokens": 5,
        "outputTokens": 15,
        "reasoningOutputTokens": 3,
        "totalTokens": 105,
    }
    total = dict(old)
    for last in [first, second]:
        total = {key: total[key] + last[key] for key in total}
        event = {"turnId": "new", "tokenUsage": {"last": last, "total": total}}
        state.update(event)
        state.update(event)
    state.update({"turnId": "old", "tokenUsage": {"total": old, "last": old}})
    usage = state.metadata()
    assert usage.input_tokens == 31
    assert usage.cached_input_tokens == 130
    assert usage.cache_write_tokens == 9
    assert usage.output_tokens == 45
    assert usage.reasoning_tokens == 13
    assert usage.total_tokens == 215
    assert usage.context_tokens == 105


def test_live_codex_tool_turn_includes_both_model_calls():
    case = next(case for case in _CASES if case["id"] == "codex-tools")
    usage = list(_usages(case))[0]
    assert usage.output_tokens == 141
    assert usage.reasoning_tokens == 95
    assert usage.total_tokens == 23398
    assert usage.context_tokens == 11761


def test_cache_writes_and_provider_specific_details_survive():
    raw = CompletionUsage.model_validate({
        "prompt_tokens": 100,
        "completion_tokens": 30,
        "total_tokens": 130,
        "prompt_tokens_details": {
            "cached_tokens": 60,
            "cache_write_tokens": 10,
            "audio_tokens": 7,
        },
        "completion_tokens_details": {
            "reasoning_tokens": 11,
            "audio_tokens": 9,
            "accepted_prediction_tokens": 3,
            "rejected_prediction_tokens": 4,
        },
    })
    usage = chat._build_usage_metadata(raw)
    assert usage.input_tokens == 30
    accumulator = chat._StreamAccumulator("audit")
    accumulator._usage = raw
    retained = json.loads(
        accumulator.backend_specific_metadata()["provider_usage"]
    )
    assert retained == raw.model_dump(exclude_none=True)


def test_unknown_counters_and_zero_usage_remain_distinct():
    assert gpt._responses_usage(None) is None
    missing = claude_code._build_usage_metadata({"output_tokens": 2})
    assert missing.input_tokens is None
    assert missing.total_tokens is None
    zero = claude_code._build_usage_metadata({
        "input_tokens": 0,
        "output_tokens": 0,
    })
    assert zero.total_tokens == 0
    snapshot = _CacheAwareChatResponse(
        model="recorded",
        message={"role": "assistant"},
        eval_count=2,
    )
    assert ollama._build_usage_metadata(snapshot).total_tokens is None


def test_claude_code_includes_auxiliary_models_once():
    case = next(case for case in _CASES if case["id"] == "claude_code-turn1")
    usage = list(_usages(case))[0]
    assert usage.input_tokens == 1100
    assert usage.output_tokens == 14
    assert usage.total_tokens == 1114
    for name in ("claude_code-turn2", "claude_code-turn3"):
        case = next(case for case in _CASES if case["id"] == name)
        assert list(_usages(case))[0].output_tokens == 5


def test_gemini_reasoning_is_an_output_subset():
    usage = gemini._build_usage_metadata(
        SimpleNamespace(
            total_input_tokens=100,
            total_cached_tokens=60,
            total_output_tokens=20,
            total_thought_tokens=22,
            total_tokens=142,
        ),
        incremental=True,
    )
    assert usage.input_tokens == 40
    assert usage.output_tokens == 42
    assert usage.reasoning_tokens == 22
    assert usage.total_tokens == 142
    assert usage.input_tokens_scope == "incremental"


def test_live_gemini_thinking_is_counted_once():
    case = next(
        case for case in _CASES if case["id"] == "gemini-reasoning-bounded"
    )
    usage = list(_usages(case))[0]
    assert usage.input_tokens == 40
    assert usage.reasoning_tokens == 119
    assert usage.output_tokens == 124
    assert usage.total_tokens == 164


def test_context_uses_request_scope_and_output_resets_each_turn():
    counter = ChatTokens()
    counter.restore_context(
        llm.UsageMetadata(
            input_tokens=3,
            cached_input_tokens=100,
            cache_write_tokens=20,
            output_tokens=7,
            reasoning_tokens=5,
        )
    )
    assert counter.context == 130
    counter.restore_context(
        llm.UsageMetadata(total_tokens=900, usage_scope="turn")
    )
    assert counter.context is None
    counter.restore_context(
        llm.UsageMetadata(
            total_tokens=900,
            usage_scope="turn",
            context_tokens=130,
        )
    )
    assert counter.context == 130
    counter.restore_context(
        llm.UsageMetadata(
            total_tokens=12,
            input_tokens_scope="incremental",
        )
    )
    assert counter.context is None


def test_terminal_uses_compact_inclusive_input_and_output():
    line = _usage_line(
        PresentationBlock(
            kind=BlockKind.USAGE,
            usage=llm.UsageMetadata(
                input_tokens=0,
                cached_input_tokens=100,
                cache_write_tokens=20,
                output_tokens=7,
                reasoning_tokens=5,
                total_tokens=127,
            ),
        )
    )
    assert line == "120 input (100 cached), 7 output"


@pytest.mark.parametrize("run", _DOWNSTREAM, ids=lambda run: run["provider"])
def test_gateway_outputs_match_reporter_scopes_across_user_turns(run):
    counter = ChatTokens()
    previous_thread_output = 0
    lifetime_output = 0
    for turn in run["turns"]:
        provider = run["provider"]
        events = turn["provider_usage_events"]
        if provider == "codex":
            updates = [
                e
                for e in events
                if e.get("method") == "thread/tokenUsage/updated"
            ]
            thread_output = updates[-1]["params"]["tokenUsage"]["total"][
                "outputTokens"
            ]
            expected = thread_output - previous_thread_output
            previous_thread_output = thread_output
        elif provider == "claude_code":
            result = next(e for e in events if e.get("kind") == "result")
            expected = sum(
                m["outputTokens"] for m in result["model_usage"].values()
            )
        else:
            records = [e["usage"] for e in events if e.get("kind") == "request"]
            expected = sum(
                r["eval_count"]
                if provider == "ollama"
                else r["total_output_tokens"]
                + (r.get("total_thought_tokens") or 0)
                if provider == "gemini"
                else r.get("output_tokens", r.get("completion_tokens", 0))
                for r in records
            )
        assert turn["persisted_usage_matches"]
        assert turn["live_replay_matches"]
        counter.begin_turn()
        retained = []
        for index, raw in enumerate(turn["usage"]):
            block = PresentationBlock(
                kind=BlockKind.USAGE,
                interaction_id=f"{turn['turn']}/{index}",
                usage=llm.UsageMetadata.model_validate(raw),
            )
            retained.append(block)
            counter.observe(block)
        counter.finish()
        assert counter._output == turn["output"] == expected
        assert counter.context == turn["context"]
        lifetime_output += counter._output
    if run["provider"] == "codex":
        assert lifetime_output == previous_thread_output == 70
