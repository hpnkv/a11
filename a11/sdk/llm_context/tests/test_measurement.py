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

"""Reconcile metadata without turning estimates into billing evidence."""

import json

import pytest

from a11.sdk.llm_context.measurement import (
    RunRecorder,
    compare_runs,
    load_runs,
    summarize_run,
)


def test_revision_reconciliation_context_rows_and_aggregate_coverage():
    def row(
        identity,
        revision,
        tokens,
        scope,
        kind="request",
        contribution="consumption",
    ):
        return {
            "type": "a11.usage",
            "id": identity,
            "revision": revision,
            "scope": scope,
            "contribution": contribution,
            "usage": {
                "input_tokens": tokens,
                "output_tokens": 2,
                "cached_input_tokens": 0,
                "cache_write_tokens": 0,
                "usage_scope": kind,
            },
        }

    scope = {"agent_id": "agent", "turn_id": "turn"}
    records = [
        row("req", 1, 5, scope),
        row("req", 2, 7, scope),
        row("turn", 1, 10, scope, "turn"),
        row("detail", 1, 99, scope, contribution="reconciliation"),
    ]
    result = summarize_run(records)
    assert result["usage"]["input_tokens"] == 10
    assert result["usage"]["output_tokens"] == 2


def test_unknown_cache_counts_and_cumulative_coverage_remain_unknown():
    records = [
        {
            "type": "a11.usage",
            "id": "one",
            "usage": {"input_tokens": 4},
            "scope": {"reporting": "cumulative"},
        }
    ]
    result = compare_runs(records, records, prices={"input_tokens": 1})
    assert result["baseline"]["usage"]["input_tokens"] is None
    assert result["baseline"]["usage"]["cached_input_tokens"] is None
    assert result["cost"]["baseline"] is None
    assert result["quality"] == "unmeasured"


def test_replayed_operations_count_once_and_zero_baseline_has_no_percentage():
    record = {"operation_id": "one", "tokens_after": 0, "bytes_after": 0}
    result = compare_runs([record, record], [record])
    assert result["baseline"]["operations"] == 1
    assert result["deltas"]["tokens_after"]["reduction_percent"] is None


def test_recorder_bounds_sink_and_loads_jsonl(tmp_path):
    path = tmp_path / "run.jsonl"
    recorder = RunRecorder(path, max_bytes=128)
    recorder.record({"operation_id": "one"})
    recorder.record({"large": "x" * 128})
    assert load_runs(path) == [{"operation_id": "one"}]
    assert recorder.dropped == 1
    assert "content" not in path.read_text()


@pytest.mark.asyncio
async def test_paired_evaluation_copies_workload_and_records_quality():
    from a11.sdk.llm_context import evaluate_pair

    workload = {"input": ["source"]}

    async def invoke(arm, specification, record):
        specification["input"].append(arm)
        record(
            {
                "operation_id": arm,
                "tokens_before": 100,
                "tokens_after": 50 if arm == "treatment" else 100,
            }
        )
        return {"answer": "correct"}

    result = await evaluate_pair(
        workload,
        invoke,
        setup={"model": "test"},
        grade=lambda result: result["answer"] == "correct",
    )
    assert workload == {"input": ["source"]}
    assert result["comparison"]["matching"] == "matched"
    assert result["comparison"]["quality"] == {
        "baseline": True,
        "treatment": True,
    }


def test_partial_usage_does_not_become_complete_billing():
    row = {
        "type": "a11.usage",
        "id": "one",
        "usage": {
            "input_tokens": 10,
            "output_tokens": 1,
            "cached_input_tokens": 0,
            "cache_write_tokens": 0,
            "complete": False,
        },
    }
    result = compare_runs([row], [row], prices={k: 1 for k in row["usage"]})
    assert result["baseline"]["usage_coverage"] == "partial"
    assert result["cost"]["baseline"] is None


def test_verified_cumulative_baseline_respects_session_epoch():
    scope = {
        "session_id": "s",
        "agent_id": "a",
        "epoch": 1,
        "reporting": "cumulative",
    }
    row = {
        "type": "a11.usage",
        "id": "one",
        "scope": scope,
        "usage": {"input_tokens": 20},
        "baseline": {"scope": scope, "usage": {"input_tokens": 5}},
    }
    assert summarize_run([row])["usage"]["input_tokens"] == 15
    row["baseline"]["scope"] = {**scope, "epoch": 0}
    assert summarize_run([row])["usage"]["input_tokens"] is None


def test_missing_metadata_is_unverified_and_wall_time_is_separate():
    rows = [
        {"operation": "run_metadata"},
        {"operation_id": "compress", "elapsed_seconds": 0.1},
        {"operation": "outcome", "elapsed_seconds": 1.0},
    ]
    result = compare_runs(rows, rows)
    assert result["matching"] == "unverified"
    assert result["baseline"]["totals"]["elapsed_seconds"] == 0.1
    assert result["baseline"]["timing"]["wall_seconds"] == 1.0
