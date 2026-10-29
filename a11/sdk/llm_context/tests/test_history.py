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

"""Preserve protected history and complete tool groups during compaction."""

import pytest

from a11.sdk.llm_context import compact_history
from a11.status import StatusException


@pytest.mark.asyncio
async def test_compaction_protects_system_and_recent_turn_and_original():
    history = [
        {"role": "system", "content": "Keep constraints"},
        {"role": "user", "content": "old " * 1000},
        {"role": "assistant", "content": "done"},
        {"role": "user", "content": "current request"},
    ]
    result = await compact_history(
        history,
        {"max_tokens": 200, "reserve_tokens": 0, "retain_recent": 1},
        checkpoint="Prior objective and decision",
    )
    assert result["history"][0] == history[0]
    assert result["history"][-1] == history[-1]
    assert history[1]["content"] == "old " * 1000
    assert result["report"]["tokens_after"] < result["report"]["tokens_before"]


@pytest.mark.asyncio
async def test_unresolved_tool_group_is_never_summarized():
    history = [
        {
            "role": "assistant",
            "tool_calls": [{"id": "call-1"}],
            "content": "x" * 3000,
        },
        {"role": "user", "content": "current"},
    ]
    with pytest.raises(StatusException):
        await compact_history(
            history,
            {"max_tokens": 100, "reserve_tokens": 0, "retain_recent": 1},
            checkpoint="summary",
        )


@pytest.mark.asyncio
async def test_missing_summarizer_is_an_explicit_capability_error():
    with pytest.raises(StatusException):
        await compact_history(
            [
                {"role": "user", "content": "x" * 4000},
                {"role": "user", "content": "now"},
            ],
            {"max_tokens": 100, "reserve_tokens": 0, "retain_recent": 1},
        )


@pytest.mark.asyncio
async def test_shadow_checkpoint_is_a_proposal_without_installation():
    history = [
        {"role": "user", "content": "x" * 4000},
        {"role": "user", "content": "now"},
    ]
    result = await compact_history(
        history,
        {
            "max_tokens": 100,
            "reserve_tokens": 0,
            "retain_recent": 1,
            "measurement": {"mode": "shadow"},
        },
        checkpoint="Earlier work",
    )
    assert result["history"] is history
    assert result["proposed_history"] != history
    assert not result["report"]["changed"]


@pytest.mark.asyncio
async def test_interaction_proposal_retains_source_ids():
    import a11
    from a11.sdk.llm import Interaction
    from a11.sdk.llm_context import compact_interactions

    old = Interaction(content=[a11.to_chunk("x" * 4000)])
    recent = Interaction(content=[a11.to_chunk("now")])
    result = await compact_interactions(
        [old, recent],
        {"max_tokens": 100, "reserve_tokens": 0, "retain_recent": 1},
        checkpoint="Earlier work",
    )
    assert result["covered_ids"] == [old.id]
    assert result["entries"][-1]["source_id"] == recent.id
    assert result["state"] == "proposal"
