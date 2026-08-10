# Copyright 2026 The A11 Authors.

"""One whole turn against the gateway, over a wire: no model, no network.

A client session calls ``interact_with_llm`` on a real gateway registry across
an in-process wire pair, exactly as the IDE plugin does across a WebSocket. The
provider is a fake Ollama that calls ``shell_execute`` once and then answers, so
the turn goes the whole way: a tool the *caller never described* is offered
because its allowed-tool patterns admit it, it runs on the gateway, its
user-facing log is kept out of the model's tool result and recorded in the
turn's metadata instead, and the conversation is persisted.

That last part is why this test drives a wire rather than a local action: the
turn's ``interactions`` port is filled by the peer, and recording a turn must
not depend on the *local* write side of a port nobody on this side writes.
"""

import argparse
import asyncio
import contextlib
import json
import pathlib

import pytest

import a11
from a11 import net
from a11.gateway import app as gateway_app
from a11.gateway import conversations
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.sdk.llm import (
    Interaction,
    LlmHeaders,
    Role,
    TOOL_LOGS_METADATA_KEY,
)
from a11.service.session import Session

ollama = pytest.importorskip("ollama")

from a11.sdk.ollama import interact_with_ollama as ollama_mod  # noqa: E402

_TOKEN = "HELLO_FROM_THE_GATEWAY"


def _chunk(message: ollama.Message, done: bool = False) -> ollama.ChatResponse:
    return ollama.ChatResponse(
        model="fake",
        done=done,
        message=message,
        prompt_eval_count=1,
        eval_count=1,
    )


def _tool_call_round() -> list[ollama.ChatResponse]:
    call = ollama.Message.ToolCall(
        function=ollama.Message.ToolCall.Function(
            name="shell_execute", arguments={"command": f"echo {_TOKEN}"}
        )
    )
    return [
        _chunk(ollama.Message(role="assistant", tool_calls=[call])),
        _chunk(ollama.Message(role="assistant"), done=True),
    ]


def _answer_round() -> list[ollama.ChatResponse]:
    return [
        _chunk(
            ollama.Message(role="assistant", content=f"it printed {_TOKEN}")
        ),
        _chunk(ollama.Message(role="assistant"), done=True),
    ]


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


class _FakeOllama:
    """Replays one scripted round per ``chat`` call, noting the tools sent."""

    def __init__(self, rounds):
        self._rounds = list(rounds)
        self._round = 0
        self.tools_offered: list[str] = []

    async def chat(self, **kwargs):
        for tool in kwargs.get("tools") or []:
            name = tool.get("function", {}).get("name") or tool.get("name")
            if name:
                self.tools_offered.append(name)
        chunks = self._rounds[self._round]
        self._round += 1
        return _FakeStream(chunks)


def _gateway(
    root: pathlib.Path,
) -> tuple[gateway_app.A11Gateway, conversations.ConversationStore]:
    args = argparse.Namespace(
        host="127.0.0.1",
        a11_port=0,
        conversation_store_root=root,
        no_shell_tools=False,
        no_audio_capture=True,
        no_speech_recognition=True,
    )
    # A store of its own, not the process-global one: two tests must not share a
    # conversation index.
    store = conversations.ConversationStore(root)
    registry = gateway_app._make_action_registry(args, store)
    return gateway_app.A11Gateway(store, registry), store


@pytest.mark.asyncio
async def test_a_turn_runs_a_gateway_tool_and_is_recorded(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
):
    fake = _FakeOllama([_tool_call_round(), _answer_round()])
    monkeypatch.setattr(
        ollama_mod, "get_ollama_client", lambda *a, **k: fake
    )

    gateway, store = _gateway(tmp_path)
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.create_task(gateway.handle_stream(server_stream))

    client = Session(action_registry=a11.ActionRegistry())
    await client.add_stream(client_stream, mode="start")

    call = (
        a11.Action(INTERACT_WITH_LLM_SCHEMA)
        .bind_node_map(client.node_map)
        .bind_session(client)
        .bind_stream(client_stream)
    )
    call.set_header(LlmHeaders.PROVIDER.value, b"ollama")
    call.set_header(LlmHeaders.MODEL.value, b"fake")
    # The plugin's header: the IDE's own tools (none here) plus the patterns
    # that let the gateway add its own. Nothing is sent on the `tools` port at
    # all, so the shell tool can only reach the model by being matched here.
    call.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"shell_.*")
    await call.call()

    question = Interaction(
        role=Role.USER,
        content=[a11.to_chunk({"role": "user", "content": f"echo {_TOKEN}"})],
    )
    async with (
        call["interactions"] as interactions,
        call["tools"] as tools,
        call["config"] as config,
    ):
        await interactions.put_final(question)
        await tools.put_null_final()
        await config.put_null_final()

    text: list[str] = []
    produced: list[Interaction] = []

    async def read_text() -> None:
        async for token in call["text_output"]:
            text.append(str(token))

    async def read_interactions() -> None:
        async for interaction in call["new_interactions"]:
            produced.append(interaction)

    async def drain(port: str) -> None:
        async for _ in call[port].iter_fragments():
            pass

    await asyncio.wait_for(
        asyncio.gather(
            read_text(),
            read_interactions(),
            drain("thoughts"),
            drain("event_stream"),
        ),
        timeout=60,
    )
    await asyncio.wait_for(call.wait(), timeout=30)

    assert "".join(text) == f"it printed {_TOKEN}"
    # Offered without the caller describing it: the allow-list is the request.
    assert "shell_execute" in fake.tools_offered

    # The tool result the model was given carries the command's output, and the
    # run log rides in metadata instead -- never in the result.
    results = [i for i in produced if i.action_outputs]
    assert len(results) == 1
    fragments = [
        f for group in results[0].action_outputs.values() for f in group
    ]
    output = [
        a11.from_chunk(f.get_chunk())
        for f in fragments
        if not f.get_chunk().is_null()
    ]
    assert _TOKEN in " ".join(str(value) for value in output)

    logs = json.loads(
        results[0].backend_specific_metadata[TOOL_LOGS_METADATA_KEY]
    )
    assert len(logs) == 1
    log = next(iter(logs.values()))
    # The narration names the command and how much came back.
    assert log.startswith(f"`echo {_TOKEN}` — 1 line of output.")
    assert _TOKEN in log
    # And none of it reached the result the model was given. Asserted against
    # the log actually written, so a reworded narration cannot make this pass
    # vacuously.
    summary = log.splitlines()[0]
    assert not any(summary in str(value) for value in output)

    # And the whole turn was recorded, run log and all.
    stored = await store.read(question.id)
    assert [i.id for i in stored] == [question.id, *[i.id for i in produced]]
    recorded = [i for i in stored if i.action_outputs]
    assert TOOL_LOGS_METADATA_KEY in recorded[0].backend_specific_metadata

    # Teardown only. The gateway's session ends when its stream does, and an
    # in-process pair has no socket to drop, so the serving task is cancelled
    # rather than waited on.
    client.half_close()
    serving.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await serving
