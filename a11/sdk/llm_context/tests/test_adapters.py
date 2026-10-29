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

"""Validate native compaction barriers and programmatic harness mappings."""

import asyncio
from types import SimpleNamespace

import pytest

from a11.sdk.llm_context import (
    LlmContextConfig,
    claude_code_hooks,
    codex_tool_hook,
    compact_acp_session,
    compact_claude_session,
    compact_codex_thread,
    project_acp_prompt,
)
from a11.status import StatusException


@pytest.mark.asyncio
async def test_codex_barrier_filters_children_and_waits_for_matching_turn():
    events = asyncio.Queue()
    calls = []

    async def request(method, params):
        calls.append((method, params))
        for thread in ("child", "root"):
            events.put_nowait(
                {
                    "method": "item/completed",
                    "params": {
                        "threadId": thread,
                        "turnId": "compact",
                        "item": {"type": "contextCompaction"},
                    },
                }
            )
            events.put_nowait(
                {
                    "method": "turn/completed",
                    "params": {
                        "threadId": thread,
                        "turn": {"id": "compact", "status": "completed"},
                    },
                }
            )
        return {}

    result = await compact_codex_thread(request, events.get, "root")
    assert calls == [("thread/compact/start", {"threadId": "root"})]
    assert result["installed"]


@pytest.mark.asyncio
async def test_codex_timeout_is_indeterminate_and_never_retries():
    calls = []

    async def request(method, params):
        calls.append(method)

    with pytest.raises(TimeoutError):
        await compact_codex_thread(
            request, asyncio.Queue().get, "root", timeout=0.01
        )
    assert calls == ["thread/compact/start"]


@pytest.mark.asyncio
async def test_claude_requires_boundary_and_successful_result():
    ResultMessage = type("ResultMessage", (), {})

    class Client:
        commands = []

        async def query(self, command):
            self.commands.append(command)

        async def receive_response(self):
            yield SimpleNamespace(subtype="compact_boundary")
            result = ResultMessage()
            result.is_error, result.session_id = False, "session"
            yield result

    client = Client()
    report = await compact_claude_session(client)
    assert report["installed"]
    assert client.commands == ["/compact"]


def test_codex_hook_requires_known_success_and_preserves_error():
    config = LlmContextConfig(shell={"mode": "compress"})
    raw = {"stdout": "same\n" * 1000, "stderr": "", "exit_code": 0}
    assert codex_tool_hook({"tool_response": raw}, config)["continue"] is False
    for result in ({**raw, "exit_code": 1}, {"stdout": raw["stdout"]}):
        assert codex_tool_hook({"tool_response": result}, config) == {}


@pytest.mark.asyncio
async def test_claude_hook_preserves_unknown_exit_status_and_result_fields():
    callback = claude_code_hooks(LlmContextConfig(shell={"mode": "compress"}))[
        "PostToolUse"
    ][0].hooks[0]
    raw = {"stdout": "same\n" * 1000, "stderr": "", "interrupted": False}
    assert await callback({"tool_response": raw}, "id", {}) == {}
    raw["exit_code"] = 0
    result = await callback({"tool_response": raw}, "id", {})
    view = result["hookSpecificOutput"]["updatedToolOutput"]
    assert view["interrupted"] is False
    assert view["stdout"] != raw["stdout"]


@pytest.mark.asyncio
async def test_acp_prompt_preserves_resources_and_requires_advertised_command():
    raw = {
        "sessionId": "s",
        "prompt": [
            {"type": "text", "text": "{ " + " " * 1000 + '"x":1 }'},
            {"type": "resource_link", "uri": "file:///source"},
        ],
    }
    result = await project_acp_prompt(
        raw, LlmContextConfig(input_content="lossless")
    )
    assert result["prompt"][0]["text"] == '{"x":1}'
    assert result["prompt"][1] == raw["prompt"][1]

    async def request(method, params):
        return {"stopReason": "end_turn", "sessionId": params["sessionId"]}

    with pytest.raises(StatusException):
        await compact_acp_session(request, "s", [])
    assert (await compact_acp_session(request, "s", [{"name": "compress"}]))[
        "sessionId"
    ] == "s"


@pytest.mark.asyncio
async def test_claude_native_short_history_requires_structured_evidence():
    ResultMessage = type("ResultMessage", (), {})

    class Client:
        structured = True
        command = None

        async def query(self, command):
            self.command = command

        async def receive_response(self):
            if self.structured:
                yield SimpleNamespace(
                    subtype="status",
                    data={
                        "compact_result": "failed",
                        "compact_error": "Not enough messages to compact.",
                    },
                )
            result = ResultMessage()
            result.is_error = False
            result.result = "Not enough messages to compact."
            yield result

    client = Client()
    report = await compact_claude_session(
        client, instructions="Preserve required IDs"
    )
    assert client.command == "/compact Preserve required IDs"
    assert report["installed"] is False
    assert report["skip_reason"] == "insufficient_history"
    client.structured = False
    with pytest.raises(StatusException):
        await compact_claude_session(client)
