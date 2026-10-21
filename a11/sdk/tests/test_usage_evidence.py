"""Check actual harness traces and their normalized usage across providers."""

from pathlib import Path
from types import SimpleNamespace

import pytest
from openai.types.completion_usage import CompletionUsage
from anthropic.types.beta import BetaUsage
from anthropic._models import construct_type

from a11.sdk.usage import COUNTERS, usage_delta
from a11.sdk.openai import interact_with_codex as codex
from a11.sdk.openai import interact_with_gpt as gpt
from a11.sdk.anthropic import interact_with_claude as claude
from a11.sdk.anthropic import interact_with_claude_code as claude_code
from a11.sdk.gemini import interact_with_gemini as gemini
from a11.sdk.ollama import interact_with_ollama as ollama
from a11.sdk.vllm import interact_with_vllm as vllm
from scripts.usage_evidence import (
    UsageEvidenceBundle,
    reconcile,
    reconcile_partial_claude_code,
)

_EVIDENCE = UsageEvidenceBundle.model_validate_json(
    (
        Path(__file__).parents[3]
        / "testdata/token_accounting/harness_usage.json"
    ).read_bytes()
)


@pytest.mark.parametrize("case", _EVIDENCE.cases, ids=lambda c: c.name)
def test_recorded_source_usage_matches_adapter_and_reference(case):
    if case.provider == "claude_code" and any(
        "after_usage" in reason for reason in case.triggers.failure
    ):
        comparisons = reconcile_partial_claude_code(case.events)
    else:
        comparisons, _ = reconcile(
            case.provider,
            case.events,
            case.baseline.model_dump() if case.baseline else None,
        )
    assert [row.differences for row in comparisons] == [
        row.differences for row in case.comparison
    ]
    for row in comparisons:
        raw = row.source.model_dump()
        if case.provider == "codex":
            state = codex._TurnUsage()
            state.fresh_thread = True
            state.turn_id = "recorded-turn"
            state.update({"tokenUsage": {"total": raw, "last": raw}})
            actual = state.metadata()
        elif case.provider == "claude_code":
            if any("after_usage" in reason for reason in case.triggers.failure):
                actual = claude_code._build_usage_metadata(raw)
                for name in COUNTERS:
                    assert getattr(actual, name) == getattr(row.expected, name)
                continue
            result = next(
                e
                for e in reversed(case.events)
                if e.get("type") == "result_message"
            )
            actual = claude_code._build_usage_metadata(
                result.get("usage"), result.get("model_usage")
            )
            if case.baseline is not None:
                actual = usage_delta(
                    actual,
                    claude_code._build_usage_metadata(
                        case.baseline.model_dump()
                    ),
                )
        elif case.provider == "gpt" and "input_tokens" in raw:
            actual = gpt._responses_usage(raw)
        elif case.provider in {"gpt", "vllm"}:
            actual = vllm._build_usage_metadata(
                CompletionUsage.model_validate(raw)
            )
        elif case.provider == "claude":
            actual = claude._build_usage_metadata(
                construct_type(type_=BetaUsage, value=raw)
            )
        elif case.provider == "gemini":
            actual = gemini._build_usage_metadata(SimpleNamespace(**raw))
        else:
            actual = ollama._build_usage_metadata(SimpleNamespace(**raw))
        for name in COUNTERS:
            assert getattr(actual, name) == getattr(row.expected, name), (
                case.name,
                name,
            )


def test_compaction_and_truncation_are_observed_and_api_usage_matches_codex():
    by_name = {case.name: case for case in _EVIDENCE.cases}
    for name in [
        "codex-automatic-compaction",
        "codex-tool-truncation",
        "codex-subagents",
    ]:
        case = by_name[name]
        usages = [
            usage.model_dump()
            for request in case.captured_requests
            for usage in request.usage
        ]
        for source_name, normalized in [
            ("input_tokens", "input_tokens"),
            ("output_tokens", "output_tokens"),
            ("total_tokens", "total_tokens"),
        ]:
            count = 0
            for row in case.comparison:
                expected = row.expected
                count += getattr(expected, normalized)
                if source_name == "input_tokens":
                    count += (expected.cached_input_tokens or 0) + (
                        expected.cache_write_tokens or 0
                    )
            assert sum(u[source_name] for u in usages) == count
    assert by_name["codex-automatic-compaction"].triggers.compaction
    assert any(
        output.truncated
        for request in by_name["codex-tool-truncation"].captured_requests
        for output in request.tool_outputs
    )
    assert by_name["claude-code-manual-compaction"].triggers.compaction
    assert by_name["claude-code-subagent"].triggers.subagents
    assert by_name["claude-code-consumed-failure"].triggers.failure
    assert by_name["claude-code-tool-truncation-1"].triggers.truncation
    assert by_name["codex-subagents"].triggers.subagents
    assert by_name["claude-context-clearing"].triggers.clearing
    assert by_name["claude-api-compaction"].triggers.compaction
    assert (
        len(by_name["claude-code-repeated-compaction-3"].triggers.compaction)
        == 2
    )


def test_capability_matrix_names_every_adapter_and_retained_evidence():
    from pydantic import BaseModel
    from typing import Literal
    from pydantic_core import from_json

    class Capability(BaseModel):
        provider: str
        path: str
        counter_scope: str
        source: str
        context: str
        limitations: list[str]
        references: list[str]
        sdk_versions: dict[str, str]
        evidence: list[str]
        verification: Literal["live", "engine_contract_tests"]

    class Matrix(BaseModel):
        version: Literal[1]
        recorded_on: str
        providers: list[Capability]

    matrix = Matrix.model_validate(
        from_json(
            (
                Path(__file__).parents[3]
                / "testdata/token_accounting/capabilities.json"
            ).read_bytes()
        )
    )
    assert {row.provider for row in matrix.providers} == {
        "gpt",
        "claude",
        "codex",
        "claude_code",
        "gemini",
        "ollama",
        "vllm",
        "gemma",
    }
    cases = {case.name: case for case in _EVIDENCE.cases}
    for row in matrix.providers:
        for name in row.evidence:
            assert cases[name].provider == row.provider
        assert row.limitations
        if row.verification == "live":
            assert row.evidence


def test_live_fallback_runs_after_primary_consumption_and_counts_both_models():
    from scripts.usage_evidence import source_counts

    case = next(
        c
        for c in _EVIDENCE.cases
        if c.name == "claude-code-fallback-after-consumption"
    )
    requests = case.captured_requests
    failure = next(i for i, r in enumerate(requests) if r.injected)
    assert any(r.usage for r in requests[:failure])
    assert requests[failure].status_code == 404
    assert any(
        r.model == "claude-sonnet-4-6" and r.usage
        for r in requests[failure + 1 :]
    )
    assert (
        sum(
            source_counts("claude", usage.model_dump()).total_tokens
            for request in requests
            for usage in request.usage
        )
        == case.comparison[0].expected.total_tokens
    )


def test_live_a11_byte_limit_reaches_the_model_as_a_bounded_error():
    case = next(c for c in _EVIDENCE.cases if c.name == "a11-tool-output-limit")
    assert max(case.original_tool_output_bytes) > 128 * 1024
    limited = [
        output
        for request in case.captured_requests
        for output in request.tool_outputs
        if output.truncated
    ]
    assert limited
    assert all(output.characters < 128 * 1024 for output in limited)
    assert all(
        "131072-byte model-facing limit" in output.head for output in limited
    )
