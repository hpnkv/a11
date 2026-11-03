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

"""OpenRouter conversion, evidence, and nested usage without network."""

import asyncio
import json

import httpx
import openai
import pytest
from openai.types.chat import ChatCompletionChunk
from openai.types.chat.chat_completion_chunk import (
    Choice,
    ChoiceDelta,
    ChoiceDeltaToolCall,
    ChoiceDeltaToolCallFunction,
)
from openai.types.completion_usage import CompletionUsage

import a11
from a11.sdk import llm
from a11.sdk.openrouter import interact_with_openrouter as mod
from a11.sdk.openrouter.client import get_openrouter_client
from a11.sdk.openrouter.interact_with_openrouter_schema import (
    INTERACT_WITH_OPENROUTER_SCHEMA,
    CreateChatCompletionConfig,
)
from a11.sdk.usage import UsageAccounting, UsageReporter, enable_usage_tracking
from a11.status import StatusException


def _chunk(
    *,
    text=None,
    reasoning=None,
    details=None,
    call=None,
    usage=None,
    routing=None,
    finish=None,
):
    delta = ChoiceDelta(content=text, reasoning=reasoning)
    if details is not None:
        delta.reasoning_details = details
    if call is not None:
        delta.tool_calls = [
            ChoiceDeltaToolCall(
                index=0,
                id=call[0],
                type="function",
                function=ChoiceDeltaToolCallFunction(
                    name=call[1], arguments=call[2]
                ),
            )
        ]
    chunk = ChatCompletionChunk(
        id="gen-1",
        choices=(
            [Choice(index=0, delta=delta, finish_reason=finish)]
            if usage is None
            else []
        ),
        created=0,
        model="provider/actual",
        object="chat.completion.chunk",
        usage=usage,
    )
    if routing is not None:
        chunk.openrouter_metadata = routing
    return chunk


class _Stream:
    def __init__(self, chunks):
        self._chunks = iter(chunks)

    def __aiter__(self):
        return self

    async def __anext__(self):
        try:
            return next(self._chunks)
        except StopIteration:
            raise StopAsyncIteration


class _Client:
    def __init__(self, rounds):
        self.rounds = iter(rounds)
        self.requests = []
        self.chat = self
        self.completions = self

    async def create(self, **request):
        self.requests.append(request)
        return _Stream(next(self.rounds))


_TOOL = a11.ActionSchema(
    name="lookup",
    inputs={
        "query": a11.ActionPortSchema("query", "text/plain", required=True)
    },
    outputs={"result": a11.ActionPortSchema("result", "text/plain")},
)


async def _lookup(action):
    reporter = UsageReporter(action, "nested-test")
    await reporter.report(llm.UsageMetadata(total_tokens=3))
    query = "".join([value async for value in action["query"]])
    await action["result"].finalize(f"found {query}")


async def _run(monkeypatch, rounds, *, config=None, history=None, tools=False):
    client = _Client(rounds)
    monkeypatch.setattr(mod, "get_openrouter_client", lambda *args: client)
    registry = a11.ActionRegistry()
    if tools:
        registry.register("lookup", _TOOL, _lookup)
    action = (
        a11.Action(INTERACT_WITH_OPENROUTER_SCHEMA)
        .bind_handler(mod.interact_with_openrouter)
        .bind_registry(registry)
        .set_header(llm.LlmHeaders.MODEL.value, b"openai/example")
    )
    if tools:
        action.set_header(llm.LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"lookup")
    enable_usage_tracking(action)
    accounting = UsageAccounting(action)
    collector = asyncio.create_task(accounting.collect())
    action.run()
    output = []

    async def read_text():
        async for text in action["text_output"]:
            output.append(text)

    text_reader = asyncio.create_task(read_text())
    for item in history or [
        llm.Interaction(
            role=llm.Role.USER,
            content=[a11.to_chunk({"role": "user", "content": "hi"})],
        )
    ]:
        await action["interactions"].put(item)
    await action["interactions"].finalize()
    await action["config"].finalize(config)
    await action["tools"].finalize()
    produced = [item async for item in action["new_interactions"]]
    events = [item async for item in action["event_stream"]]
    await text_reader
    await action.wait()
    await asyncio.wait_for(collector, 5)
    return client, produced, output, events, accounting


