# Copyright 2026 The A11 Authors.

"""The four browser guides' backend, driven the way the guides' pages drive it.

A client session talks to `a11.demos.web_demos_server`'s own service over a
wire, so what is exercised is what a page does: a recorded chat turn read back
after a reload, the deep-research flow, an action the *page* serves being called
by the model, and the WebSocket listener a browser actually connects to.

The provider is a fake Ollama that answers from what it was asked, so the whole
turn runs -- the real `interact_with_llm`, the real tool runner, the real
conversation store -- with no network and no model.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import os
import pathlib
import subprocess
import sys

import pytest
import pytest_asyncio

import a11
from a11 import net, timing
from a11.cli.commands import serve as serve_command
from a11.demos import web_demos_server as demos
from a11.gateway import conversation_actions, conversations
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.sdk.llm import Interaction, LlmHeaders, Role
from a11.service.serving import serving, websocket
from a11.service.session import Session

ollama = pytest.importorskip("ollama")

from a11.sdk.ollama import interact_with_ollama as ollama_mod  # noqa: E402

_TIMEOUT = 60


# --- The fake provider -------------------------------------------------------


def _chunk(message: ollama.Message, done: bool = False) -> ollama.ChatResponse:
    return ollama.ChatResponse(
        model="fake",
        done=done,
        message=message,
        prompt_eval_count=1,
        eval_count=1,
    )


def _says(text: str) -> list[ollama.ChatResponse]:
    return [
        _chunk(ollama.Message(role="assistant", content=text)),
        _chunk(ollama.Message(role="assistant"), done=True),
    ]


def _calls(name: str, arguments: dict) -> list[ollama.ChatResponse]:
    call = ollama.Message.ToolCall(
        function=ollama.Message.ToolCall.Function(
            name=name, arguments=arguments
        )
    )
    return [
        _chunk(ollama.Message(role="assistant", tool_calls=[call])),
        _chunk(ollama.Message(role="assistant"), done=True),
    ]


class _FakeStream:
    def __init__(self, chunks) -> None:
        self._it = iter(chunks)

    def __aiter__(self):
        return self

    async def __anext__(self):
        try:
            return next(self._it)
        except StopIteration:
            raise StopAsyncIteration


class _FakeOllama:
    """Return deterministic `chat` responses based on request content.

    Parallel investigations may reach the provider in any order, so responses
    cannot depend on call sequence.
    """

    def __init__(self, reply) -> None:
        self._reply = reply
        self.prompts: list[str] = []
        self.tools_offered: list[str] = []

    async def chat(self, **kwargs):
        for tool in kwargs.get("tools") or []:
            name = tool.get("function", {}).get("name") or tool.get("name")
            if name:
                self.tools_offered.append(name)
        asked = "\n".join(
            str(message.get("content") or "")
            for message in kwargs.get("messages") or []
        )
        self.prompts.append(asked)
        return _FakeStream(self._reply(asked))


@pytest.fixture
def fake_ollama(monkeypatch: pytest.MonkeyPatch):
    """Installs a fake client and returns the "reply to a prompt" hook."""

    def install(reply) -> _FakeOllama:
        fake = _FakeOllama(reply)
        monkeypatch.setattr(
            ollama_mod, "get_ollama_client", lambda *a, **k: fake
        )
        return fake

    return install


# --- Connecting to the demo service ------------------------------------------


class _Peer:
    """A client session on one end of a wire, the demo service on the other."""

    def __init__(
        self,
        session: Session,
        stream: net.WireStream,
        store: conversations.ConversationStore,
        service,
        serving_task: asyncio.Task,
    ) -> None:
        self.session = session
        self.stream = stream
        self.store = store
        # Held, not merely started: a service nobody references is collected,
        # and its sessions go down with it mid-test.
        self.service = service
        self._serving = serving_task

    def action(self, schema: a11.ActionSchema) -> a11.Action:
        return (
            a11
            .Action(schema)
            .bind_node_map(self.session.node_map)
            .bind_session(self.session)
            .bind_stream(self.stream)
        )

    async def close(self) -> None:
        """Shut the connection *and its service* down, in that order.

        Cancelling the serving future alone is not enough: the service outlives
        it, and a live one with an accepted session leaves native pumps running
        that the tests after this one then share a process with. Aborting it and
        waiting is what makes this module's teardown invisible to the rest of
        the suite.
        """
        self.session.half_close()
        with contextlib.suppress(Exception):
            await self.stream.close()
        self.service.abort(
            a11.Status(code=a11.StatusCode.CANCELLED, message="test over")
        )
        with contextlib.suppress(asyncio.CancelledError, Exception):
            await asyncio.wait_for(self._serving, timeout=10)
        with contextlib.suppress(Exception):
            await self.service.aclose(timeout=timing.Duration.seconds(5))


@pytest_asyncio.fixture
async def peer(tmp_path: pathlib.Path):
    """The demo service over an in-process pair, and a client attached to it."""
    peers: list[_Peer] = []

    async def connect(registry: a11.ActionRegistry | None = None) -> _Peer:
        store = conversations.ConversationStore(tmp_path)
        service = demos.make_service(
            demos.make_registry(store, text_to_image=False)
        )
        server_stream, client_stream = net.create_in_process_wire_stream_pair()
        # `accept` hands back a native future, not a coroutine.
        task = asyncio.ensure_future(service.accept(server_stream))
        session = Session(action_registry=registry or a11.ActionRegistry())
        await session.add_stream(client_stream, mode="start")
        made = _Peer(session, client_stream, store, service, task)
        peers.append(made)
        return made

    yield connect

    for made in peers:
        await made.close()


def _user(text: str) -> Interaction:
    return Interaction(
        role=Role.USER,
        content=[a11.to_chunk({"role": "user", "content": text})],
    )


async def _one_turn(
    peer: _Peer,
    question: Interaction,
    *,
    allowed: bytes = b"",
    tools: list[dict] | None = None,
) -> tuple[str, list[Interaction]]:
    """One chat turn, read the way the guides' page reads it."""
    call = peer.action(INTERACT_WITH_LLM_SCHEMA)
    call.set_header(LlmHeaders.PROVIDER.value, b"ollama")
    call.set_header(LlmHeaders.MODEL.value, b"fake")
    if allowed:
        call.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, allowed)
    await call.call()

    await call["interactions"].finalize(question)
    tools_port = call["tools"]
    for tool in tools or []:
        await tools_port.put(tool)
    await tools_port.finalize()
    await call["config"].finalize()

    text: list[str] = []
    produced: list[Interaction] = []

    async def read_text() -> None:
        async for token in call["text_output"]:
            text.append(str(token))

    async def read_new() -> None:
        async for interaction in call["new_interactions"]:
            produced.append(interaction)

    async def drain(port: str) -> None:
        async for _ in call[port].iter_fragments():
            pass

    await asyncio.wait_for(
        asyncio.gather(
            read_text(), read_new(), drain("thoughts"), drain("event_stream")
        ),
        timeout=_TIMEOUT,
    )
    await asyncio.wait_for(call.wait(), timeout=_TIMEOUT)
    return "".join(text), produced


