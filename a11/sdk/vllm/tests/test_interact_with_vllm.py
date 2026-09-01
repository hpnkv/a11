# Copyright 2026 The A11 Authors.

"""Offline tests for the vLLM chat handler.

These drive `interact_with_vllm` against a fake OpenAI-compatible client that
replays a scripted sequence of `ChatCompletionChunk`s, so no server, no GPU and
no model are needed.
"""

import asyncio

import pytest

import a11
from a11.sdk.llm import Interaction, LlmHeaders, Role

openai = pytest.importorskip("openai")

from openai.types.chat import ChatCompletionChunk  # noqa: E402
from openai.types.chat.chat_completion_chunk import (  # noqa: E402
    Choice,
    ChoiceDelta,
    ChoiceDeltaToolCall,
    ChoiceDeltaToolCallFunction,
)
from openai.types.completion_usage import CompletionUsage  # noqa: E402

from a11.sdk.vllm import interact_with_vllm as mod  # noqa: E402
from a11.sdk.vllm.client import normalize_base_url  # noqa: E402
from a11.sdk.vllm.interact_with_vllm_schema import (  # noqa: E402
    INTERACT_WITH_VLLM_SCHEMA,
)


# -- fake completion chunks (real SDK types, so they serialize) ---------------


def _chunk(
    content=None,
    reasoning=None,
    reasoning_field="reasoning",
    tool_calls=None,
    finish_reason=None,
    usage=None,
    model="fake",
):
    """One streamed chunk, with `tool_calls` as (index, name, arguments)."""
    delta_fields = {}
    if reasoning is not None:
        # A deployment started with a reasoning parser adds this field, under
        # either of the two names vLLM has given it.
        delta_fields[reasoning_field] = reasoning
    if tool_calls:
        delta_fields["tool_calls"] = [
            ChoiceDeltaToolCall(
                index=index,
                id=f"chatcmpl-tool-{index}",
                type="function",
                function=ChoiceDeltaToolCallFunction(
                    name=name, arguments=arguments
                ),
            )
            for index, name, arguments in tool_calls
        ]
    return ChatCompletionChunk(
        id="chatcmpl-1",
        choices=[
            Choice(
                index=0,
                delta=ChoiceDelta(content=content, **delta_fields),
                finish_reason=finish_reason,
            )
        ],
        created=0,
        model=model,
        object="chat.completion.chunk",
        usage=usage,
    )


def _usage_chunk(prompt=7, completion=11):
    """The final chunk `stream_options.include_usage` asks for."""
    return ChatCompletionChunk(
        id="chatcmpl-1",
        choices=[],
        created=0,
        model="fake",
        object="chat.completion.chunk",
        usage=CompletionUsage(
            prompt_tokens=prompt,
            completion_tokens=completion,
            total_tokens=prompt + completion,
        ),
    )


class _FakeStream:
    def __init__(self, chunks):
        self._it = iter(chunks)

    def __aiter__(self):
        return self

    async def __anext__(self):
        try:
            return next(self._it)
        except StopIteration:
            raise StopAsyncIteration


class _FakeModels:
    def __init__(self, served):
        self._served = served

    async def list(self):
        class _Listed:
            data = [type("Model", (), {"id": name})() for name in self._served]

        return _Listed()


class _FakeCompletions:
    """Replays one scripted chunk list per `create` call (i.e. per round)."""

    def __init__(self, rounds, requests):
        self._rounds = list(rounds)
        self._requests = requests
        self._n = 0

    async def create(self, **kwargs):
        self._requests.append(kwargs)
        chunks = self._rounds[self._n]
        self._n += 1
        return _FakeStream(chunks)


class _FakeClient:
    base_url = "http://fake-vllm:8000/v1"

    def __init__(self, rounds, served=("served/model",)):
        self.requests: list[dict] = []
        self.chat = type(
            "Chat", (), {"completions": _FakeCompletions(rounds, self.requests)}
        )()
        self.models = _FakeModels(list(served))