@pytest.mark.asyncio
async def test_native_reasoning_and_routing_metadata_replay(monkeypatch):
    usage = CompletionUsage(
        prompt_tokens=10,
        completion_tokens=4,
        total_tokens=14,
        prompt_tokens_details={"cached_tokens": 3},
        completion_tokens_details={"reasoning_tokens": 2},
    )
    usage.cost = 0.004
    details = [{"type": "reasoning.text", "text": "thinking"}]
    client, produced, output, events, accounting = await _run(
        monkeypatch,
        [
            [
                _chunk(reasoning="thinking", details=details),
                _chunk(
                    text="ready", routing={"provider": "fast"}, finish="stop"
                ),
                _chunk(usage=usage),
            ]
        ],
        config=CreateChatCompletionConfig(
            metadata={"job": "example"},
            provider={"order": ["fast"]},
            reasoning={"effort": "low"},
            modalities=["text"],
        ),
    )
    request = client.requests[0]
    assert request["extra_body"]["usage"] == {"include": True}
    assert request["extra_body"]["metadata"] == {"job": "example"}
    assert request["extra_body"]["provider"] == {"order": ["fast"]}
    assert request["extra_body"]["reasoning"] == {"effort": "low"}
    assert request["extra_body"]["modalities"] == ["text"]
    assert request["stream_options"] == {"include_usage": True}
    assert output == ["ready"]
    stored = a11.from_chunk(produced[0].content[0])
    assert stored["reasoning_details"] == details
    assert (
        produced[0].backend_specific_metadata["openrouter_metadata"]
        == b'{"provider":"fast"}'
    )
    assert "openrouter_metadata" not in request["messages"][-1]
    assert produced[0].usage_metadata.input_tokens == 7
    assert produced[0].usage_metadata.cached_input_tokens == 3
    assert produced[0].usage_metadata.reasoning_tokens == 2
    assert (
        json.loads(produced[0].backend_specific_metadata["provider_usage"])[
            "cost"
        ]
        == 0.004
    )
    assert accounting.direct_usage.total_tokens == 14
    assert accounting.total_usage.total_tokens == 14
    assert any(
        isinstance(event, dict)
        and (event.get("usage") or {}).get("cost") == 0.004
        for event in events
    )

    continued = mod.Conversation()
    continued.feed_next_interaction(
        llm.Interaction(
            id=produced[0].previous_interaction_id,
            role=llm.Role.USER,
            content=[a11.to_chunk({"role": "user", "content": "hi"})],
        )
    )
    continued.feed_next_interaction(produced[0])
    assert continued.messages[1]["reasoning_details"] == details
    assert "openrouter_metadata" not in continued.messages[1]
    assert "reasoning_content" not in continued.messages[1]


@pytest.mark.asyncio
async def test_tool_round_preserves_provider_id_and_counts_nested_usage(
    monkeypatch,
):
    first = [
        _chunk(call=("tool-provider-1", "lookup", '{"query":"a"}')),
        _chunk(finish="tool_calls"),
        _chunk(
            usage=CompletionUsage(
                prompt_tokens=5, completion_tokens=3, total_tokens=8
            )
        ),
    ]
    second = [
        _chunk(text="done", finish="stop"),
        _chunk(
            usage=CompletionUsage(
                prompt_tokens=9, completion_tokens=2, total_tokens=11
            )
        ),
    ]
    client, produced, output, _, accounting = await _run(
        monkeypatch,
        [first, second],
        tools=True,
        config=CreateChatCompletionConfig(tool_choice="required"),
    )
    assert output == ["done"]
    assert len(produced) == 3
    assert client.requests[0]["tool_choice"] == "required"
    assert client.requests[1]["tool_choice"] == "auto"
    assert produced[0].action_calls[0].id == "tool-provider-1"
    replay = client.requests[1]["messages"]
    assert replay[-2]["tool_calls"][0]["id"] == "tool-provider-1"
    assert replay[-1]["tool_call_id"] == "tool-provider-1"
    assert replay[-1]["content"] == '{"result":"found a"}'
    assert accounting.direct_usage.total_tokens == 19
    assert accounting.descendant_usage.total_tokens == 3
    assert accounting.total_usage.total_tokens == 22