# --- The chat guide ----------------------------------------------------------


@pytest.mark.asyncio
async def test_a_turn_is_recorded_and_read_back_after_a_reload(
    peer, fake_ollama
):
    """What makes a reloaded page continue rather than start over."""
    fake_ollama(lambda asked: _says("a node is a stream."))
    connected = await peer()

    question = _user("what is a node?")
    answer, produced = await _one_turn(connected, question)
    assert answer == "a node is a stream."

    # The conversation itself, which is the reload.
    reading = connected.action(conversation_actions.GET_CONVERSATION_SCHEMA)
    await reading.call()
    await reading["id"].finalize(question.id)
    restored = [interaction async for interaction in reading["interactions"]]
    await asyncio.wait_for(reading.wait(), timeout=_TIMEOUT)
    assert [i.id for i in restored] == [
        question.id,
        *[i.id for i in produced],
    ]


# --- The deep-research guide -------------------------------------------------

_PLAN = "\n".join((
    "Find out what a fiber is.",
    "Find out what a node is.",
    "FINALLY: write both up in one paragraph.",
))


def _research_reply(asked: str) -> list[ollama.ChatResponse]:
    """Answer as whichever step of the composition is asking."""
    if "Here is what the investigations found" in asked:
        return _says("fibers and nodes, in one paragraph.")
    if "Your brief is" in asked:
        brief = asked.split("Your brief is:", 1)[1].strip().splitlines()[0]
        return _says(f"findings for {brief}")
    return _says(_PLAN)


