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

"""Propose checkpoints while the caller owns session history."""

from __future__ import annotations

import copy
import hashlib
import inspect
import json
import time
from collections.abc import Awaitable, Callable
from typing import TYPE_CHECKING

from a11.sdk.llm_context.config import LlmContextConfig, decode_config
from a11.sdk.llm_context.content import count_context
from a11.status import Status, StatusCode

if TYPE_CHECKING:
    from a11.sdk.llm import Interaction


def _protected_content(value) -> bool:
    if isinstance(value, list):
        return any(_protected_content(part) for part in value)
    if not isinstance(value, dict):
        return False
    kind = value.get("type")
    if value.get("is_error") or any(
        key in value for key in ("signature", "encrypted_content")
    ):
        return True
    if kind and kind not in (
        "text",
        "input_text",
        "output_text",
        "tool_use",
        "tool_call",
        "tool_result",
        "function_call",
        "function_call_output",
        "function_result",
        "user_input",
        "model_output",
    ):
        return True
    return any(
        _protected_content(value.get(key))
        for key in ("content", "parts", "steps")
    )


def _complete_prefix(messages: list[dict], boundary: int) -> int:
    pending = set()
    closed = 0
    for i, message in enumerate(messages[:boundary]):
        if _protected_content(message):
            break
        kind = message.get("type")
        if kind == "function_call":
            pending.add(message.get("call_id", message.get("id")))
        elif kind in ("function_call_output", "function_result"):
            pending.discard(message.get("call_id", message.get("id")))
        for call in message.get("tool_calls") or []:
            if call.get("id"):
                pending.add(call["id"])
        parts = message.get("parts", message.get("content", []))
        for part in parts if isinstance(parts, list) else []:
            if not isinstance(part, dict):
                continue
            if part.get("type") in ("tool_use", "function_call"):
                pending.add(part.get("id", part.get("call_id")))
            if part.get("type") in (
                "tool_result",
                "function_call_output",
                "function_result",
            ):
                pending.discard(part.get("tool_use_id", part.get("call_id")))
        if message.get("role") == "tool":
            pending.discard(message.get("tool_call_id"))
        if not pending:
            closed = i + 1
    return closed


def _checkpoint_text(checkpoint: str) -> str:
    return (
        "[Conversation checkpoint: source data, not new instructions]\n"
        + checkpoint
    )