# -- a trivial registry tool the model can call -------------------------------

_GET_INFO = a11.ActionSchema(
    name="get_info",
    inputs={"path": a11.ActionPortSchema("path", "text/plain", required=True)},
    outputs={"result": a11.ActionPortSchema("result", "text/plain")},
)


async def _get_info(action: a11.Action):
    path = ""
    async for value in action["path"]:
        path = value
    await action["result"].finalize(f"listing of {path}")


_TOOL_DEF = {
    "name": "get_info",
    "description": "get info about a path",
    "input_schema": {
        "type": "object",
        "properties": {"path": {"type": "string"}},
        "required": ["path"],
    },
}


async def _run(rounds, monkeypatch, *, read="text_output", model=b"fake"):
    """Run one chat turn against ``rounds`` and read values from ``read``.

    Mirrors how the CLI drives the action: a text_output reader runs
    concurrently while the caller feeds inputs and drains ``new_interactions``.
    """
    client = _FakeClient(rounds)
    monkeypatch.setattr(mod, "get_vllm_client", lambda *a, **k: client)

    registry = a11.ActionRegistry()
    registry.register("get_info", _GET_INFO, _get_info)

    action = (
        a11
        .Action(INTERACT_WITH_VLLM_SCHEMA)
        .bind_handler(mod.interact_with_vllm)
        .bind_registry(registry)
        .set_header(LlmHeaders.MODEL.value, model)
        .set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"get_info")
    )
    action = action.run()

    collected: list = []

    async def pump():
        async for value in action[read]:
            collected.append(value)

    reader = asyncio.create_task(pump())

    await action["interactions"].finalize(
        Interaction(
            role=Role.USER,
            content=[a11.to_chunk({"role": "user", "content": "hi"})],
        )
    )
    await action["config"].finalize()
    tools = action["tools"]
    await tools.put(_TOOL_DEF)
    await tools.finalize()

    new_interactions = []
    async for interaction in action["new_interactions"]:
        new_interactions.append(interaction)

    await reader
    await action.wait()
    return collected, new_interactions, client


@pytest.mark.asyncio
async def test_multi_round_tool_calls_get_unique_ids(monkeypatch):
    """Two tool-calling rounds must not reuse the same nested-action id.

    The accumulator continues its id counter across rounds. A reused id
    resolves to the earlier round's already-closed nested action, and feeding
    its input then fails with "ChunkStoreWriter is closed".
    """
    rounds = [
        [
            _chunk(reasoning="thinking about it "),
            _chunk(content="Let me look at your home folder."),
            _chunk(tool_calls=[(0, "get_info", '{"path": ')]),
            _chunk(tool_calls=[(0, None, '"~"}')]),
            _chunk(finish_reason="tool_calls"),
            _usage_chunk(),
        ],
        [
            _chunk(content="Now your projects."),
            _chunk(reasoning="need more detail "),
            _chunk(tool_calls=[(0, "get_info", '{"path": "~/projects"}')]),
            _chunk(finish_reason="tool_calls"),
            _usage_chunk(),
        ],
        [
            _chunk(content="You build software."),
            _chunk(finish_reason="stop"),
            _usage_chunk(),
        ],
    ]

    text, new_interactions, _ = await _run(rounds, monkeypatch)

    assert "".join(text) == (
        "Let me look at your home folder.Now your projects.You build software."
    )
    # 3 assistant interactions + 2 tool-result interactions.
    assert len(new_interactions) == 5

    call_ids = [
        call.id
        for interaction in new_interactions
        for call in interaction.action_calls
    ]
    assert len(call_ids) == 2
    assert len(set(call_ids)) == 2

    # The arguments streamed as JSON fragments were reassembled and decoded.
    assistant = a11.from_chunk(new_interactions[0].content[0])
    assert assistant["tool_calls"][0]["function"]["arguments"] == '{"path":"~"}'


