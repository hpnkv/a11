# Copyright 2026 The A11 Authors.

"""Talk to a model, out loud, over a flow that runs on the gateway.

One turn is one `flow_run`: the flow opens this machine's microphone with
`call`, recognises what was said on the gateway with `run`, and answers the
first full sentence. The loop is the conversation -- each turn hands back the
interactions to remember, and the next turn sends them in as `history`, which
the flow concatenates in front of the new question with `then`.

Stay quiet for a turn (or press Ctrl-C) to stop. `--check_only` asks what is
composable and compiles the flow, needing neither a microphone nor a model.
"""

import asyncio
import os
import pathlib
import uuid
from typing import Any, Sequence

from absl import app, flags, logging
from rich.console import Console

import a11
from a11.gateway.config import DEFAULT_GATEWAY_URL
from a11.sdk import flow_tools
from a11.sdk.audio import actions as audio_actions
from a11.sdk.llm import LlmHeaders
from a11.status import StatusException

FLAGS = flags.FLAGS

flags.DEFINE_string("gateway", DEFAULT_GATEWAY_URL, "Gateway to connect to.")
flags.DEFINE_string("provider", "claude", "LLM provider to answer with.")
flags.DEFINE_string("model", "", "Model to answer with; provider default.")
flags.DEFINE_string("voice_model", "", "Whisper model; the CLI default.")
flags.DEFINE_integer("listen_seconds", 120, "How long to wait for a sentence.")
flags.DEFINE_integer(
    "turns", 0, "How many turns to take; 0 keeps going until nothing is said."
)
flags.DEFINE_bool(
    "check_only", False, "Show what is composable and compile, but do not run."
)

#: Distinguishes this run's own nodes from another playground's against the same
#: gateway, since a node id is only as unique as whoever chose it.
RUN_ID = uuid.uuid4().hex[:8]

#: What each provider calls its usual model, and where a key comes from.
DEFAULT_MODEL: dict[str, str] = {
    "claude": "claude-sonnet-4-6",
    "gemini": "gemini-3.5-flash",
    "ollama": "glm-4.7-flash",
}
API_KEY_ENV: dict[str, tuple[str, ...]] = {
    "claude": ("ANTHROPIC_API_KEY",),
    "gemini": ("GEMINI_API_KEY", "GOOGLE_API_KEY"),
}
DEFAULT_BASE_URL: dict[str, str] = {"ollama": "http://localhost:11434"}


# --- The flow -----------------------------------------------------------------

# The composition itself, beside this script so an editor highlights it and
# `flow_check` can be pointed straight at the file.
FLOW_PATH = pathlib.Path(__file__).with_name("interact-on-full-sentence.flow")
FLOW_SOURCE = FLOW_PATH.read_text(encoding="utf-8")


# --- Talking to the gateway ---------------------------------------------------


async def connect() -> tuple[a11.Session, Any]:
    options = a11.WebSocketClientOptions()
    options.http2_options.client_preference = a11.HttpProtocolPreference.HTTP11
    stream = a11.WebSocketWireStream.connect(
        FLAGS.gateway, websocket_options=options
    )
    registry = a11.ActionRegistry()
    audio_actions.register(registry, capture=True, recognition=False)
    session = a11.Session(action_registry=registry)
    await session.add_stream(stream, mode="start")
    return session, stream


def remote(session: a11.Session, stream: Any, schema: a11.ActionSchema):
    """An action bound to run on the other end of the wire."""
    return (
        a11.Action(schema)
        .bind_node_map(session.node_map)
        .bind_session(session)
        .bind_stream(stream)
    )


async def show_what_is_composable(
    session: a11.Session, stream: Any, console: Console
) -> None:
    """`flow_actions`: what a flow may call, and what its ports are called."""
    called = remote(session, stream, flow_tools.FLOW_ACTIONS_SCHEMA)
    await called.call()
    described = await called["actions"].next_object()
    await called.wait()

    console.print("[bold]the gateway will compose[/]")
    for entry in described or []:
        console.print(f"  {entry['action']}", highlight=False)
        for kind in ("inputs", "outputs"):
            named = " ".join(
                port["port"] + ("*" if port.get("stream") else "")
                for port in entry[kind]
            )
            if named:
                console.print(
                    f"    [dim]{kind[:-1]:<6}[/] {named}", highlight=False
                )
    console.print("  [dim]* = a stream; anything else carries one value[/]\n")


