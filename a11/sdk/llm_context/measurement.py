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

"""Record context operations and compare matched disabled/enabled runs."""

from __future__ import annotations

import copy
import hashlib
import inspect
import json
import os
import threading
import time
from collections import defaultdict
from collections.abc import Awaitable, Callable
from pathlib import Path

from a11.sdk import usage as usage_api
from a11.sdk.llm import UsageMetadata
from a11.status import Status, StatusCode


class RunRecorder:
    """Append metadata JSONL to a host-selected bounded file.

    Args:
        path: Host-owned output path, independent of caller headers.
        max_bytes: Sink ceiling. Records exceeding the ceiling increment
            dropped and leave the file unchanged.

    Call record with operation/usage metadata. Transform helpers emit no source
    content; callers supplying extra records control those records' contents.
    Each instance serializes its own appends. Separate processes select separate
    run files or provide a shared recording service.
    """

    def __init__(self, path: str | Path, *, max_bytes: int = 64 * 1024 * 1024):
        self.path = Path(path)
        self.max_bytes = max_bytes
        self.dropped = 0
        self._lock = threading.Lock()

    def record(self, report: dict) -> None:
        """Append one JSON record within the configured sink ceiling."""
        encoded = (json.dumps(report, ensure_ascii=False) + "\n").encode()
        with self._lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            fd = os.open(
                self.path, os.O_APPEND | os.O_CREAT | os.O_WRONLY, 0o600
            )
            try:
                if os.fstat(fd).st_size + len(encoded) > self.max_bytes:
                    self.dropped += 1
                    return
                os.write(fd, encoded)
            finally:
                os.close(fd)


def load_runs(path: str | Path) -> list[dict]:
    """Load operation records from a JSON array or JSONL stream."""
    text = Path(path).read_text()
    if text.lstrip().startswith("["):
        return json.loads(text)
    return [json.loads(line) for line in text.splitlines() if line.strip()]


def summarize_run(records: list[dict]) -> dict:
    """Aggregate transform records and known provider contributions.

    Replacement usage revisions share an id. Reconciliation/context-only rows
    contribute no consumption. Known agent/turn aggregates cover their request
    details; unverified cumulative coverage and missing counters stay unknown.
    Repeated operation IDs contribute transform counts once.
    """
    latest = {}
    operations = []
    for row in records:
        if row.get("type") == "a11.usage":
            key = row.get("id")
            if key not in latest or row.get("revision", 0) >= latest[key].get(
                "revision", 0
            ):
                latest[key] = row
        elif row.get("operation") not in (
            "run_metadata",
            "outcome",
            "provider_result",
            "response_observation",
        ):
            operations.append(row)
    totals = defaultdict(float)
    seen = set()
    for row in operations:
        key = row.get("operation_id")
        if key and key in seen:
            continue
        if key:
            seen.add(key)
        for name in (
            "tokens_before",
            "tokens_after",
            "bytes_before",
            "bytes_after",
            "elapsed_seconds",
        ):
            if isinstance(row.get(name), (int, float)):
                totals[name] += row[name]
    contributions = [
        row
        for row in latest.values()
        if row.get("contribution", "consumption") == "consumption"
        and not row.get("usage", {}).get("context_only")
        and not row.get("usage", {}).get("context_invalidated")
    ]
    for index, row in enumerate(contributions):
        if row.get("scope", {}).get("reporting") != "cumulative":
            continue
        baseline = row.get("baseline")
        if not isinstance(baseline, dict):
            continue
        scope = row.get("scope", {})
        source_scope = baseline.get("scope", {})
        keys = ("session_id", "agent_id", "epoch")
        if (
            not scope.get("session_id")
            or any(scope.get(k) != source_scope.get(k) for k in keys)
            or not isinstance(baseline.get("usage"), dict)
        ):
            continue
        delta = usage_api.usage_delta(
            UsageMetadata.model_validate(row["usage"]),
            UsageMetadata.model_validate(baseline["usage"]),
        )
        contributions[index] = {
            **row,
            "usage": delta.model_dump(),
            "scope": {**scope, "reporting": "delta"},
        }
    # A turn aggregate covers requests from the same agent and turn.
    aggregates = {
        (r.get("scope", {}).get("agent_id"), r.get("scope", {}).get("turn_id"))
        for r in contributions
        if r.get("usage", {}).get("usage_scope") == "turn"
        and r.get("scope", {}).get("agent_id")
        and r.get("scope", {}).get("turn_id")
    }
    contributions = [
        r
        for r in contributions
        if r.get("usage", {}).get("usage_scope") == "turn"
        or (
            r.get("scope", {}).get("agent_id"),
            r.get("scope", {}).get("turn_id"),
        )
        not in aggregates
    ]
    ambiguous_cumulative = any(
        r.get("scope", {}).get("reporting") == "cumulative"
        for r in contributions
    )
    partial = any(
        r.get("usage", {}).get("complete") is False for r in contributions
    )
    usage = {}
    for name in (
        "input_tokens",
        "output_tokens",
        "cached_input_tokens",
        "cache_write_tokens",
    ):
        values = [row.get("usage", {}).get(name) for row in contributions]
        usage[name] = (
            sum(values)
            if not ambiguous_cumulative
            and values
            and all(v is not None for v in values)
            else None
        )
    return {
        "operations": (
            len(seen) + sum(not r.get("operation_id") for r in operations)
        ),
        "totals": dict(totals),
        "usage": usage,
        "usage_coverage": (
            "unknown_cumulative"
            if ambiguous_cumulative
            else "partial" if partial else "reported_contributions"
        ),
        "count_sources": sorted(
            {r.get("count_source", "unknown") for r in operations}
        ),
        "quality": "unmeasured",
        "timing": {
            "wall_seconds": next(
                (
                    r.get("elapsed_seconds")
                    for r in records
                    if r.get("operation") == "outcome"
                ),
                None,
            ),
            "first_byte_seconds": [
                r["first_byte_seconds"]
                for r in records
                if r.get("first_byte_seconds") is not None
            ],
        },
    }


