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

"""Offline tests for the Codex app-server interaction provider."""

import asyncio
import json
from pathlib import Path

import pytest

import a11
from a11.sdk import llm
from a11.sdk.openai import interact_with_codex as mod
from a11.sdk.openai.interact_with_codex_schema import (
    CreateCodexSessionConfig,
    INTERACT_WITH_CODEX_SCHEMA,
    THREAD_ID_METADATA_KEY,
)


async def _collect(node):
    return [value async for value in node]


async def _run(monkeypatch, *, call_tool=False, interactions=None, tools=()):
    calls = []
    tool_response = None

    async def fake_run(*args):
        nonlocal tool_response
        calls.append(args)
        action = args[0]
        handler = args[7]
        if call_tool:
            tool_response = await handler(
                {
                    "tool": "lookup",
                    "callId": "call-1",
                    "arguments": {"key": "x"},
                },
                "thread-8",
            )
        await action["text_output"].put("Done.")
        return "Done.", "thread-8", llm.UsageMetadata(total_tokens=12)

    monkeypatch.setattr(mod, "_run_codex", fake_run)
    registry = a11.ActionRegistry()
    schema = a11.ActionSchema(
        name="lookup",
        inputs={"key": a11.ActionPortSchema("key", "text/plain")},
        outputs={"value": a11.ActionPortSchema("value", "text/plain")},
    )

    async def lookup(action):
        key = await action["key"].consume(str)
        await action["value"].finalize(f"value of {key}")

    registry.register("lookup", schema, lookup)
    action = a11.Action(INTERACT_WITH_CODEX_SCHEMA).bind_handler(
        mod.interact_with_codex
    )
    if tools:
        action = action.bind_registry(registry).set_header(
            llm.LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"lookup"
        )
    action = action.run()
    text_task = asyncio.create_task(_collect(action["text_output"]))
    await action["interactions"].finalize(
        interactions
        or llm.Interaction(
            role=llm.Role.USER,
            content=[a11.to_chunk({"role": "user", "content": "hello"})],
        )
    )
    await action["config"].finalize(CreateCodexSessionConfig())
    for tool in tools:
        await action["tools"].put(tool)
    await action["tools"].finalize()
    produced = [value async for value in action["new_interactions"]]
    await action.wait()
    return await text_task, produced, calls, tool_response


class _Sink:
    def __init__(self):
        self.values = []

    async def put(self, value):
        self.values.append(value)


class _Input:
    def __init__(self):
        self.data = b""

    def write(self, value):
        self.data += value

    async def drain(self):
        pass


class _Process:
    def __init__(self, events):
        self.stdin = _Input()
        self.stdout = asyncio.StreamReader()
        self.stderr = asyncio.StreamReader()
        for event in events:
            self.stdout.feed_data(json.dumps(event).encode() + b"\n")
        self.stdout.feed_eof()
        self.stderr.feed_eof()
        self.returncode = None

    async def wait(self):
        self.returncode = 0
        return 0

    def terminate(self):
        self.returncode = -15


@pytest.mark.asyncio
async def test_jsonl_reader_accepts_a_multimodal_message_over_64_kib():
    stream = asyncio.StreamReader(limit=2**16)
    message = json.dumps({"imageUrl": "x" * 70000}).encode() + b"\n"
    stream.feed_data(message)
    stream.feed_eof()

    assert await mod._read_jsonl_line(stream) == message


