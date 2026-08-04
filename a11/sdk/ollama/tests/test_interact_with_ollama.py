# Copyright 2026 The A11 Authors.

"""Offline tests for the Ollama chat handler.

These drive `interact_with_ollama` against a fake Ollama client that replays a
scripted sequence of `ChatResponse` chunks, so no server or model is needed.
"""

import asyncio

import pytest

import a11
from a11.sdk.llm import Interaction, LlmHeaders, Role
from a11.status import StatusException

ollama = pytest.importorskip("ollama")

from a11.sdk.ollama import interact_with_ollama as mod
from a11.sdk.ollama.interact_with_ollama_schema import (
    INTERACT_WITH_OLLAMA_SCHEMA,
)


# -- fake Ollama chunk builders (real SDK types, so they serialize) -----------


def _message(content=None, thinking=None, tool_calls=None):
    calls = None
    if tool_calls:
        calls = [
            ollama.Message.ToolCall(
                function=ollama.Message.ToolCall.Function(name=n, arguments=a)
            )
            for n, a in tool_calls
        ]
    return ollama.Message(
        role="assistant", content=content, thinking=thinking, tool_calls=calls
    )


def _chunk(message=None, done=False):
    return ollama.ChatResponse(
        model="fake",
        done=done,
        message=message if message is not None else ollama.Message(
            role="assistant"
        ),
        prompt_eval_count=1,
        eval_count=1,
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


class _FakeClient:
    """Replays one scripted chunk list per `chat` call (i.e. per round)."""

    def __init__(self, rounds):
        self._rounds = list(rounds)
        self._n = 0

    async def chat(self, **kwargs):
        chunks = self._rounds[self._n]
        self._n += 1
        return _FakeStream(chunks)


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
    await action["result"].put(f"listing of {path}", final=True)
    await action["result"].drain_and_close()


_TOOL_DEF = {
    "name": "get_info",
    "description": "get info about a path",
    "input_schema": {
        "type": "object",
        "properties": {"path": {"type": "string"}},
        "required": ["path"],
    },
}


async def _run(rounds, monkeypatch, *, read="text_output"):
    """Run one chat turn against ``rounds`` and read values from ``read``.

    Mirrors how the CLI drives the action: a text_output reader runs
    concurrently while the caller feeds inputs and drains ``new_interactions``.
    """
    monkeypatch.setattr(
        mod, "get_ollama_client", lambda *a, **k: _FakeClient(rounds)
    )

    registry = a11.ActionRegistry()
    registry.register("get_info", _GET_INFO, _get_info)

    action = (
        a11.Action(INTERACT_WITH_OLLAMA_SCHEMA)
        .bind_handler(mod.interact_with_ollama)
        .bind_registry(registry)
        .set_header(LlmHeaders.MODEL.value, b"fake")
        .set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"get_info")
    )
    action = action.run()

    collected: list = []

    async def pump():
        async for value in action[read]:
            collected.append(value)

    reader = asyncio.create_task(pump())

    async with (
        action["interactions"] as interactions,
        action["config"],
        action["tools"] as tools,
    ):
        await interactions.put_final(
            Interaction(
                role=Role.USER,
                content=[a11.to_chunk({"role": "user", "content": "hi"})],
            )
        )
        await tools.put(_TOOL_DEF)
        await tools.put_null_final()

    new_interactions = []
    async for interaction in action["new_interactions"]:
        new_interactions.append(interaction)

    await reader
    await action.wait()
    return collected, new_interactions


@pytest.mark.asyncio
async def test_multi_round_tool_calls_get_unique_ids(monkeypatch):
    """Two tool-calling rounds must not reuse the same nested-action id.

    Regression: the accumulator numbered tool calls per round, so the second
    round reused ``call_0`` and its nested action resolved to the first,
    already-closed one — feeding its input then raised "ChunkStoreWriter is
    closed". Each round now continues the id counter.
    """
    rounds = [
        [
            _chunk(_message(thinking="thinking about it ")),
            _chunk(_message(content="Let me look at your home folder.")),
            _chunk(_message(tool_calls=[("get_info", {"path": "~"})])),
            _chunk(done=True),
        ],
        [
            # Text first, then thinking resumes, then a second tool call.
            _chunk(_message(content="Now your projects.")),
            _chunk(_message(thinking="need more detail ")),
            _chunk(_message(tool_calls=[("get_info", {"path": "~/projects"})])),
            _chunk(done=True),
        ],
        [
            _chunk(_message(content="You build software.")),
            _chunk(done=True),
        ],
    ]

    text, new_interactions = await _run(rounds, monkeypatch)

    # All three assistant text segments streamed through, in order.
    assert "".join(text) == (
        "Let me look at your home folder."
        "Now your projects."
        "You build software."
    )
    # 3 assistant interactions + 2 tool-result interactions.
    assert len(new_interactions) == 5

    # The two tool calls were dispatched under distinct ids.
    call_ids = [
        call.id
        for interaction in new_interactions
        for call in interaction.action_calls
    ]
    assert call_ids == ["call_0", "call_1"]


@pytest.mark.asyncio
async def test_thoughts_stream_separately_from_text(monkeypatch):
    """Thinking deltas land on ``thoughts``, not on ``text_output``."""
    rounds = [
        [
            _chunk(_message(thinking="pondering ")),
            _chunk(_message(content="Hello.")),
            _chunk(done=True),
        ],
    ]

    thoughts, _ = await _run(rounds, monkeypatch, read="thoughts")
    assert "".join(thoughts) == "pondering "

    text, _ = await _run(rounds, monkeypatch, read="text_output")
    assert "".join(text) == "Hello."
