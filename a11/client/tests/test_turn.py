# Copyright 2026 The A11 Authors.

"""One whole turn driven the way `a11 chat` drives it: over a gateway.

The chain this covers is the point of the change. A client announces a tool of
its own, asks a gateway to run a turn, the (fake) model calls that tool, the
gateway reverse-dispatches the call back over the same stream so it runs *here*,
and the whole thing comes out as `PresentationBlock`s a renderer can draw --
including the tool run, which `a11 chat` previously could not show at all.
"""

from __future__ import annotations

import asyncio
import pathlib

import pytest

import a11
from a11 import timing
from a11.client.connection import GatewayConnection
from a11.client.turn import TurnConfig, run_turn
from a11.gateway.config import GatewayConfig
from a11.gateway.embedded import embedded_gateway
from a11.sdk.llm import Interaction, Role
from a11.sdk.presentation import BlockKind, PresentationReducer

ollama = pytest.importorskip("ollama")


@pytest.fixture(autouse=True)
def _fresh_conversation_store():
    """Reset the process-global store between tests.

    `get_conversation_store` is deliberately process-wide -- one gateway serves
    every client and a conversation id must mean one thing -- so two tests with
    two roots would otherwise collide.
    """
    from a11.gateway import conversations

    conversations._STORE = None
    yield
    conversations._STORE = None

from a11.sdk.ollama import interact_with_ollama as ollama_mod  # noqa: E402

_ANSWER = "the tool said hello"
_LOG = "$ echo hello\nhello"


def _chunk(message, done: bool = False):
    return ollama.ChatResponse(
        model="fake",
        done=done,
        message=message,
        prompt_eval_count=1,
        eval_count=1,
    )