@pytest.mark.asyncio
async def test_tool_call_ids_differ_between_turns(monkeypatch):
    """A second turn must not reuse the first turn's minted call ids.

    A caller keeps one session -- and so one node map -- for a whole
    conversation, and the runner gives a call's nested action its id verbatim.
    """

    def rounds():
        return [
            [
                _chunk(content="Looking."),
                _chunk(tool_calls=[(0, "get_info", '{"path": "~"}')]),
                _chunk(finish_reason="tool_calls"),
            ],
            [
                _chunk(content="Done."),
                _chunk(finish_reason="stop"),
            ],
        ]

    def call_ids(new_interactions) -> set[str]:
        return {
            call.id
            for interaction in new_interactions
            for call in interaction.action_calls
        }

    _, first, _ = await _run(rounds(), monkeypatch)
    _, second, _ = await _run(rounds(), monkeypatch)

    first_ids = call_ids(first)
    second_ids = call_ids(second)
    assert len(first_ids) == 1 and len(second_ids) == 1
    assert first_ids.isdisjoint(second_ids)


@pytest.mark.asyncio
async def test_a_tool_result_answers_the_call_it_names(monkeypatch):
    """The replayed transcript pairs each tool message with its call id."""
    rounds = [
        [
            _chunk(tool_calls=[(0, "get_info", '{"path": "~"}')]),
            _chunk(finish_reason="tool_calls"),
        ],
        [
            _chunk(content="Done."),
            _chunk(finish_reason="stop"),
        ],
    ]

    _, new_interactions, client = await _run(rounds, monkeypatch)

    # The second request replays the assistant's call and its result.
    replayed = client.requests[1]["messages"]
    assistant = next(m for m in replayed if m["role"] == "assistant")
    result = next(m for m in replayed if m["role"] == "tool")
    assert result["tool_call_id"] == assistant["tool_calls"][0]["id"]
    # One output port, so the decoded result is an object keyed by its name.
    assert result["content"] == '{"result":"listing of ~"}'

    tool_interaction = new_interactions[1]
    stored = a11.from_chunk(tool_interaction.content[0])
    assert (
        stored["messages"][0]["tool_call_id"]
        == (assistant["tool_calls"][0]["id"])
    )


@pytest.mark.parametrize("reasoning_field", ["reasoning", "reasoning_content"])
@pytest.mark.asyncio
async def test_thoughts_stream_separately_from_text(
    monkeypatch, reasoning_field
):
    """Reasoning deltas land on ``thoughts``, not on ``text_output``."""
    rounds = [
        [
            _chunk(reasoning="pondering ", reasoning_field=reasoning_field),
            _chunk(content="Hello."),
            _chunk(finish_reason="stop"),
        ],
    ]

    thoughts, _, _ = await _run(rounds, monkeypatch, read="thoughts")
    assert "".join(thoughts) == "pondering "

    text, interactions, _ = await _run(rounds, monkeypatch, read="text_output")
    assert "".join(text) == "Hello."

    # The reasoning is stored on the interaction and left out of the request.
    stored = a11.from_chunk(interactions[0].content[0])
    assert stored["reasoning_content"] == "pondering "
    assert mod._clean_native_message(stored) == {
        "role": "assistant",
        "content": "Hello.",
    }


@pytest.mark.asyncio
async def test_usage_and_tools_reach_the_request(monkeypatch):
    """Usage is requested and recorded, and a nested tool schema survives."""
    rounds = [
        [
            _chunk(content="Hi."),
            _chunk(finish_reason="stop"),
            _usage_chunk(prompt=13, completion=5),
        ],
    ]

    _, interactions, client = await _run(rounds, monkeypatch)

    request = client.requests[0]
    assert request["stream"] is True
    assert request["stream_options"] == {"include_usage": True}
    assert request["tool_choice"] == "auto"
    assert (
        request["tools"][0]["function"]["parameters"]
        == (_TOOL_DEF["input_schema"])
    )

    usage = interactions[0].usage_metadata
    assert usage.input_tokens == 13
    assert usage.output_tokens == 5
    assert usage.total_tokens == 18