@pytest.mark.asyncio
async def test_deep_research_plans_investigates_and_synthesises(
    peer, fake_ollama
):
    """The whole composition, over a wire, as the page dispatches it."""
    fake = fake_ollama(_research_reply)
    connected = await peer()

    research = connected.action(
        demos.deep_research_program()["deep-research"].schema
    )
    # The provider is named once, on the composition. Every model call inside it
    # is a nested action, and A11 hands a nested action its parent's `x-a11-`
    # headers -- which is why the flow says nothing about providers at all.
    research.set_header(LlmHeaders.PROVIDER.value, b"ollama")
    research.set_header(LlmHeaders.MODEL.value, b"fake")
    # Claimed before the call, which is when the port has to be held: a log
    # written before anything reads it goes to the process sink and is gone.
    # The composition narrates itself *here* and on no output port, which is
    # what lets a client that knows nothing about it -- the console at
    # a11.to/ui, showing it in its log panel -- follow a slow run.
    log_node = research.get_log_node()
    await research.call()
    await research["topic"].finalize("fibers and nodes")

    report: list[str] = []
    plan: list[str] = []
    logged: list[str] = []

    async def collect_logs() -> None:
        # A separate task, never awaited to completion: nothing finalizes the
        # log port, so this only ends when the reader is cancelled.
        async for chunk in log_node.iter_chunks(
            timeout=timing.Duration.seconds(_TIMEOUT)
        ):
            logged.append(str(bytes(chunk.data), "utf-8", "replace"))

    async def collect(port: str, into: list[str]) -> None:
        async for value in research[port]:
            into.append(str(value))

    narrating = asyncio.ensure_future(collect_logs())
    await asyncio.wait_for(
        asyncio.gather(
            collect("report", report),
            collect("plan", plan),
        ),
        timeout=_TIMEOUT,
    )
    await asyncio.wait_for(research.wait(), timeout=_TIMEOUT)
    narrating.cancel()
    with contextlib.suppress(asyncio.CancelledError, Exception):
        await narrating

    # The planner's briefs, minus the one about writing the report.
    assert plan == [
        "Find out what a fiber is.",
        "Find out what a node is.",
    ]
    assert "".join(report) == "fibers and nodes, in one paragraph."

    # One model call to plan, one per brief, one to synthesise.
    assert len(fake.prompts) == 4
    # And the synthesis saw both investigations' findings, which is the only
    # place in the composition where anything waits for anything.
    synthesis = [p for p in fake.prompts if "investigations found" in p]
    assert len(synthesis) == 1
    assert "findings for Find out what a fiber is." in synthesis[0]
    assert "findings for Find out what a node is." in synthesis[0]

    # The narration a page shows while it waits, from every step -- all of it on
    # the reserved log port, and none of it on an output the caller has to know
    # the name of.
    narrated = "\n".join(logged)
    assert "[plan] planning research on: fibers and nodes" in narrated
    assert "[investigate] Find out what a node is." in narrated
    assert "[synthesise] writing the report on: fibers and nodes" in narrated
    assert "user_log" not in research.get_schema().outputs


def test_the_research_flows_advertise_the_local_ollama(
    tmp_path: pathlib.Path,
):
    """Registered with the provider filled in, so the console can just call it.

    A flow declares no headers -- a composition is configured by the call that
    started it -- so the console had no field to fill and an unconfigured call
    failed with "no provider". These are *this deployment's* answer to that,
    which is why they are added at registration and not in the `.flow` file.

    A default here is applied and not merely advertised, so what this pins is
    the whole behaviour: the value reaches the action, and the console reads the
    same value out of the schema to put in front of somebody.
    """
    store = conversations.ConversationStore(tmp_path)
    registry = demos.make_registry(store, text_to_image=False)

    for name in demos.deep_research_program().names:
        headers = registry.get_schema(name).headers
        for header, expected in demos.OLLAMA_DEFAULTS.items():
            assert bytes(headers[header].default) == expected.encode(), name
        # Declared with no default: a local provider needs none, and a key with
        # a default would be a key nobody meant to send.
        assert headers[LlmHeaders.API_KEY].default is None

    # The compiled program is untouched: it is the file, and the file says
    # nothing about providers. Anything checking a flow's ports against it --
    # the browser declares `deep-research`'s by hand -- keeps working.
    for name in demos.deep_research_program().names:
        assert not dict(demos.deep_research_program()[name].schema.headers)

    # And the chat guide's action is left exactly as it was: a turn there is
    # routed by what the page sends, and a default would silently answer from
    # somewhere else when it sent nothing.
    for name in ("interact_with_llm", "ask_model"):
        headers = registry.get_schema(name).headers
        assert LlmHeaders.BASE_URL not in headers
        assert headers[LlmHeaders.PROVIDER].default is None