async def check_the_flow(
    session: a11.Session, stream: Any, console: Console
) -> dict[str, Any]:
    """`flow_check`: the gateway compiles the source, and runs nothing.

    Returns the description of the flow that will run -- the first one declared,
    which is what `flow_run` picks. Its ports are then read off *that* rather
    than written out here a second time, so a flow that grows an output does not
    leave this script waiting on a node nobody writes.
    """
    call = remote(session, stream, flow_tools.FLOW_CHECK_SCHEMA)
    await call.call()
    async with call["source"] as source:
        await source.put_final(FLOW_SOURCE)
    plan = await call["plan"].next_object()
    await call.wait()

    for compiled in plan["flows"]:
        steps = ", ".join(step["step"] for step in compiled["steps"])
        console.print(
            f"[bold]{compiled['flow']}[/] compiles on the gateway: {steps}\n"
        )
    return plan["flows"][0]


# --- The conversation ---------------------------------------------------------


async def write_history(
    session: a11.Session, stream: Any, turn: int, history: Sequence[Any]
) -> str:
    """Put the conversation on a node of this client's, and say where.

    A node, rather than a value inside ``inputs``. An `Interaction` *does*
    serialize inside that one JSON object -- it is a pydantic model, so the JSON
    codec dumps it -- but it does not come back: a chunk's data dumps as text for
    a JSON mimetype while the wire validator reads base64, so an interaction
    cannot be validated out of its own dump. Sending records the flow rebuilt was
    the alternative, and that is two conversions to lose a field in.

    Written to a node, each interaction is the whole value of its own chunk, so
    its own codec applies and the flow reads back exactly what was sent -- ids,
    tool metadata and all -- as the type it is.

    One node per turn, written and closed *before* the flow is dispatched: the
    flow reads it to the end before the new question, so a node still open would
    be a conversation that never finishes arriving. The turn before's node is
    dropped on the way past, since nothing reads a conversation twice.
    """
    session.node_map.discard(_history_node_id(turn - 1))
    node = session.node_map.get(_history_node_id(turn))
    node.attach_stream(stream)
    for interaction in history:
        await (await node.put(interaction))
    await node.put_null_final()
    return node.get_id()


def _history_node_id(turn: int) -> str:
    return f"chat-{RUN_ID}-history-{turn}"


def flow_inputs(
    asr_options: Any,
    capture_options: Any,
    history_node: str,
) -> dict[str, Any]:
    """A value per flow input port, as plain data.

    ``inputs`` is one JSON object, so a registered type left inside it reaches
    the JSON codec rather than its own -- an *"objects of type
    SpeechRecognizerOptions cannot be serialized"* away. The conversation is not
    in here for the same reason: it travels as a node, and this is its id.
    """
    return {
        "asr": asr_options.model_dump(),
        "device": capture_options.model_dump(),
        "history": history_node,
    }


async def run_one_turn(
    session: a11.Session,
    stream: Any,
    headers: dict[str, str],
    inputs_value: dict[str, Any],
    described: dict[str, Any],
    console: Console,
) -> list[Any]:
    """Run the flow once, print what it produces, and return what to remember.

    Which ports to read comes from ``described`` -- the plan the gateway just
    compiled -- rather than from a list of names kept here: a reader subscribed
    to a port the flow does not declare waits on a node nobody writes, and it
    waits in silence.

    Returns the interactions the flow put on ``turn``: the question it heard,
    and what the model made of it. Empty when nothing was heard, which is how
    the caller knows the conversation is over.
    """
    called = remote(session, stream, flow_tools.FLOW_RUN_SCHEMA)
    for name, value in headers.items():
        called.set_header(name, value.encode())
    await called.call()

    # Subscribed before the source is sent, so nothing produced early is
    # missed: a node hands a late reader the stream from its beginning anyway,
    # but there is no reason to rely on that here.
    def published(port: str):
        return session.node_map.get(
            flow_tools.flow_output_node_id(called.get_id(), port)
        )

    outputs = {port: published(port) for port in described["outputs"]}

    async with (
        called["source"] as source,
        called["inputs"] as inputs,
        called["flow"] as which,
    ):
        await source.put_final(FLOW_SOURCE)
        await inputs.put_final(inputs_value)
        await which.put_null_final()

    remembered: list[Any] = []

    async def show(port: str) -> None:
        """Print one output port as it fills, the way that port reads best."""
        async for value in outputs[port]:
            if port == "turn":
                # Not for reading: this is the conversation, coming back.
                remembered.append(value)
                continue
            text = str(value)
            if port == "sentence":
                console.print(f"\n[bold]you said:[/] {text.strip()}")
                console.print("[bold]answer:[/] ", end="")
            elif port == "reply":
                # A model's answer, token by token: no newline, no markup.
                print(text, end="", flush=True)
            else:
                console.print(f"[dim]{port}:[/] {text.strip()}")

    console.print(
        "[bold]listening[/] — say something that ends in a full stop "
        f"(up to {FLAGS.listen_seconds}s)"
    )
    readers = [asyncio.ensure_future(show(port)) for port in outputs]
    try:
        # The flow finishing is what ends this, and a flow that fails says so
        # here rather than after a silent wait for a sentence that was never
        # going to arrive.
        await asyncio.wait_for(called.wait(), timeout=FLAGS.listen_seconds)
        await asyncio.wait_for(asyncio.gather(*readers), timeout=10)
    except asyncio.TimeoutError:
        console.print("\n[red]nothing that ended a sentence was heard[/]")
        called.cancel()
        remembered.clear()
    finally:
        for reader in readers:
            reader.cancel()
    print()
    return remembered


