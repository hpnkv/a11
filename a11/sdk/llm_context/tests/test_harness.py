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

"""Validate programmatic host installation without an agent or listener."""

import pytest

from a11.sdk.llm_context import (
    LlmContextConfig,
    compact_host_history,
    project_messages,
)
from a11.status import StatusCode, StatusException


def policy(**overrides):
    return LlmContextConfig(
        max_tokens=150, reserve_tokens=0, retain_recent=1, **overrides
    )


def snapshot():
    return {
        "revision": "r1",
        "history": [
            {"role": "user", "id": "old", "content": "x" * 4000},
            {"role": "user", "id": "current", "content": "now"},
        ],
    }


@pytest.mark.asyncio
async def test_host_callbacks_install_with_explicit_revision_and_source():
    source = snapshot()
    accepted = []

    def install(proposal, *, expected_revision, source_digest):
        assert expected_revision == "r1"
        assert source_digest == proposal["source_digest"]
        accepted.append(proposal)
        return {"installed": True, "revision": "r2"}

    result = await compact_host_history(
        lambda: source,
        install,
        policy(),
        summarizer=lambda prefix: "Prior work",
    )
    assert result["installation"] == {"installed": True, "revision": "r2"}
    assert len(accepted) == 1
    assert source["history"][0]["content"] == "x" * 4000


@pytest.mark.asyncio
async def test_changed_revision_on_same_snapshot_prevents_install():
    source = snapshot()
    accepted = []

    def summarize(prefix):
        source["revision"] = "r2"
        return "Prior work"

    with pytest.raises(StatusException) as error:
        await compact_host_history(
            lambda: source,
            lambda *a, **kw: accepted.append(a),
            policy(),
            summarizer=summarize,
        )
    assert error.value.status.code == StatusCode.ABORTED
    assert accepted == []


@pytest.mark.asyncio
async def test_shadow_proposal_needs_no_installer_and_preserves_host_state():
    source = snapshot()
    result = await compact_host_history(
        lambda: source,
        None,
        policy(measurement={"mode": "shadow"}),
        summarizer=lambda prefix: "Prior work",
    )
    assert result["installation"]["installed"] is False
    assert result["proposal"]["checkpoint"] == "Prior work"
    assert source["revision"] == "r1"


@pytest.mark.asyncio
async def test_missing_install_capability_is_explicit():
    with pytest.raises(StatusException) as error:
        await compact_host_history(
            snapshot, None, policy(), summarizer=lambda prefix: "Prior work"
        )
    assert error.value.status.code == StatusCode.FAILED_PRECONDITION


@pytest.mark.asyncio
async def test_programmatic_request_hook_needs_no_action_or_http():
    messages = [
        {"role": "system", "content": "keep  spacing"},
        {"role": "user", "content": "{ " + " " * 200 + '"x": 1 }'},
    ]
    reports = []
    projected = await project_messages(
        messages,
        LlmContextConfig(input_content="lossless"),
        reporter=reports.append,
    )
    assert projected[0] == messages[0]
    assert projected[1]["content"] == '{"x":1}'
    assert reports[0]["changed"]
    assert messages[1]["content"] != projected[1]["content"]