# --- The browser-tools guide -------------------------------------------------

#: A port per argument. The model tool definition is derived from these ports
#: through [ToolAdapter][a11.sdk.llm_tools.adapter.ToolAdapter]. A streaming
#: port becomes an array.
SET_COLOR_SCHEMA = a11.ActionSchema(
    name="set_color",
    description="Recolour the blobs with the given ids.",
    inputs={
        "ids": a11.ActionPortSchema(
            name="ids",
            type="application/json",
            typeinfo=int,
            required=True,
            description="Which blobs to recolour.",
        ),
        "colors": a11.ActionPortSchema(
            name="colors",
            type="text/plain",
            typeinfo=str,
            required=True,
            description="One `#rrggbb` per id, in the same order.",
        ),
    },
    outputs={
        "recoloured": a11.ActionPortSchema(
            name="recoloured",
            type="application/json",
            typeinfo=int,
            unary=True,
            required=True,
            description="How many blobs changed.",
        )
    },
)


@pytest.mark.asyncio
async def test_an_action_the_page_serves_is_called_by_the_model(
    peer, fake_ollama
):
    """The model's tool call runs in the page, over the page's own socket."""
    ran: list[tuple[list[int], list[str]]] = []

    async def set_color(action: a11.Action) -> None:
        ids = [int(value) async for value in action["ids"]]
        colors = [str(value) async for value in action["colors"]]
        ran.append((ids, colors))
        await action["recoloured"].finalize(len(ids))

    def reply(asked: str) -> list[ollama.ChatResponse]:
        if "recoloured" in asked:
            return _says("done.")
        return _calls("set_color", {"ids": [2], "colors": ["#ff0044"]})

    fake = fake_ollama(reply)

    mine = a11.ActionRegistry()
    mine.register(SET_COLOR_SCHEMA.name, SET_COLOR_SCHEMA, set_color)
    connected = await peer(mine)

    # The page registers `set_color` on its session registry. The server reads
    # the available actions from that registry when a turn first needs tools.
    answer, _ = await _one_turn(
        connected, _user("make blob 2 red"), allowed=b"set_color"
    )

    assert answer == "done."
    assert "set_color" in fake.tools_offered
    # The handler ran here, with the arguments the model chose.
    assert ran == [([2], ["#ff0044"])]


# --- Being served by `a11 serve` ----------------------------------------------


