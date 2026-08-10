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

    The accumulator continues its id counter across rounds. A reused id would
    resolve to the earlier round's already-closed nested action, and feeding
    its input would raise "ChunkStoreWriter is closed".
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
    assert len(call_ids) == 2
    assert len(set(call_ids)) == 2


@pytest.mark.asyncio
async def test_tool_call_ids_differ_between_turns(monkeypatch):
    """A second turn must not reuse the first turn's synthesised call ids.

    A caller keeps one session — and so one node map — for a whole
    conversation, and the runner gives a call's nested action its id verbatim.
    A counter restarting at zero per handler invocation would resolve the
    second message's ``call_0`` to the first message's closed nodes, and the
    model would get "ChunkStoreWriter is closed" where its tool result belongs.
    Only observable across turns, hence a separate test from the per-round one
    above.
    """

    def rounds():
        return [
            [
                _chunk(_message(content="Looking.")),
                _chunk(_message(tool_calls=[("get_info", {"path": "~"})])),
                _chunk(done=True),
            ],
            [
                _chunk(_message(content="Done.")),
                _chunk(done=True),
            ],
        ]

    def call_ids(new_interactions) -> set[str]:
        return {
            call.id
            for interaction in new_interactions
            for call in interaction.action_calls
        }

    _, first = await _run(rounds(), monkeypatch)
    _, second = await _run(rounds(), monkeypatch)

    first_ids = call_ids(first)
    second_ids = call_ids(second)
    assert len(first_ids) == 1 and len(second_ids) == 1
    assert first_ids.isdisjoint(second_ids)


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


def test_a_claude_tool_result_is_named_for_ollama():
    """Continuing a Claude conversation on Ollama names its tool results.

    Ollama identifies a tool result by the tool's *name*; every other backend
    identifies it by the id of the call it answers, and a bridged result
    therefore arrives with an id and no name. Sending that id as `tool_name`
    hands the model a name it never called — so the name is recovered from the
    tool call in the same conversation.
    """
    pytest.importorskip("anthropic")
    from a11.sdk.anthropic import interact_with_claude  # registers the normalizer

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
        claude(
            {
                "role": "assistant",
                "content": [
                    {
                        "type": "tool_use",
                        "id": "toolu_01",
                        "name": "get_info",
                        "input": {"path": "~"},
                    }
                ],
            }
        )
    )
    conversation.feed_next_interaction(
        claude(
            {
                "role": "user",
                "content": [
                    {
                        "type": "tool_result",
                        "tool_use_id": "toolu_01",
                        "content": "listing of ~",
                    }
                ],
            }
        )
    )

    assistant, tool_result = conversation.messages
    assert assistant["tool_calls"][0]["function"]["name"] == "get_info"
    assert tool_result["role"] == "tool"
    assert tool_result["tool_name"] == "get_info"
    assert tool_result["content"] == "listing of ~"


def test_a_nested_tool_schema_survives_the_sdk():
    """The model is shown the fields of an object parameter, not just its type.

    `Tool.model_validate` coerces a plain dict into the SDK's own `Tool`, whose
    parameters are `Property` objects carrying no nested `properties`; an
    action taking a `request` object would then reach the model as
    `{"type": "object"}` with its `query` and `max_results` fields gone. This
    asserts against the SDK's real validation path, so an SDK or pydantic
    upgrade that breaks the passthrough fails here.
    """
    request_schema = {
        "type": "object",
        "description": "What to search for.",
        "properties": {
            "query": {"type": "string", "description": "Substring to match."},
            "max_results": {"type": "integer", "description": "How many."},
        },
        "required": ["query"],
    }
    tool = {
        "name": "search_project",
        "description": "Find project files.",
        "input_schema": {
            "type": "object",
            "properties": {"request": request_schema},
            "required": ["request"],
        },
    }

    built = mod._build_tools([tool])

    # `Tool.model_validate` is exactly what the client runs on every tool it
    # sends (`ollama._client._copy_tools`).
    sent = ollama.Tool.model_validate(built[0]).model_dump(exclude_none=True)
    parameters = sent["function"]["parameters"]
    assert parameters["properties"]["request"] == request_schema
    assert parameters["required"] == ["request"]


@pytest.mark.asyncio
async def test_a_raw_tool_schema_serializes_without_warnings():
    """The passthrough tool must not narrate itself into the log every turn.

    Pydantic warns when it serializes a value that is not of the declared type,
    which is exactly what `_PassthroughTool` is; the stack varies enough that the
    "once per location" rule does not collapse the copies, so each turn logged
    another one. This drives the real `chat` call — which builds and serializes
    the request, and for a streaming call does no I/O — and asserts nothing is
    warned and nothing is lost.
    """
    import warnings

    from ollama import AsyncClient

    tool = {
        "name": "search_project",
        "description": "Find project files.",
        "input_schema": {
            "type": "object",
            "properties": {
                "request": {
                    "type": "object",
                    "properties": {"query": {"type": "string"}},
                    "required": ["query"],
                }
            },
            "required": ["request"],
        },
    }
    built = mod._build_tools([tool])
    # Port 9 (discard) so a future SDK that does connect fails fast rather than
    # hanging; the assertion is about warnings, not about the response.
    client = AsyncClient(host="http://127.0.0.1:9")

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        with mod._serializing_a_raw_tool_schema():
            try:
                await client.chat(
                    model="m",
                    messages=[{"role": "user", "content": "hi"}],
                    tools=built,
                    stream=True,
                )
            except Exception:  # connection, if it ever gets that far
                pass

    assert [str(warning.message) for warning in caught] == []

    # And the silencing did not come at the cost of the schema.
    sent = ollama.Tool.model_validate(built[0]).model_dump(exclude_none=True)
    request = sent["function"]["parameters"]["properties"]["request"]
    assert request["properties"]["query"] == {"type": "string"}