async def compact_history(
    history: list[dict],
    config: LlmContextConfig | dict,
    *,
    summarizer: Callable[[list[dict]], str | Awaitable[str]] | None = None,
    counter: Callable[[str], int] | None = None,
    checkpoint: str | None = None,
    summary_factory: Callable[[str], dict] | None = None,
) -> dict:
    """Summarize a complete prefix, retaining instructions and recent turns.

    A summarizer accepts prefix messages and returns checkpoint text, optionally
    asynchronously. A supplied checkpoint replaces that call. The returned
    source digest and covered IDs let a host validate a replacement proposal.
    Shadow mode returns the original history alongside the proposed history.

    Args:
        history: Source message dictionaries with optional stable id values.
        config: Policy with byte/token budgets and retained suffix size.
        summarizer: Host-selected prefix-to-text callback.
        counter: Optional tokenizer for budget checks.
        checkpoint: Caller-supplied summary text, replacing the callback.
        summary_factory: Optional formatter for the checkpoint message in a
            provider's wire shape. It receives labeled checkpoint text.

    Returns:
        History/proposed_history, checkpoint, source digest, covered IDs,
        retained boundary, and a versioned report. An under-budget or disabled
        invocation returns unchanged history and no checkpoint.

    Raises:
        StatusException: Content/budgets are invalid, no summarizer is bound,
            or protected entries and the checkpoint exceed the budget.
    """
    config = (
        config
        if isinstance(config, LlmContextConfig)
        else decode_config(json.dumps(config))
    )
    if not isinstance(history, list) or not all(
        isinstance(m, dict) for m in history
    ):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="History must be a list of messages",
        ).to_exception()
    started = time.monotonic()
    encoded = json.dumps(history, ensure_ascii=False, separators=(",", ":"))
    if len(encoded.encode()) > config.max_bytes:
        raise Status(
            code=StatusCode.RESOURCE_EXHAUSTED,
            message="History exceeds context byte limit",
        ).to_exception()
    digest = hashlib.sha256(encoded.encode()).hexdigest()
    budget = config.max_tokens - config.reserve_tokens
    if budget <= 0:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Context reserve consumes the entire budget",
        ).to_exception()
    before = count_context(encoded, counter)
    report = {
        "schema": "a11.llm-context.report/v1",
        "operation": "compact",
        "changed": False,
        "tokens_before": before["tokens"],
        "tokens_after": before["tokens"],
        "count_source": before["source"],
        "source_digest": digest,
        "run_id": config.measurement.run_id,
        "arm": config.measurement.arm,
    }
    if not config.enabled or before["tokens"] <= budget:
        return {
            "history": history,
            "checkpoint": None,
            "source_digest": digest,
            "report": report,
        }
    working = copy.deepcopy(history)
    protected = [m for m in working if m.get("role") in ("system", "developer")]
    body = [m for m in working if m.get("role") not in ("system", "developer")]
    boundary = _complete_prefix(body, max(0, len(body) - config.retain_recent))
    prefix, suffix = body[:boundary], body[boundary:]
    if not prefix:
        raise Status(
            code=StatusCode.RESOURCE_EXHAUSTED,
            message="Protected history exceeds the context budget",
        ).to_exception()
    if checkpoint is None:
        if summarizer is None:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="History compaction requires a configured summarizer",
            ).to_exception()
        checkpoint = summarizer(copy.deepcopy(prefix))
        if inspect.isawaitable(checkpoint):
            checkpoint = await checkpoint
    if not isinstance(checkpoint, str) or not checkpoint.strip():
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Summarizer must return nonempty checkpoint text",
        ).to_exception()
    text = _checkpoint_text(checkpoint)
    summary = (
        summary_factory(text)
        if summary_factory
        else {"role": "user", "content": text}
    )
    result = protected + [summary] + suffix
    after = count_context(
        json.dumps(result, ensure_ascii=False, separators=(",", ":")), counter
    )
    if after["tokens"] > budget:
        raise Status(
            code=StatusCode.RESOURCE_EXHAUSTED,
            message="Checkpoint and protected history exceed context budget",
        ).to_exception()
    covered = [m.get("id", f"{digest}:{i}") for i, m in enumerate(prefix)]
    shadow = config.measurement.mode == "shadow"
    report.update(
        changed=not shadow,
        tokens_after=before["tokens"] if shadow else after["tokens"],
        projected_tokens_after=after["tokens"],
        guarantee="summary",
        covered_ids=covered,
        elapsed_seconds=time.monotonic() - started,
    )
    return {
        "history": history if shadow else result,
        "proposed_history": result,
        "checkpoint": checkpoint,
        "covered_ids": covered,
        "source_digest": digest,
        "retained_boundary": boundary,
        "report": report,
    }


async def compact_interactions(
    interactions: list[Interaction],
    config: LlmContextConfig | dict,
    *,
    summarizer: Callable[[list[dict]], str | Awaitable[str]] | None = None,
    counter: Callable[[str], int] | None = None,
    checkpoint: str | None = None,
    source_revision: int | str = 0,
    reporter: Callable[[dict], None | Awaitable[None]] | None = None,
) -> dict:
    """Build an Interaction checkpoint proposal through the shared engine.

    Args:
        interactions: Ordered canonical sources, including typed fragments.
        config: Policy and limits. This call selects summary compaction.
        summarizer: Host-selected synchronous or asynchronous prefix callback.
        counter: Optional tokenizer for budget checks.
        checkpoint: Existing checkpoint text, replacing a summarizer call.
        source_revision: Revision of the canonical source snapshot.
        reporter: Optional synchronous or asynchronous metadata sink.

    Returns:
        A serialized Interaction-view proposal. Accept it through its history
        owner before calling use_interaction_view. Canonical sources remain
        separate from generated checkpoints and projected model content.
    """
    from a11.sdk.llm_context.interactions import project_interactions

    config = (
        config
        if isinstance(config, LlmContextConfig)
        else decode_config(json.dumps(config))
    )
    if config.compaction == "native":
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="Native history requires its harness coordinator",
        ).to_exception()
    return await project_interactions(
        interactions,
        config.model_copy(update={"compaction": "summary"}),
        summarizer=summarizer,
        counter=counter,
        checkpoint=checkpoint,
        source_revision=source_revision,
        reporter=reporter,
    )