@pytest.mark.asyncio
async def test_the_module_is_a_target_a11_serve_can_resolve(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The whole of how this is started: one symbol `a11 serve` reads.

    And it has to be the *service*, not the registry: each connection here gets
    a copy with the caller's own tools registered on it, which is an
    `on_connection` hook and cannot travel on a registry.

    Resolved with a loop running, as the command does it: building a native
    `Service` needs one, and without it this passes alone and fails after any
    test that has already used a loop.
    """
    monkeypatch.setattr(demos, "_SERVICE", None)
    monkeypatch.setenv(demos.CONVERSATION_ROOT_ENV, str(tmp_path))
    monkeypatch.setenv(demos.TEXT_TO_IMAGE_ENV, "0")

    served = serve_command.resolve_served(demos.TARGET)
    assert served.service is not None
    assert "interact_with_llm" in served.registry.list_registered_actions()
    # The store went where it was told, rather than into the user's cache.
    assert (tmp_path / "conversations.sqlite").exists()


def test_the_service_is_not_built_by_importing_the_module(
    tmp_path: pathlib.Path,
) -> None:
    """Importing must not create a conversation store on disk.

    Which is why the symbol is lazy: `python -m` imports this module before its
    own flags -- the ones that say *where* the store goes -- have been read, so
    a service built at import would record into the wrong place and a second one
    would be built for the right one. In a subprocess because "what a fresh
    import does" is not answerable in a process that has already done it.
    """
    root = tmp_path / "unwanted"
    environment = dict(os.environ, **{demos.CONVERSATION_ROOT_ENV: str(root)})
    finished = subprocess.run(
        [
            sys.executable,
            "-c",
            "import a11.demos.web_demos_server as d; print(d._SERVICE)",
        ],
        capture_output=True,
        text=True,
        env=environment,
        timeout=_TIMEOUT,
    )

    assert finished.returncode == 0, finished.stderr
    assert finished.stdout.strip() == "None"
    assert not root.exists()


def test_the_hosting_flags_are_a11_serves_own(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """One vocabulary of transport flags, whichever entry point was typed.

    The two defaults that differ are the ones the guides quote, so a page whose
    instructions say 9010 is right.
    """
    parser = argparse.ArgumentParser()
    demos._configure(parser)
    args = parser.parse_args(["--ws", "--hosted", "demoserver"])

    assert args.target == demos.TARGET
    assert (args.ws_port, args.ws_path) == (
        demos.DEFAULT_PORT,
        demos.DEFAULT_PATH,
    )
    assert args.hosted == "demoserver"


def test_its_own_flags_reach_the_service_through_the_environment(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """`--conversation-root` has to arrive before the symbol is read."""
    monkeypatch.setattr(demos, "_SERVICE", None)
    monkeypatch.delenv(demos.CONVERSATION_ROOT_ENV, raising=False)
    monkeypatch.delenv(demos.TEXT_TO_IMAGE_ENV, raising=False)
    ran: list[argparse.Namespace] = []

    async def fake_run(args: argparse.Namespace) -> int:
        # Read where the real command reads it: inside the loop, because a
        # native Service binds to the loop it is built on.
        ran.append(args)
        actions = demos.SERVICE.action_registry.list_registered_actions()
        assert "text_to_image" not in actions
        assert "deep-research" in actions
        return 0

    monkeypatch.setattr(serve_command, "run", fake_run)
    code = demos.main([
        "--conversation-root",
        str(tmp_path / "here"),
        "--no-text-to-image",
    ])

    assert code == 0
    assert ran and ran[0].target == demos.TARGET
    assert os.environ[demos.CONVERSATION_ROOT_ENV] == str(tmp_path / "here")
    assert os.environ[demos.TEXT_TO_IMAGE_ENV] == "0"


# --- The transport the pages use ---------------------------------------------


@pytest.mark.asyncio
async def test_a_browser_style_websocket_client_reaches_the_demo_actions(
    tmp_path: pathlib.Path,
):
    """The listener `serve` binds, connected to the way a page connects."""
    store = conversations.ConversationStore(tmp_path)
    service = demos.make_service(
        demos.make_registry(store, text_to_image=False)
    )

    options = net.WebSocketServerOptions()
    options.bind_address = "127.0.0.1"
    options.port = 0
    options.path = demos.DEFAULT_PATH

    async with serving(service, websocket(options)) as (listener,):
        stream = net.WebSocketWireStream.connect(
            f"ws://127.0.0.1:{listener.port}{demos.DEFAULT_PATH}",
            websocket_options=net.WebSocketClientOptions(),
        )
        session = Session(action_registry=a11.ActionRegistry())
        await asyncio.wait_for(
            session.add_stream(stream, mode="start"), timeout=_TIMEOUT
        )
        try:
            reading = (
                a11
                .Action(conversation_actions.GET_CONVERSATION_SCHEMA)
                .bind_node_map(session.node_map)
                .bind_session(session)
                .bind_stream(stream)
            )
            await reading.call()
            await reading["id"].finalize("nonexistent")
            restored = [i async for i in reading["interactions"]]
            await asyncio.wait_for(reading.wait(), timeout=_TIMEOUT)
            # Nothing has been said yet; an empty conversation is the
            # answer, and getting one over a WebSocket is the point.
            assert restored == []
        finally:
            # Teardown: the page going away. `serving` stops the listener and
            # drains the service on the way out of the block, so this only has
            # to let go of the socket -- and is bounded because a session whose
            # peer is being torn down underneath it need not finish cleanly.
            session.half_close()
            with contextlib.suppress(Exception):
                await asyncio.wait_for(session.done.wait(), timeout=5)