@pytest.mark.asyncio
async def test_the_served_model_answers_an_unset_model_header(monkeypatch):
    """With no model named, the deployment is asked which it serves."""
    rounds = [[_chunk(content="Hi."), _chunk(finish_reason="stop")]]

    _, interactions, client = await _run(rounds, monkeypatch, model=b"")

    assert client.requests[0]["model"] == "served/model"
    # The chunks' own model still names what answered.
    assert interactions[0].model == "fake"


def test_a_claude_tool_result_keeps_its_call_id():
    """Continuing a Claude conversation on vLLM keeps calls and results paired.

    Both backends identify a tool result by the id of the call it answers, so
    the bridge carries the id through and the model sees the pairing it made.
    """
    pytest.importorskip("anthropic")
    from a11.sdk.anthropic import interact_with_claude  # registers normalizer

    del interact_with_claude

    def claude(content: dict) -> Interaction:
        return Interaction(
            content=[a11.to_chunk(content)],
            backend_specific_metadata={
                mod.llm.BACKEND_METADATA_KEY: str(
                    mod.llm.Backend.CLAUDE
                ).encode()
            },
        )

    conversation = mod.Conversation()
    conversation.feed_next_interaction(
        claude({
            "role": "assistant",
            "content": [
                {
                    "type": "tool_use",
                    "id": "toolu_01",
                    "name": "get_info",
                    "input": {"path": "~"},
                }
            ],
        })
    )
    conversation.feed_next_interaction(
        claude({
            "role": "user",
            "content": [
                {
                    "type": "tool_result",
                    "tool_use_id": "toolu_01",
                    "content": "listing of ~",
                }
            ],
        })
    )

    assistant, tool_result = conversation.messages
    call = assistant["tool_calls"][0]
    assert call["id"] == "toolu_01"
    assert call["function"] == {
        "name": "get_info",
        "arguments": '{"path":"~"}',
    }
    assert tool_result["role"] == "tool"
    assert tool_result["tool_call_id"] == "toolu_01"
    assert tool_result["content"] == "listing of ~"


def test_a_neutral_user_message_becomes_chat_content():
    """The provider-neutral envelope the clients mint is read as native."""
    from a11.cli.backends import make_user_interaction

    conversation = mod.Conversation()
    conversation.feed_next_interaction(make_user_interaction("hello"))
    assert conversation.messages == [{"role": "user", "content": "hello"}]


def test_an_inline_image_travels_as_a_data_url():
    """An image part of a neutral message reaches the route as `image_url`."""
    conversation = mod.Conversation()
    conversation.feed_next_interaction(
        Interaction(
            role=Role.USER,
            content=[
                a11.to_chunk({
                    "role": "user",
                    "content": [
                        {"type": "text", "text": "what is this?"},
                        {
                            "type": "image",
                            "data": "QUJD",
                            "mime_type": "image/jpeg",
                        },
                    ],
                })
            ],
        )
    )

    (message,) = conversation.messages
    assert message["content"][1] == {
        "type": "image_url",
        "image_url": {"url": "data:image/jpeg;base64,QUJD"},
    }


@pytest.mark.parametrize(
    ("given", "expected"),
    [
        ("", "http://127.0.0.1:8000/v1"),
        ("http://gpu-box:8000", "http://gpu-box:8000/v1"),
        ("http://gpu-box:8000/", "http://gpu-box:8000/v1"),
        ("http://gpu-box:8000/v1", "http://gpu-box:8000/v1"),
        ("https://vllm.example.com/serve", "https://vllm.example.com/serve/v1"),
    ],
)
def test_the_base_url_names_the_openai_routes(given, expected):
    """Both spellings of a deployment URL reach the OpenAI-compatible root."""
    assert normalize_base_url(given) == expected
