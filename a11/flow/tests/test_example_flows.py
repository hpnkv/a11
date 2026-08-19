"""The flows in ``examples/003-flow-dsl`` that need a gateway, run on fakes.

Three of the example files compose actions a real ``a11 gateway run`` serves --
its shell, its microphone, and ``interact_with_llm`` -- so nobody can run them
in a test, and an example that quietly stops working is worse than none. Here
the *real* schemas of those actions are served by a peer whose handlers only
pretend, which checks the parts that rot: port names, the shape of the prompt
each flow builds, and the wiring between the steps.

The peer is a genuine one, over `create_in_process_wire_stream_pair`, because
these flows say `call` for the gateway's actions and `call` means dispatch. The
flow's own registry holds the toy actions it `run`s and the *schemas* -- no
handlers -- of the ones it `call`s, which is exactly what a client composing a
gateway's actions has, and is what makes the two verbs distinguishable at all.

The ones that need nothing (``research``, ``triage``, ``recover``) are covered
by ``test_the_example_flows_compile`` and by running ``main.py``.
"""

import asyncio
import contextlib
import importlib.util
import pathlib

import pytest
import pytest_asyncio

import a11
from a11 import flow, net, timing
from a11.actions import Action, ActionRegistry
from a11.sdk import bash
from a11.sdk.audio import actions as audio_actions
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.sdk.llm import Interaction, LlmHeaders

# The example flows name `a11.sdk.Interaction`; a tag resolves only to a type
# this process has imported.
from a11.sdk.llm import Interaction as _Interaction  # noqa: F401

EXAMPLES = pathlib.Path(__file__).parents[3] / "examples" / "003-flow-dsl"

HEADERS = {
    LlmHeaders.PROVIDER.value: "claude",
    LlmHeaders.MODEL.value: "a-model",
    LlmHeaders.API_KEY.value: "a-key",
    LlmHeaders.BASE_URL.value: "",
}