def compare_runs(
    baseline: list[dict], treatment: list[dict], *, prices: dict | None = None
) -> dict:
    """Compare matched run records with explicit missing coverage.

    Args:
        baseline: Records from the disabled policy or baseline arm.
        treatment: Records from the enabled policy or treatment arm.
        prices: Optional per-million tariffs for input_tokens,
            cached_input_tokens, cache_write_tokens, and output_tokens.

    Returns:
        A versioned comparison with per-arm totals, reductions, and optionally
        usage-derived cost. Quality stays unmeasured. Missing counters or
        tariffs leave cost unknown; projected counts are separate evidence.
    """
    left, right = summarize_run(baseline), summarize_run(treatment)
    deltas = {}
    for name in sorted(set(left["totals"]) | set(right["totals"])):
        before, after = left["totals"].get(name), right["totals"].get(name)
        deltas[name] = {
            "baseline": before,
            "treatment": after,
            "reduction": (
                before - after
                if before is not None and after is not None
                else None
            ),
            "reduction_percent": (
                100 * (before - after) / before
                if before and after is not None
                else None
            ),
        }
    costs = None
    if prices is not None:

        def cost(run):
            values = run["usage"]
            return (
                sum(values[k] * prices[k] / 1000000 for k in values)
                if run["usage_coverage"] == "reported_contributions"
                and all(values[k] is not None and k in prices for k in values)
                else None
            )

        b, t = cost(left), cost(right)
        costs = {
            "baseline": b,
            "treatment": t,
            "reduction": b - t if b is not None and t is not None else None,
            "source": "usage_times_supplied_tariff",
        }
    identities = [
        [r for r in rows if r.get("operation") == "run_metadata"]
        for rows in (baseline, treatment)
    ]
    match = "unverified"
    if all(
        len(rows) == 1
        and rows[0].get("workload_digest")
        and rows[0].get("pair_id")
        and rows[0].get("setup")
        for rows in identities
    ):
        fields = ("workload_digest", "setup", "pair_id")
        match = (
            "matched"
            if all(
                identities[0][0].get(k) == identities[1][0].get(k)
                for k in fields
            )
            else "mismatched"
        )
    return {
        "schema": "a11.llm-context.comparison/v1",
        "matching": match,
        "baseline": left,
        "treatment": right,
        "deltas": deltas,
        "cost": costs,
        "quality": "unmeasured",
    }


async def evaluate_pair(
    workload: dict,
    invoke: Callable[
        [str, dict, Callable[[dict], None]], dict | Awaitable[dict]
    ],
    *,
    setup: dict,
    grade: Callable[[dict], object] | None = None,
    prices: dict | None = None,
) -> dict:
    """Run isolated baseline/treatment callbacks with matched metadata.

    Args:
        workload: JSON-serializable immutable workload specification.
        invoke: Host callback receiving arm, independent workload copy, and a
            metadata sink. It creates an isolated session/workspace, enables
            only the treatment policy, and returns outcome data. All model,
            summary, retrieval, and subagent usage goes to the same sink with
            distinct usage IDs and explicit scopes.
        setup: Shared provider/model/harness versions, tokenizer identity, seed,
            and generation options. Record actual upstream settings.
        grade: Optional outcome grader returning a JSON value. Errors propagate.
        prices: Optional known per-million disjoint token tariffs.

    Returns:
        Matched records and comparison, measured wall time, and explicit quality
        results. Provider-reported costs remain host evidence in outcome rows.
        Calls run sequentially; side-effect isolation belongs to the host.
    """
    encoded = json.dumps(workload, sort_keys=True, ensure_ascii=False)
    digest = hashlib.sha256(encoded.encode()).hexdigest()
    if not isinstance(setup, dict) or not setup:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Paired evaluation requires explicit setup metadata",
        ).to_exception()
    arms = {}
    for arm in ("baseline", "treatment"):
        records = [
            {
                "schema": "a11.llm-context.report/v1",
                "operation": "run_metadata",
                "pair_id": digest,
                "workload_digest": digest,
                "setup": copy.deepcopy(setup),
                "arm": arm,
            }
        ]
        started = time.monotonic()
        outcome = invoke(arm, json.loads(encoded), records.append)
        if inspect.isawaitable(outcome):
            outcome = await outcome
        quality = grade(outcome) if grade is not None else "unmeasured"
        records.append(
            {
                "operation": "outcome",
                "arm": arm,
                "elapsed_seconds": time.monotonic() - started,
                "quality": quality,
            }
        )
        arms[arm] = {"records": records, "outcome": outcome, "quality": quality}
    comparison = compare_runs(
        arms["baseline"]["records"],
        arms["treatment"]["records"],
        prices=prices,
    )
    comparison["quality"] = {a: arms[a]["quality"] for a in arms}
    return {
        "schema": "a11.llm-context.evaluation/v1",
        "arms": arms,
        "comparison": comparison,
    }
