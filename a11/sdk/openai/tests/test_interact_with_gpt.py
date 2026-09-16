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

"""Offline tests for the OpenAI API interaction provider."""

import asyncio
import json

import pytest
from openai.types.chat import ChatCompletionChunk
from openai.types.chat.chat_completion_chunk import Choice, ChoiceDelta
from openai.types.completion_usage import CompletionUsage
from openai.types.responses import (
    Response,
    ResponseCompletedEvent,
    ResponseFunctionToolCall,
    ResponseOutputItemDoneEvent,
    ResponseTextDeltaEvent,
)

import a11
from a11.sdk import llm
from a11.sdk.openai import interact_with_gpt as mod
from a11.sdk.openai.interact_with_gpt_schema import (
    CreateChatCompletionConfig,
    INTERACT_WITH_GPT_SCHEMA,
)


def _chunk(text: str | None = None, usage=None):
    return ChatCompletionChunk(
        id="chatcmpl-test",
        choices=(
            [Choice(index=0, delta=ChoiceDelta(content=text))]
            if text is not None
            else []
        ),
        created=0,
        model="gpt-test",
        object="chat.completion.chunk",
        usage=usage,
    )


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


class _Completions:
    def __init__(self, chunks):
        self.chunks = chunks
        self.request = None

    async def create(self, **request):
        self.request = request
        return _Stream(self.chunks)


class _Client:
    def __init__(self, chunks, response_events=()):
        self.completions = _Completions(chunks)
        self.chat = type("Chat", (), {"completions": self.completions})()
        rounds = (
            list(response_events)
            if response_events and isinstance(response_events[0], list)
            else [list(response_events)]
        )
        self.responses = _Responses(rounds)


class _Responses:
    def __init__(self, rounds):
        self.rounds = iter(rounds)
        self.requests = []

    @property
    def request(self):
        return self.requests[-1] if self.requests else None

    async def create(self, **request):
        self.requests.append(request)
        return _Stream(next(self.rounds))


def _response_events(text: str):
    response = Response.model_construct(
        id="resp-test",
        created_at=0.0,
        model="gpt-6-astra",
        object="response",
        output=[],
        parallel_tool_calls=True,
        tool_choice="auto",
        tools=[],
        usage=None,
    )
    return [
        ResponseTextDeltaEvent(
            content_index=0,
            delta=text,
            item_id="msg-test",
            logprobs=[],
            output_index=0,
            sequence_number=1,
            type="response.output_text.delta",
        ),
        ResponseCompletedEvent(
            response=response,
            sequence_number=2,
            type="response.completed",
        ),
    ]


def _tool_response_events():
    call = ResponseFunctionToolCall(
        arguments='{"query":"answer"}',
        call_id="call-lookup",
        name="lookup",
        type="function_call",
        status="completed",
    )
    response = Response.model_construct(
        id="resp-tool",
        created_at=0.0,
        model="gpt-6-astra",
        object="response",
        output=[call],
        parallel_tool_calls=True,
        tool_choice="auto",
        tools=[],
        usage=None,
    )
    return [
        ResponseOutputItemDoneEvent(
            item=call,
            output_index=0,
            sequence_number=1,
            type="response.output_item.done",
        ),
        ResponseCompletedEvent(
            response=response,
            sequence_number=2,
            type="response.completed",
        ),
    ]


@pytest.mark.asyncio
async def test_streams_text_and_records_usage_and_structured_options(
    monkeypatch,
):
    usage = CompletionUsage(
        prompt_tokens=8, completion_tokens=3, total_tokens=11
    )
    client = _Client([_chunk("Hel"), _chunk("lo"), _chunk(usage=usage)])
    monkeypatch.setattr(mod, "get_openai_client", lambda *args: client)
    action = (
        a11
        .Action(INTERACT_WITH_GPT_SCHEMA)
        .bind_handler(mod.interact_with_gpt)
        .set_header(llm.LlmHeaders.API_KEY.value, b"test-key")
        .set_header(llm.LlmHeaders.MODEL.value, b"gpt-test")
        .run()
    )

    async def text_output():
        return [value async for value in action["text_output"]]

    text_task = asyncio.create_task(text_output())
    await action["interactions"].finalize(
        llm.Interaction(
            role=llm.Role.USER,
            content=[a11.to_chunk({"role": "user", "content": "hi"})],
        )
    )
    await action["config"].finalize(
        CreateChatCompletionConfig(
            max_completion_tokens=40,
            reasoning_effort="high",
            json_schema={
                "type": "object",
                "properties": {"answer": {"type": "string"}},
                "required": ["answer"],
                "additionalProperties": False,
            },
        )
    )
    await action["tools"].finalize()
    interactions = [value async for value in action["new_interactions"]]
    await action.wait()

    assert "".join(await text_task) == "Hello"
    assert len(interactions) == 1
    assert interactions[0].backend_specific_metadata["backend"] == b"gpt"
    assert interactions[0].usage_metadata.total_tokens == 11
    request = client.completions.request
    assert request["max_completion_tokens"] == 40
    assert request["reasoning_effort"] == "high"
    assert request["response_format"]["type"] == "json_schema"