@pytest.fixture(scope="module")
def toy_actions():
    """The example's own ``actions.py``: `examples/` is not a package."""
    path = EXAMPLES / "actions.py"
    if not path.is_file():
        pytest.skip("the flow examples are not in this checkout")
    spec = importlib.util.spec_from_file_location("example_actions", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest_asyncio.fixture
async def registry(toy_actions):
    """The example's toy actions here, and stand-ins for the gateway's there.

    Yields the registry the flow runs against. Its ``session`` and ``stream``
    attributes are the connection to the peer, which [invoke][] hands to the
    flow so its `call` steps have somewhere to go.
    """
    seen: dict = {}

    def asked(interaction: Interaction) -> str:
        return a11.from_chunk(interaction.content[0])["content"][0]["text"]

    async def fake_llm(action: Action) -> None:
        async for interaction in action["interactions"]:
            seen.setdefault("prompts", []).append(asked(interaction))
        await action["config"].consume(dict, allow_none=True)
        seen["model"] = action.get_header(LlmHeaders.MODEL.value, decode=True)
        for word in ("as far as ", "I can tell."):
            await (await action["text_output"].put(word))
        await action["text_output"].drain_and_close()
        # What a real one hands back for the next turn to carry.
        await (
            await action["new_interactions"].put(
                Interaction(
                    content=[
                        a11.to_chunk(
                            {
                                "role": "assistant",
                                "content": [
                                    {"type": "text", "text": "I answered."}
                                ],
                            }
                        )
                    ]
                ),
                final=True,
            )
        )
        for port in ("new_interactions", "thoughts", "event_stream"):
            await action[port].drain_and_close()

    async def fake_shell(action: Action) -> None:
        seen["command"] = await action["command"].consume(str, allow_none=True)
        await action["parameters"].consume(dict, allow_none=True)
        for line in (" M cpp/a11/flow/parser.cc", "?? scripts/", ""):
            await (await action["output_lines"].put(line))
        await action["output_lines"].drain_and_close()

    async def fake_mic(action: Action) -> None:
        await action["options"].consume(dict, allow_none=True)
        await (await action["audio"].put(b"\x00\x01"))
        # Held open until the caller says stop, as the real one is.
        seen["stop"] = await action["control_events"].next_object()
        for port in ("audio", "events"):
            await action[port].drain_and_close()

    async def fake_asr(action: Action) -> None:
        await action["asr_options"].consume(dict, allow_none=True)
        async for _ in action["audio"]:
            pass
        for piece in ("a note about", "fibers.", "and one about", "nodes."):
            await (await action["transcription_pieces"].put(piece))
        for port in ("transcription_pieces", "events"):
            await action[port].drain_and_close()

    #: The gateway's actions, and who pretends to be each of them.
    served = (
        (INTERACT_WITH_LLM_SCHEMA.name, INTERACT_WITH_LLM_SCHEMA, fake_llm),
        (bash.SHELL_EXECUTE_SCHEMA.name, bash.SHELL_EXECUTE_SCHEMA, fake_shell),
        (
            audio_actions.CAPTURE_AUDIO,
            audio_actions.CAPTURE_AUDIO_SCHEMA,
            fake_mic,
        ),
        (
            audio_actions.TRANSCRIBE_AUDIO,
            audio_actions.TRANSCRIBE_AUDIO_SCHEMA,
            fake_asr,
        ),
    )

    theirs = ActionRegistry()
    for name, schema, handler in served:
        theirs.register(name, schema, handler)

    # Schemas without handlers: enough to know the port names, and the plain
    # statement that this work is not ours to do.
    built = toy_actions.make_registry()
    for name, schema, _ in served:
        built.register(name, schema)
    built.seen = seen  # type: ignore[attr-defined]

    service = a11.Service(action_registry=theirs)
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.ensure_future(service.accept(server_stream))
    session = a11.Session(action_registry=ActionRegistry())
    await session.add_stream(client_stream, mode="start")
    built.session = session  # type: ignore[attr-defined]
    built.stream = client_stream  # type: ignore[attr-defined]

    yield built

    with contextlib.suppress(Exception):
        await client_stream.close()
    service.abort(
        a11.Status(code=a11.StatusCode.CANCELLED, message="test over")
    )
    with contextlib.suppress(Exception):
        await asyncio.wait_for(serving, timeout=10)


def load(name: str, registry: ActionRegistry):
    program = flow.load(EXAMPLES / name)
    program.register_all(registry)
    return program


async def invoke(program, name: str, registry, inputs=None, **options):
    """Run one flow the way a client runs it: `run` here, `call` on the peer."""
    return await program[name].invoke(
        inputs or {},
        registry=registry,
        session=registry.session,
        dispatch_stream=registry.stream,
        timeout=timing.Duration.seconds(30),
        **options,
    )


@pytest.mark.asyncio
async def test_ask_the_pages_builds_one_prompt_out_of_the_pages(
    registry, toy_actions
):
    """Retrieval here, model there, and the prompt the flow made itself."""
    program = load("assistant.flow", registry)
    result = await invoke(
        program,
        "ask-the-pages",
        registry,
        {"question": "how do nodes and actions stream"},
        headers=HEADERS,
    )
    assert result["answer"] == "as far as I can tell."
    assert result["cited"]
    prompt = registry.seen["prompts"][0]
    # One prompt, holding the question and the pages the search turned up.
    assert registry.seen["prompts"] == [result["prompt"]]
    assert "how do nodes and actions stream" in prompt
    assert "AsyncNode" in prompt
    # Trimmed on the way in: the corpus is far bigger than what was sent.
    assert len(prompt) < toy_actions.raw_corpus_size()


@pytest.mark.asyncio
async def test_chat_turn_keeps_the_conversation_in_order(registry):
    """`then` is the whole reason a second turn is possible."""
    program = load("assistant.flow", registry)

    first = await invoke(
        program,
        "chat-turn",
        registry,
        {"history": [], "question": "what is a fiber?"},
        headers=HEADERS,
    )
    assert first["reply"] == "as far as I can tell."
    # The turn is what the caller feeds back in: the question, then the answer.
    assert len(first["turn"]) == 2

    second = await invoke(
        program,
        "chat-turn",
        registry,
        {"history": first["turn"], "question": "and a node?"},
        headers=HEADERS,
    )
    assert second["reply"] == "as far as I can tell."
    # The model saw the whole conversation, oldest first, with the new question
    # last -- which two writers to one port could not have promised.
    said = registry.seen["prompts"]
    assert said == [
        "what is a fiber?",  # turn one
        "what is a fiber?",  # turn two: the history...
        "I answered.",
        "and a node?",  # ...and then what was just asked
    ]


@pytest.mark.asyncio
async def test_quote_the_pages_needs_no_model_at_all(registry):
    program = load("assistant.flow", registry)
    result = await invoke(
        program, "quote-the-pages", registry, {"question": "what are fibers"}
    )
    assert "fiber" in result["answer"].lower()
    assert result["cited"] == ["https://example.test/fibers"]


@pytest.mark.asyncio
async def test_count_changes_reads_one_output_twice(registry):
    program = load("ops.flow", registry)
    result = await invoke(program, "count-changes", registry)
    assert registry.seen["command"] == "git status --porcelain"
    # The blank line the command printed is not a changed file.
    assert result["changed"] == 2
    assert result["files"] == [" M cpp/a11/flow/parser.cc", "?? scripts/"]


@pytest.mark.asyncio
async def test_explain_a_command_sends_the_output_to_the_model(registry):
    program = load("ops.flow", registry)
    result = await invoke(
        program,
        "explain-a-command",
        registry,
        {"command": "git status --porcelain"},
        headers=HEADERS,
    )
    assert result["verdict"] == "as far as I can tell."
    prompt = registry.seen["prompts"][0]
    assert "$ git status --porcelain" in prompt
    assert " M cpp/a11/flow/parser.cc" in prompt
    assert registry.seen["model"] == "a-model"


@pytest.mark.asyncio
async def test_dictate_a_note_turns_utterances_into_sentences(registry):
    """A flow calling a flow, over audio that never leaves the composition."""
    program = load("dictate.flow", registry)
    result = await invoke(
        program,
        "dictate-a-note",
        registry,
        {
            "asr": {"model": "fake.bin"},
            "capture": {},
            "control": [{"command": "stop"}],
        },
        headers=HEADERS,
    )
    # Utterances are gathered into sentences, not passed through one by one.
    assert result["said"] == ["a note about fibers.", "and one about nodes."]
    assert result["note"] == "as far as I can tell."
    assert "a note about fibers. and one about nodes." in (
        registry.seen["prompts"][0]
    )
    # The caller's stop reached the microphone through the inner flow.
    assert registry.seen["stop"] == {"command": "stop"}