def _rounds():
    """A tool call, then an answer."""
    call = ollama.Message.ToolCall(
        function=ollama.Message.ToolCall.Function(
            name="client_echo", arguments={"text": "hello"}
        )
    )
    return [
        [
            _chunk(ollama.Message(role="assistant", tool_calls=[call])),
            _chunk(ollama.Message(role="assistant"), done=True),
        ],
        [
            _chunk(ollama.Message(role="assistant", content=_ANSWER)),
            _chunk(ollama.Message(role="assistant"), done=True),
        ],
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


#: A tool the *client* owns. Its user-facing log is what a reader sees for the
#: run; its result is what the model sees.
_ECHO_SCHEMA = a11.ActionSchema(
    name="client_echo",
    description="Echo text back, as the client's own tool.",
    inputs={
        "text": a11.ActionPortSchema(
            name="text", type="application/json", unary=True, required=True
        )
    },
    outputs={
        "result": a11.ActionPortSchema(name="result", type="application/json"),
        "user_facing_log": a11.ActionPortSchema(
            name="user_facing_log", type="text/plain"
        ),
    },
)


async def _echo(action: a11.Action) -> None:
    text = await action["text"].consume(str)
    async with action["result"] as result, action["user_facing_log"] as log:
        await result.put_final({"echoed": text})
        await log.put_final(_LOG)


def _descriptor() -> dict:
    return {
        "name": "client_echo",
        "description": _ECHO_SCHEMA.description,
        "inputs": [
            {
                "name": "text",
                "type": "application/json",
                "unary": True,
                "required": True,
            }
        ],
        "outputs": [
            {"name": "result", "type": "application/json"},
            {
                "name": "user_facing_log",
                "type": "text/plain",
                "user_facing": True,
            },
        ],
        # "$" means the `result` port *is* the whole tool result, rather than a
        # field of an object wrapping it.
        "output_to_json_field": {"result": "$"},
    }


@pytest.mark.asyncio
async def test_a_turn_runs_a_client_tool_and_renders_as_blocks(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
):
    fake = _FakeOllama(_rounds())
    monkeypatch.setattr(ollama_mod, "get_ollama_client", lambda *a, **k: fake)

    # The client's own registry: the gateway dispatches the model's tool calls
    # back to this, over the same stream.
    registry = a11.ActionRegistry()
    registry.register(_ECHO_SCHEMA.name, _ECHO_SCHEMA, _echo)

    settings = GatewayConfig(
        conversation_store_root=tmp_path,
        shell_tools=False,
        audio_capture=False,
        speech_recognition=False,
    )

    reducer = PresentationReducer()
    async with embedded_gateway(settings, registry=registry) as connection:
        await connection.announce_tools([_descriptor()])
        new_interactions = await run_turn(
            connection,
            [],
            Interaction(
                role=Role.USER,
                content=[
                    a11.to_chunk(
                        {
                            "role": "user",
                            "content": [{"type": "text", "text": "say hello"}],
                        }
                    )
                ],
            ),
            [_descriptor()],
            TurnConfig(
                provider="ollama",
                model="fake",
                # Announcing a tool is not enough: the header gates what the
                # model may see, bridged tools included.
                allowed_actions="client_echo",
            ),
            reducer,
        )

    # The model was offered the client's tool, by name.
    assert "client_echo" in fake.tools_offered
    assert new_interactions

    kinds = [block.kind for block in reducer.blocks]
    # A tool run and the answer, in that order: the run happened first.
    assert BlockKind.TOOL_RUN in kinds
    assert BlockKind.TEXT in kinds
    assert kinds.index(BlockKind.TOOL_RUN) < kinds.index(BlockKind.TEXT)

    run = next(b for b in reducer.blocks if b.kind == BlockKind.TOOL_RUN)
    assert run.tool_name == "client_echo"
    # The tool's own narration reached the reader -- this is what `a11 chat`
    # could not show before.
    assert run.text == _LOG

    answer = "".join(
        b.text for b in reducer.blocks if b.kind == BlockKind.TEXT
    )
    assert _ANSWER in answer


@pytest.mark.asyncio
async def test_the_turn_is_recorded_by_the_gateway(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
):
    """Persistence is the gateway's job now, not the client's."""
    from a11.gateway import conversations

    fake = _FakeOllama([_rounds()[1]])
    monkeypatch.setattr(ollama_mod, "get_ollama_client", lambda *a, **k: fake)

    settings = GatewayConfig(
        conversation_store_root=tmp_path,
        shell_tools=False,
        audio_capture=False,
        speech_recognition=False,
    )
    question = Interaction(
        role=Role.USER,
        content=[
            a11.to_chunk(
                {"role": "user", "content": [{"type": "text", "text": "hi"}]}
            )
        ],
    )

    async with embedded_gateway(settings) as connection:
        await run_turn(
            connection,
            [],
            question,
            [],
            TurnConfig(provider="ollama", model="fake"),
            PresentationReducer(),
        )

    store = conversations.ConversationStore(tmp_path)
    listed = await store.list()
    assert len(listed) == 1
    # The conversation id is the first interaction's id, and the title comes
    # from the shared presentation text.
    assert listed[0]["id"] == question.id
    assert listed[0]["title"] == "hi"


@pytest.mark.asyncio
async def test_a_turn_works_over_a_websocket_after_a_pause(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
):
    """A real WebSocket, and a delay between connecting and the first turn.

    Two things this pins that an in-process stream cannot. The framing and
    lifecycle live in `channel_wire_stream`, which backs WebSocket but not the
    in-memory pair -- so a stream-level regression is invisible to the embedded
    tests. And the pause matters: the connect timeout must bound the *handshake*
    only. Setting it as `WireStreamOptions.deadline` instead aborts the stream at
    that absolute time, which let a chat connect, announce its tools, and then
    fail every turn a user was slow enough to reach.
    """
    from a11 import net
    from a11.gateway import app as gateway_app
    from a11.gateway import conversations

    fake = _FakeOllama([_rounds()[1]])
    monkeypatch.setattr(ollama_mod, "get_ollama_client", lambda *a, **k: fake)

    settings = GatewayConfig(
        a11_port=0,
        conversation_store_root=tmp_path,
        shell_tools=False,
        audio_capture=False,
        speech_recognition=False,
    )
    store = conversations.ConversationStore(tmp_path)
    gateway = gateway_app.A11Gateway(
        store, gateway_app._make_action_registry(settings, store)
    )
    options = net.WebSocketServerOptions()
    options.path = "/a11"
    options.bind_address = "127.0.0.1"
    options.port = 0
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False
    server = net.WebSocketWireServer.create(gateway.handle_stream, options)

    reducer = PresentationReducer()
    try:
        connection = await GatewayConnection.connect(
            f"ws://127.0.0.1:{server.port}/a11",
            timeout=timing.Duration.milliseconds(500),
        )
        await connection.probe()
        # Longer than the connect timeout just used. The stream must not have
        # been given that as its own deadline.
        await asyncio.sleep(1.0)
        await run_turn(
            connection,
            [],
            Interaction(
                role=Role.USER,
                content=[
                    a11.to_chunk(
                        {
                            "role": "user",
                            "content": [{"type": "text", "text": "hey"}],
                        }
                    )
                ],
            ),
            [],
            TurnConfig(provider="ollama", model="fake"),
            reducer,
        )
        await connection.aclose()
    finally:
        server.stop()

    assert BlockKind.TEXT in [block.kind for block in reducer.blocks]


@pytest.mark.asyncio
async def test_the_event_stream_port_is_drained_even_when_unwatched(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
):
    """An output port with no reader eventually stops the backend writing it.

    Every backend mirrors its raw provider chunks onto `event_stream`, and a
    client that never reads them fills the session's per-stream buffer and then
    stalls mid-answer -- a turn that never finishes, intermittently, and worse
    the longer the answer. So `run_turn` drains the port whether or not the
    caller asked to see it, and `on_event` only decides who is told.
    """
    fake = _FakeOllama([_rounds()[1]])
    monkeypatch.setattr(ollama_mod, "get_ollama_client", lambda *a, **k: fake)

    settings = GatewayConfig(
        conversation_store_root=tmp_path,
        shell_tools=False,
        audio_capture=False,
        speech_recognition=False,
    )
    seen: list[object] = []

    async with embedded_gateway(settings) as connection:
        await run_turn(
            connection,
            [],
            Interaction(
                role=Role.USER,
                content=[
                    a11.to_chunk(
                        {"role": "user", "content": [{"type": "text", "text": "hi"}]}
                    )
                ],
            ),
            [],
            TurnConfig(
                provider="ollama", model="fake", on_event=seen.append
            ),
            PresentationReducer(),
        )

    # The backend really does write there, which is what makes leaving it unread
    # a hazard rather than a theoretical one.
    assert seen, "the backend wrote no events, so this test proves nothing"