@pytest.mark.asyncio
async def test_tool_turn_preserves_explicit_reasoning_effort(monkeypatch):
    client = _Client([], [_tool_response_events(), _response_events("done")])
    monkeypatch.setattr(mod, "get_openai_client", lambda *args: client)
    registry = a11.ActionRegistry()
    schema = a11.ActionSchema(
        name="lookup",
        inputs={"query": a11.ActionPortSchema("query", "text/plain")},
        outputs={"result": a11.ActionPortSchema("result", "text/plain")},
    )

    async def lookup(action):
        await action["query"].consume(str)
        await action["result"].finalize("found")

    registry.register("lookup", schema, lookup)
    action = (
        a11
        .Action(INTERACT_WITH_GPT_SCHEMA)
        .bind_handler(mod.interact_with_gpt)
        .bind_registry(registry)
        .set_header(llm.LlmHeaders.API_KEY.value, b"test-key")
        .set_header(llm.LlmHeaders.MODEL.value, b"gpt-6-astra")
        .set_header(llm.LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"lookup")
        .run()
    )
    await action["interactions"].finalize(
        llm.Interaction(
            role=llm.Role.USER,
            content=[a11.to_chunk({"role": "user", "content": "find it"})],
        )
    )
    await action["config"].finalize(
        CreateChatCompletionConfig(
            reasoning_effort="high", max_completion_tokens=321
        )
    )
    await action["tools"].finalize()
    await asyncio.gather(
        *(
            asyncio.create_task(_drain(action[port]))
            for port in (
                "event_stream",
                "thoughts",
                "text_output",
                "new_interactions",
            )
        )
    )
    await action.wait()

    request = client.responses.request
    assert request["reasoning"] == {"effort": "high", "summary": "auto"}
    assert request["tools"][0]["name"] == "lookup"
    assert client.completions.request is None
    assert len(client.responses.requests) == 2
    assert all(
        request["tools"][0]["name"] == "lookup"
        for request in client.responses.requests
    )
    assert all(
        request["max_output_tokens"] == 321
        for request in client.responses.requests
    )
    assert client.responses.requests[1]["input"][0]["role"] == "user"
    assert any(
        item.get("type") == "function_call_output"
        and item.get("call_id") == "call-lookup"
        for item in client.responses.requests[1]["input"]
    )


async def _drain(node):
    return [value async for value in node]


@pytest.mark.asyncio
async def test_raw_response_stream_tolerates_null_function_name():
    events = [
        {
            "type": "response.function_call_arguments.done",
            "name": None,
            "arguments": '{"query":"answer"}',
        },
        {
            "type": "response.output_item.done",
            "item": {
                "type": "function_call",
                "name": "lookup",
                "call_id": "call-lookup",
                "arguments": '{"query":"answer"}',
            },
        },
    ]
    lines = []
    for event in events:
        lines.extend((f"data: {json.dumps(event)}", ""))
    lines.extend(("data: [DONE]", ""))

    class _Raw:
        async def __aenter__(self):
            return self

        async def __aexit__(self, *args):
            return None

        async def iter_lines(self):
            for line in lines:
                yield line

    class _Streaming:
        def create(self, **request):
            return _Raw()

    responses = type(
        "Responses",
        (),
        {"with_streaming_response": _Streaming()},
    )()
    client = type("Client", (), {"responses": responses})()

    decoded = [
        event
        async for event in mod._raw_response_events(
            client, {"stream": True}
        )
    ]

    assert decoded == events
