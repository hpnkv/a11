# Copyright 2026 The A11 Authors.

"""The flow in ``scripts/flow_playground.py``, run against fake actions.

The script needs a microphone, a gateway and a model, so nothing about it can
be exercised by hand in CI -- and a playground that quietly stops working is
worse than none.

The topology is the thing being checked, because it is the thing the example is
about. The composition runs on the **gateway**, and it uses both verbs:

* `call capture_audio` is dispatched back to the **client**, which is where the
  microphone is. Both ends register `capture_audio`; only the client registers
  a handler for it, and the gateway would happily have run its own if the flow
  had said `run`. That is the assertion that matters -- the fake that speaks is
  the client's.
* `run transcribe_audio` and `run interact_with_llm` are the gateway's, and the
  audio buffers never come back over the wire.

The reply is read off the published output node while the flow is still
running, because "the client reads it as the model writes it" is a property of
the example rather than an implementation detail.

The other property worth a test is the **conversation**: the flow is stateless,
so the history is the script's, and what the model sees on the second turn is
whatever came back from the first. Nothing about that is visible from one turn,
which is why the loop is exercised here rather than only a single call.
"""

import asyncio
import contextlib
import importlib.util
import pathlib

import pytest
import pytest_asyncio

import a11
from a11 import net
from a11.actions import Action, ActionRegistry
from a11.sdk import flow_tools
from a11.sdk.audio import actions as audio_actions
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.sdk.llm import Interaction, LlmHeaders, Role

SCRIPT = pathlib.Path(__file__).parents[4] / "scripts" / "flow_playground.py"

HEADERS = {
    LlmHeaders.PROVIDER.value: "claude",
    LlmHeaders.MODEL.value: "a-model",
    LlmHeaders.API_KEY.value: "a-key",
    LlmHeaders.BASE_URL.value: "",
}


