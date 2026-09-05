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

import pytest
from openai.types.chat import ChatCompletionChunk
from openai.types.chat.chat_completion_chunk import Choice, ChoiceDelta
from openai.types.completion_usage import CompletionUsage

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
    def __init__(self, chunks):
        self.completions = _Completions(chunks)
        self.chat = type("Chat", (), {"completions": self.completions})()


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
        a11.Action(INTERACT_WITH_GPT_SCHEMA)
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