async def chat(
    session: a11.Session,
    stream: Any,
    headers: dict[str, str],
    asr_options: Any,
    capture_options: Any,
    described: dict[str, Any],
    console: Console,
) -> None:
    """One turn after another, each one carrying the ones before it.

    The history is this script's, not the gateway's: a turn hands back the
    interactions to keep, and the next turn sends them in as `history` for the
    flow to put in front of the new question. Nothing is stored anywhere, and
    the composition is stateless -- which is what lets the same flow serve a
    first turn and a fiftieth.
    """
    history: list[Any] = []
    turn = 0
    failures = 0
    while FLAGS.turns <= 0 or turn < FLAGS.turns:
        turn += 1
        console.print(
            f"[bold cyan]turn {turn}[/]"
            + (
                f" [dim]— remembering {len(history)} interaction(s)[/]"
                if history
                else " [dim]— a new conversation[/]"
            )
        )
        try:
            where = await write_history(session, stream, turn, history)
            remembered = await run_one_turn(
                session,
                stream,
                headers,
                flow_inputs(asr_options, capture_options, where),
                described,
                console,
            )
        except StatusException as error:
            # One turn failing is not the conversation failing: a provider
            # hiccup should cost the turn and nothing else, and the history is
            # this script's, so it survives. Twice in a row is a configuration
            # problem rather than a blip, and no amount of talking will fix it.
            failures += 1
            console.print(f"\n[red]that turn failed:[/] {error}")
            if failures > 1:
                console.print("[dim]twice in a row; giving up[/]")
                return
            continue
        failures = 0
        if not remembered:
            console.print("[dim]nothing said; goodbye[/]")
            return
        history.extend(remembered)
    console.print(
        f"[dim]{turn} turn(s), {len(history)} interaction(s) remembered[/]"
    )


async def run(console: Console) -> int:
    session, stream = await connect()
    console.print(f"connected to [bold]{FLAGS.gateway}[/]\n")

    # Both of these are the gateway's flow tools answering, and neither one
    # needs a microphone or a model -- `--check_only` stops here.
    await show_what_is_composable(session, stream, console)
    described = await check_the_flow(session, stream, console)
    if FLAGS.check_only:
        return 0

    provider = FLAGS.provider
    model = FLAGS.model or DEFAULT_MODEL.get(provider, "")
    api_key = next(
        (
            os.environ[name]
            for name in API_KEY_ENV.get(provider, ())
            if os.environ.get(name)
        ),
        "",
    )
    if provider in API_KEY_ENV and not api_key:
        expected = " or ".join(API_KEY_ENV[provider])
        console.print(f"[red]set {expected} to answer with {provider}[/]")
        return 1

    # Resolve (downloading if this is the first run) the same models `a11 chat`
    # uses for voice input.
    from a11.cli.voice import (
        DEFAULT_VOICE_MODEL,
        ensure_vad_model,
        ensure_voice_model,
    )
    from a11.sdk.audio import SpeechRecognizerOptions

    name = FLAGS.voice_model or DEFAULT_VOICE_MODEL
    voice_model = await ensure_voice_model(name, console)
    vad_model = await ensure_vad_model(console)
    asr_options = SpeechRecognizerOptions(
        model=str(voice_model),
        vad_model=str(vad_model),
        language="en" if name.endswith(".en") else "auto",
    )

    from a11.sdk.audio import AudioInputOptions

    await chat(
        session,
        stream,
        {
            LlmHeaders.PROVIDER.value: provider,
            LlmHeaders.MODEL.value: model,
            LlmHeaders.API_KEY.value: api_key,
            LlmHeaders.BASE_URL.value: DEFAULT_BASE_URL.get(provider, ""),
        },
        asr_options,
        AudioInputOptions(),
        described,
        console,
    )
    return 0


def main(_: Sequence[str]) -> int:
    console = Console()
    try:
        return asyncio.run(run(console))
    except KeyboardInterrupt:
        console.print("\n[dim]goodbye[/]")
        return 0
    except StatusException as error:
        logging.error("%s", error, exc_info=True)
        console.print(f"[red]{error}[/]")
        return 1


if __name__ == "__main__":
    app.run(main)