@pytest.fixture(scope="module")
def playground():
    """The script, loaded from disk: `scripts/` is not an importable package."""
    if not SCRIPT.is_file():
        pytest.skip("the playground script is not in this checkout")
    spec = importlib.util.spec_from_file_location("flow_playground", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest_asyncio.fixture
async def wired():
    """A gateway serving the flow tools, and a client serving a microphone."""
    heard: dict = {}

    # --- the client's half: the microphone ---------------------------------

    async def fake_mic(action: Action) -> None:
        """Emit buffers until told to stop, which is what a microphone does.

        Continuously rather than a fixed handful: an open node holds its last
        fragment until there is more to send or it is closed, and this one is
        not closed until the flow has its answer. A fake that wrote three
        buffers and waited would have its third still in the buffer, and the
        composition would be waiting on the sentence it completes.
        """
        heard["capture_ran_on"] = "client"
        await action["options"].consume(dict, allow_none=True)
        stopping = asyncio.Event()

        async def emit() -> None:
            index = 0
            while not stopping.is_set():
                await (await action["audio"].put(f"buffer-{index}".encode()))
                index += 1
                heard["written"] = index
                await asyncio.sleep(0.02)

        emitting = asyncio.ensure_future(emit())
        heard["stop"] = await action["control_events"].next_object()
        stopping.set()
        await emitting
        for port in ("audio", "events"):
            await action[port].drain_and_close()

    mine = ActionRegistry()
    mine.register(
        audio_actions.CAPTURE_AUDIO,
        audio_actions.CAPTURE_AUDIO_SCHEMA,
        fake_mic,
    )

    # --- the gateway's half: recognition and the model ---------------------

    async def fake_asr(action: Action) -> None:
        """Decode as the audio arrives, which the real one also does.

        It has to: the flow only tells the microphone to stop once the model
        has answered, so the audio stream does not end until after a sentence
        has been recognised. A fake that drained the audio first would wait
        for an ending that this composition is waiting on *it* to cause.
        """
        heard["asr_ran_on"] = "gateway"
        await action["asr_options"].consume(dict, allow_none=True)
        spoken = ("um", "so what is", "a fiber?")
        count = 0
        async for _ in action["audio"]:
            if count < len(spoken):
                await (await action["transcription_pieces"].put(spoken[count]))
            count += 1
            heard["buffers"] = count
        assert count >= len(spoken), "the microphone stopped too early"
        # Once the microphone really has stopped, one last thing nobody wants.
        await (await action["transcription_pieces"].put("never mind"))
        for port in ("transcription_pieces", "events"):
            await action[port].drain_and_close()

    async def fake_llm(action: Action) -> None:
        """Answer, and hand back an interaction shaped the way a backend's is.

        `role` and a `content` list inside the chunk, because that is what the
        real backends write (`snapshot.model_dump()`) and what they read back
        out of a replayed interaction. A conversation only survives a round trip
        if the fake keeps to that shape.
        """
        asked = []
        async for interaction in action["interactions"]:
            message = a11.from_chunk(interaction.content[0])
            asked.append(
                f"{message['role']}: {message['content'][0]['text']}"
            )
        await action["config"].consume(dict, allow_none=True)
        heard["asked"] = asked
        heard.setdefault("turns", []).append(asked)
        heard["model"] = action.get_header(LlmHeaders.MODEL.value, decode=True)
        answer = f"a stack a scheduler can park (#{len(heard['turns'])})."
        for word in ("a stack ", answer[len("a stack ") :]):
            await (await action["text_output"].put(word))
        await action["text_output"].drain_and_close()
        await (
            await action["new_interactions"].put(
                Interaction(
                    # A11 spells the model's own role `model`; the *message*
                    # inside keeps whatever the provider calls it.
                    role=Role.ASSISTANT,
                    content=[
                        a11.to_chunk(
                            {
                                "role": "assistant",
                                "content": [{"type": "text", "text": answer}],
                            }
                        )
                    ],
                ),
                final=True,
            )
        )
        for port in ("new_interactions", "thoughts", "event_stream"):
            await action[port].drain_and_close()

    theirs = ActionRegistry()
    theirs.register(
        audio_actions.TRANSCRIBE_AUDIO,
        audio_actions.TRANSCRIBE_AUDIO_SCHEMA,
        fake_asr,
    )
    theirs.register(
        INTERACT_WITH_LLM_SCHEMA.name, INTERACT_WITH_LLM_SCHEMA, fake_llm
    )
    # The gateway knows `capture_audio` too, with a microphone of its own. The
    # flow says `call`, so this one must never be the one that runs.
    async def wrong_mic(action: Action) -> None:
        heard["capture_ran_on"] = "gateway"
        raise AssertionError("the gateway's microphone should not be opened")

    theirs.register(
        audio_actions.CAPTURE_AUDIO,
        audio_actions.CAPTURE_AUDIO_SCHEMA,
        wrong_mic,
    )
    flow_tools.register(theirs)

    service = a11.Service(action_registry=theirs)
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.ensure_future(service.accept(server_stream))
    client = a11.Session(action_registry=mine)
    await client.add_stream(client_stream, mode="start")

    yield client, client_stream, heard

    with contextlib.suppress(Exception):
        await client_stream.close()
    service.abort(
        a11.Status(code=a11.StatusCode.CANCELLED, message="test over")
    )
    with contextlib.suppress(Exception):
        await asyncio.wait_for(serving, timeout=10)


@pytest.mark.asyncio
async def test_the_first_full_sentence_becomes_the_question(
    playground, wired
):
    client, stream, heard = wired

    call = (
        a11.Action(flow_tools.FLOW_RUN_SCHEMA)
        .bind_node_map(client.node_map)
        .bind_session(client)
        .bind_stream(stream)
    )
    for name, value in HEADERS.items():
        call.set_header(name, value.encode())
    await call.call()

    def published(port: str):
        return client.node_map.get(
            flow_tools.flow_output_node_id(call.get_id(), port)
        )

    reply_node = published("reply")
    sentence_node = published("sentence")
    turn_node = published("turn")

    # An empty conversation, on a node of this client's: the flow reads the
    # turns so far off it, and a first turn has none. Closed before the flow is
    # dispatched, because the flow reads it to the end before the new question.
    history = client.node_map.get("first-turn-history")
    history.attach_stream(stream)
    await history.put_null_final()

    # Every input port of the call, including the ones it has nothing for: one
    # that is neither written nor closed is one the handler waits on.
    async with (
        call["source"] as source,
        call["inputs"] as inputs,
        call["flow"] as which,
        call["input_streams"] as streamed,
    ):
        await source.put_final(playground.FLOW_SOURCE)
        await inputs.put_final(
            {
                "asr": {"model": "fake.bin"},
                "device": {},
                "history": history.get_id(),
            }
        )
        await which.put_null_final()
        await streamed.put_null_final()

    # Read off the published nodes rather than out of `result`: this is the
    # client watching the answer being written.
    sentence = await asyncio.wait_for(
        sentence_node.next_object(), timeout=30
    )
    reply = [
        await asyncio.wait_for(reply_node.next_object(), timeout=30)
        for _ in range(2)
    ]
    # The turn to remember, read as the client reads it: off the node, where a
    # value arrives as the `Interaction` it was written as. (Inside `result` the
    # same values are one JSON object deep, and are plain records there.)
    remembered = [
        await asyncio.wait_for(turn_node.next_object(), timeout=30)
        for _ in range(2)
    ]

    result = await asyncio.wait_for(call["result"].next_object(), timeout=30)
    await asyncio.wait_for(call.wait(), timeout=30)

    # The microphone that opened was the client's, not the gateway's. This is
    # the `call`/`run` distinction doing the only job it exists for.
    assert heard["capture_ran_on"] == "client"
    assert heard["asr_ran_on"] == "gateway"
    assert heard["buffers"] == 3

    # The fragments are accumulated: the question is everything said up to and
    # including the piece that ended the sentence, and `first 1` means "never
    # mind" never becomes a second question.
    assert heard["asked"] == ["user: um so what is a fiber?"]
    assert sentence == "um so what is a fiber?"
    assert reply == ["a stack ", "a scheduler can park (#1)."]
    # The transcription itself stays on the gateway: it goes through the
    # `scratch` node map, so the caller is sent the sentence, the answer, and
    # the turn to remember -- and nothing else.
    assert set(result) == {"sentence", "reply", "turn"}
    # The turn to remember is the question and then the answer, in that order:
    # `then` is what puts them in it.
    assert [
        a11.from_chunk(one.content[0])["content"][0]["text"]
        for one in remembered
    ] == ["um so what is a fiber?", "a stack a scheduler can park (#1)."]
    assert [str(one.role.value) for one in remembered] == ["user", "model"]
    # The headers the flow forwards are the ones the model was chosen with.
    assert heard["model"] == "a-model"
    # And the client's microphone was told to stop, by the flow, after the
    # question -- the stop event travelled back across the wire.
    assert heard["stop"] == {"command": "stop"}


def test_the_script_feeds_the_ports_the_flow_declares(playground):
    """The `inputs` object names the flow's input ports, and holds plain data.

    Both halves of that have bitten this script. A port name it invents is a
    required input nobody feeds; a registered type left in the dict is an
    *"objects of type SpeechRecognizerOptions cannot be serialized"* the moment
    it is written, because ``inputs`` is one JSON object and the value inside it
    reaches the JSON codec rather than its own.
    """
    from a11 import flow
    from a11.sdk.audio import AudioInputOptions, SpeechRecognizerOptions

    declared = flow.loads(playground.FLOW_SOURCE, "playground.flow").main.inputs
    fed = playground.flow_inputs(
        SpeechRecognizerOptions(model="fake.bin"),
        AudioInputOptions(),
        "some-history-node",
    )
    assert set(fed) == set(declared)
    for name, value in fed.items():
        assert isinstance(value, (dict, list, str)), f"{name} is plain data"
    # And it survives the trip a node would take it on, which is where a
    # registered type left inside it would have raised.
    assert a11.to_chunk(fed) is not None


def _script_console(written):
    """A console the test can read back, in place of the terminal's."""
    from rich.console import Console

    return Console(file=written, force_terminal=False, width=200)


@pytest.mark.asyncio
async def test_the_script_chats_and_each_turn_carries_the_ones_before_it(
    playground, wired
):
    """The script's own functions, two turns, against the fakes.

    The playground cannot be exercised by hand in CI, and every failure it has
    had was of the same kind: a port read that the flow does not declare, a port
    fed that it does not have, and then silence. So the script's own
    `check_the_flow` and `chat` run here, and what they print -- plus what the
    fake model was *asked* -- is the assertion.

    The second turn is the point: what the model sees is the first question, the
    first answer, and then the new question, in that order. The flow built that
    from `history` and `then`; the script only kept the interactions.
    """
    import io

    from a11.sdk.audio import AudioInputOptions, SpeechRecognizerOptions

    client, stream, heard = wired
    if not playground.FLAGS.is_parsed():
        playground.FLAGS(["flow_playground"])
    playground.FLAGS.listen_seconds = 30
    playground.FLAGS.turns = 2

    written = io.StringIO()
    console = _script_console(written)

    # Both of the read-only tools first, exactly as the script's `run` does.
    await asyncio.wait_for(
        playground.show_what_is_composable(client, stream, console), timeout=30
    )
    described = await asyncio.wait_for(
        playground.check_the_flow(client, stream, console), timeout=30
    )
    assert described["flow"] == "interact-on-full-sentence"

    # `reply` is printed with a bare `print`, token by token, so redirect that
    # too: it is the one thing a person watching the model actually reads.
    with contextlib.redirect_stdout(written):
        await asyncio.wait_for(
            playground.chat(
                client,
                stream,
                HEADERS,
                SpeechRecognizerOptions(model="fake.bin"),
                AudioInputOptions(),
                described,
                console,
            ),
            timeout=90,
        )

    shown = written.getvalue()
    assert "um so what is a fiber?" in shown, shown
    assert "a stack a scheduler can park (#1)." in shown, shown
    assert "nothing that ended a sentence was heard" not in shown, shown
    assert "turn 2" in shown and "remembering 2 interaction(s)" in shown, shown
    # The flow really ran the composition, not just the compile.
    assert heard["capture_ran_on"] == "client"
    assert heard["model"] == "a-model"

    # Turn one asked the question alone; turn two asked it again with the whole
    # conversation in front of it, oldest first. The roles survive the round
    # trip through `history` -- including the model's own, which the provider
    # message spells `assistant` where A11 spells it `model`.
    assert heard["turns"] == [
        ["user: um so what is a fiber?"],
        [
            "user: um so what is a fiber?",
            "assistant: a stack a scheduler can park (#1).",
            "user: um so what is a fiber?",
        ],
    ]