def test_portable_message_and_metadata_boundary():
    conversation = mod.Conversation()
    source = llm.Interaction(
        role=llm.Role.USER,
        content=[
            a11.to_chunk("hello"),
            a11.to_chunk(b"image", mimetype="image/png"),
        ],
    )
    conversation.feed_next_interaction(source)
    parts = conversation.messages[0]["content"]
    assert parts[0] == {"type": "text", "text": "hello"}
    assert parts[1]["type"] == "image_url"
    assert parts[1]["image_url"]["url"].startswith("data:image/png;base64,")


def test_streamed_image_is_stored_normalized_and_replayed():
    image = {
        "type": "image_url",
        "image_url": {"url": "data:image/png;base64,aGVsbG8="},
    }
    chunk = _chunk(text="picture")
    chunk.choices[0].delta.images = [image]
    accumulator = mod._StreamAccumulator(set())
    accumulator.add(chunk)
    message = accumulator.message_dict()
    assert message["images"] == [image]
    interaction = llm.Interaction(
        role=llm.Role.ASSISTANT,
        content=[a11.to_chunk(message)],
        backend_specific_metadata={llm.BACKEND_METADATA_KEY: b"openrouter"},
    )
    normalized = llm.normalize_interaction(interaction)
    assert [part.type for part in normalized.parts] == [
        llm.NormalizedContentType.TEXT,
        llm.NormalizedContentType.IMAGE,
    ]
    assert normalized.parts[1].data == "aGVsbG8="
    conversation = mod.Conversation()
    conversation.feed_next_interaction(interaction)
    assert conversation.messages[0]["content"] == [
        {"type": "text", "text": "picture"},
        image,
    ]


def test_native_multimodal_parts_survive_replay():
    parts = [
        {"type": "text", "text": "Summarize"},
        {
            "type": "input_audio",
            "input_audio": {"data": "YQ==", "format": "wav"},
        },
        {"type": "file", "file": {"filename": "note.pdf", "file_data": "YQ=="}},
        {
            "type": "video_url",
            "video_url": {"url": "https://example.test/a.mp4"},
        },
    ]
    interaction = llm.Interaction(
        role=llm.Role.USER,
        content=[a11.to_chunk({"role": "user", "content": parts})],
    )
    conversation = mod.Conversation()
    conversation.feed_next_interaction(interaction)
    assert conversation.messages[0]["content"] == parts


def test_client_uses_only_openrouter_credentials(monkeypatch):
    monkeypatch.delenv("OPENROUTER_API_KEY", raising=False)
    monkeypatch.setenv("OPENAI_API_KEY", "unrelated")
    with pytest.raises(StatusException):
        get_openrouter_client()
    client = get_openrouter_client(api_key="openrouter-key")
    assert str(client.base_url) == "https://openrouter.ai/api/v1/"
    assert client.api_key == "openrouter-key"


@pytest.mark.parametrize(
    ("http_status", "expected_code"),
    [
        (402, a11.StatusCode.RESOURCE_EXHAUSTED),
        (404, a11.StatusCode.NOT_FOUND),
    ],
)
def test_routing_and_balance_errors_have_specific_statuses(
    http_status, expected_code
):
    response = httpx.Response(
        http_status,
        request=httpx.Request(
            "POST", "https://openrouter.ai/api/v1/chat/completions"
        ),
    )
    if http_status == 404:
        error = openai.NotFoundError(
            "No compatible endpoint", response=response, body={}
        )
    else:
        error = openai.APIStatusError(
            "Video requires a minimum balance", response=response, body={}
        )
    mapped = mod._api_error(error)
    assert mapped.status.code == expected_code