@pytest.mark.asyncio
async def test_app_server_uses_structured_thread_turn_and_usage(monkeypatch):
    process = _Process(
        [
            {"id": 1, "result": {}},
            {"id": 2, "result": {"thread": {"id": "thread-9"}}},
            {"id": 3, "result": {"turn": {"id": "turn-1"}}},
            {
                "method": "item/tool/call",
                "id": 77,
                "params": {
                    "threadId": "thread-9",
                    "turnId": "turn-1",
                    "callId": "call-7",
                    "tool": "lookup",
                    "arguments": {"key": "x"},
                },
            },
            {
                "method": "item/reasoning/summaryTextDelta",
                "params": {"delta": "considering"},
            },
            {
                "method": "item/agentMessage/delta",
                "params": {"delta": "Answer."},
            },
            {
                "method": "item/completed",
                "params": {
                    "item": {"type": "agentMessage", "text": "Answer."}
                },
            },
            {
                "method": "thread/tokenUsage/updated",
                "params": {
                    "tokenUsage": {
                        "last": {
                            "inputTokens": 5,
                            "cachedInputTokens": 2,
                            "outputTokens": 3,
                            "totalTokens": 8,
                        }
                    }
                },
            },
            {
                "method": "turn/completed",
                "params": {"turn": {"status": "completed"}},
            },
        ]
    )
    command = []

    async def create(*args, **kwargs):
        command.extend(args)
        return process

    tool_calls = []

    async def tool_handler(params, thread_id):
        tool_calls.append((params, thread_id))
        return {
            "contentItems": [{"type": "inputText", "text": "value"}],
            "success": True,
        }

    monkeypatch.setattr(asyncio, "create_subprocess_exec", create)
    action = {
        "event_stream": _Sink(),
        "thoughts": _Sink(),
        "text_output": _Sink(),
    }
    text, thread_id, usage = await mod._run_codex(
        action,
        CreateCodexSessionConfig(),
        "gpt-test",
        "hello",
        None,
        "be concise",
        [
            {
                "type": "function",
                "name": "lookup",
                "description": "Look up",
                "inputSchema": {"type": "object"},
            }
        ],
        tool_handler,
        {},
    )

    messages = [json.loads(line) for line in process.stdin.data.splitlines()]
    assert text == "Answer."
    assert thread_id == "thread-9"
    assert usage.cached_input_tokens == 2
    assert action["thoughts"].values == ["considering"]
    assert action["text_output"].values == ["Answer."]
    assert command == ["codex", "app-server"]
    assert messages[0]["method"] == "initialize"
    assert messages[0]["params"]["capabilities"]["experimentalApi"]
    assert messages[2]["method"] == "thread/start"
    assert messages[2]["params"]["dynamicTools"][0]["name"] == "lookup"
    assert messages[3]["method"] == "turn/start"
    assert messages[3]["params"]["input"] == [
        {"type": "text", "text": "hello"}
    ]
    assert tool_calls[0][0]["callId"] == "call-7"
    assert tool_calls[0][1] == "thread-9"
    assert messages[-1] == {
        "id": 77,
        "result": {
            "contentItems": [{"type": "inputText", "text": "value"}],
            "success": True,
        },
    }


def test_dynamic_tool_names_are_codex_compatible_and_stable():
    tools, names, digest = mod._dynamic_tools(
        [
            {"name": "files/read", "input_schema": {"type": "object"}},
            {"name": "files read", "input_schema": {"type": "object"}},
            {"name": "mcp__hidden", "input_schema": {"type": "object"}},
        ]
    )

    assert [tool["name"] for tool in tools] == [
        "files_read",
        "files_read_ad37e30bac",
        "a11_mcp__hidden",
    ]
    assert names["files_read"] == "files/read"
    assert len(digest) == 64


def test_recorded_thread_requires_native_transport_and_same_tools():
    metadata = {
        llm.BACKEND_METADATA_KEY: b"codex",
        THREAD_ID_METADATA_KEY: b"thread-10",
        mod.CODEX_TRANSPORT_METADATA_KEY: mod.CODEX_TRANSPORT,
        mod.CODEX_TOOLS_METADATA_KEY: b"same",
    }
    answered = llm.Interaction(
        role=llm.Role.ASSISTANT,
        content=[a11.to_chunk({"role": "assistant", "content": "Earlier."})],
        backend_specific_metadata=metadata,
    )

    assert mod._latest_thread([answered], b"same") == ("thread-10", 0)
    assert mod._latest_thread([answered], b"changed") == (None, -1)


def test_inline_images_become_local_image_inputs(tmp_path):
    interaction = llm.Interaction(
        role=llm.Role.USER,
        content=[
            a11.to_chunk(
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": "inspect this"},
                        {
                            "type": "image",
                            "data": "QUJD",
                            "mime_type": "image/png",
                        },
                    ],
                }
            )
        ],
    )

    paths = mod._write_prompt_images([interaction], str(tmp_path))

    assert len(paths) == 1
    assert paths[0].endswith(".png")
    assert Path(paths[0]).read_bytes() == b"ABC"


@pytest.mark.asyncio
async def test_final_message_records_native_thread(monkeypatch):
    text, produced, _, _ = await _run(monkeypatch)

    assert text == ["Done."]
    assert produced[0].backend_specific_metadata["backend"] == b"codex"
    assert produced[0].backend_specific_metadata[THREAD_ID_METADATA_KEY] == (
        b"thread-8"
    )
    assert produced[0].backend_specific_metadata[
        mod.CODEX_TRANSPORT_METADATA_KEY
    ] == mod.CODEX_TRANSPORT
    assert produced[0].usage_metadata.total_tokens == 12


@pytest.mark.asyncio
async def test_native_tool_call_executes_and_returns_structured_output(
    monkeypatch,
):
    definition = {
        "name": "lookup",
        "description": "look up a key",
        "input_schema": {
            "type": "object",
            "properties": {"key": {"type": "string"}},
            "required": ["key"],
        },
    }
    text, produced, calls, response = await _run(
        monkeypatch, call_tool=True, tools=[definition]
    )

    assert text == ["Done."]
    assert len(produced) == 3
    assert produced[0].action_calls[0].name == "lookup"
    assert produced[1].action_outputs
    assert response == {
        "contentItems": [
            {"type": "inputText", "text": '{"value":"value of x"}'}
        ],
        "success": True,
    }
    assert calls[0][6][0]["type"] == "function"
