"""Run the flows in this directory.

Three things are worth watching for as it runs:

* The flows are *text*. They are compiled here from files, but the same call
  compiles a string that arrived over a wire -- which is the point: a gateway
  can be given a composition of actions it has never seen and run it, with no
  repository change and no redeploy.
* A flow is an action. `headline` calls `research`, and neither one knows the
  other is a composition; the registry treats both like anything else.
* What the flows trim, nothing pays for. The summarizer reports how much text it
  was handed, and the flow hands it a fraction of what was fetched.

    python examples/003-flow-dsl/main.py

`research.flow`, `triage.flow` and `recover.flow` run against the toy actions in
`actions.py`, so that needs nothing at all. The other three compose actions a
real `a11 gateway run` serves -- its shell, its microphone, and the same
`interact_with_llm` `a11 chat` uses -- and run when you point at one:

    a11 gateway run                                  # in another terminal
    export ANTHROPIC_API_KEY=...                     # for the model ones
    python examples/003-flow-dsl/main.py --gateway ws://127.0.0.1:8011/a11
    python examples/003-flow-dsl/main.py --gateway ... --listen   # microphone

`ask-the-pages` is the one to read twice: its retrieval actions run *here*, its
model runs on the gateway, and the flow does not distinguish between them. An
action with a schema and no handler in the local registry is one the flow
dispatches over the session -- that is the whole configuration.
"""

from __future__ import annotations

import asyncio
import os
import pathlib
from typing import Any, Sequence

from absl import app as absl_app
from absl import flags

import a11
from a11 import flow
from a11.flow.runtime import make_handler
from a11.gateway.config import DEFAULT_GATEWAY_URL
from a11.sdk import bash
from a11.sdk.audio import actions as audio_actions
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.sdk.llm import LlmHeaders
from a11.status import StatusException

from actions import make_registry, raw_corpus_size

# The model flows name `a11.sdk.Interaction`, and a tag resolves only to a type
# this process has been told about; importing the module is what tells it.
import a11.sdk.llm  # noqa: E402,F401  isort:skip

HERE = pathlib.Path(__file__).parent

FLAGS = flags.FLAGS

flags.DEFINE_string(
    "gateway",
    "",
    f"Gateway to compose against, e.g. {DEFAULT_GATEWAY_URL}. Empty runs only"
    " the examples that need nothing.",
)
flags.DEFINE_bool(
    "listen", False, "Also run the microphone example (needs a gateway)."
)
flags.DEFINE_integer("listen_seconds", 15, "How long to dictate for.")
flags.DEFINE_string("provider", "claude", "LLM provider for the model flows.")
flags.DEFINE_string("model", "", "Model to answer with; provider default.")

#: What each provider calls its usual model, and where its key comes from.
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

#: The gateway actions these flows call. Registered here as schemas with no
#: handler: a flow reads the ports off the schema, and dispatches the call over
#: the session precisely because there is nothing here to run it with.
REMOTE_ACTIONS = (
    INTERACT_WITH_LLM_SCHEMA,
    bash.SHELL_EXECUTE_SCHEMA,
    audio_actions.CAPTURE_AUDIO_SCHEMA,
    audio_actions.TRANSCRIBE_AUDIO_SCHEMA,
)


async def run_research() -> None:
    registry = make_registry()
    program = flow.load(HERE / "research.flow")
    program.register_all(registry)

    print("--- research ---------------------------------------------------")
    answer = await program["research"].invoke(
        question="how do nodes and actions stream",
        registry=registry,
        headers={"x-a11-deadline": "60000"},
    )
    print("answer :", answer["answer"])
    print("sources:", answer["sources"])
    print(
        f"the model read {answer['read']} characters, out of "
        f"{raw_corpus_size()} in the corpus -- the rest was cut by "
        "`truncate` before it ever left the fetches"
    )

    print()
    print("--- headline (a flow calling a flow) ---------------------------")
    line = await program["headline"].invoke(
        question="how do nodes and actions stream", registry=registry
    )
    print("line   :", line["line"])


async def run_triage() -> None:
    registry = make_registry()
    program = flow.load(HERE / "triage.flow")
    program.register_all(registry)

    print()
    print("--- triage (a loop with carried state) -------------------------")
    result = await program["triage"].invoke(
        report="the deploy went sideways at 04:00",
        registry=registry,
    )
    print("verdict:", result["verdict"])
    print("notes  :", result["notes"])
    for one in result["passes"]:
        print(f"  pass {one['round']}: confidence {one['confidence']}")

    print()
    print("--- triage-strictly (a flow that refuses) ----------------------")
    try:
        await program["triage-strictly"].invoke(
            report="short", registry=registry
        )
    except StatusException as error:
        print(f"refused: {error.status.code.name} -- {error.status.message}")


async def run_recovery() -> None:
    registry = make_registry()
    program = flow.load(HERE / "recover.flow")
    program.register_all(registry)

    print()
    print("--- resilient-read (a failure the flow expects) ----------------")
    result = await program["resilient-read"].invoke(
        urls=[
            "https://example.test/missing",
            "https://example.test/nodes",
            "https://example.test/actions",
        ],
        registry=registry,
    )
    for attempt in result["tried"]:
        outcome = "ok" if attempt["ok"] else f"failed: {attempt['why']}"
        print(f"  {attempt['url']}: {outcome}")
    print("text   :", result["text"])

    print()
    print("--- annotated-read (a node the flow lends out) -----------------")
    read = await program["annotated-read"].invoke(
        url="https://example.test/fibers", registry=registry
    )
    print("summary :", read["summary"])
    print("progress:", read["progress"])

    print()
    print("--- strict-read (a failure translated) -------------------------")
    try:
        await program["strict-read"].invoke(
            url="https://example.test/nowhere", registry=registry
        )
    except StatusException as error:
        print(f"refused: {error.status.code.name} -- {error.status.message}")


async def show_a_flow_as_data() -> None:
    """A compiled flow is inspectable, which is what makes one reviewable."""
    program = flow.load(HERE / "research.flow")
    described = program["research"].describe()
    print()
    print("--- research, as data -----------------------------------------")
    print("inputs  :", ", ".join(described["inputs"]))
    print("outputs :", ", ".join(described["outputs"]))
    print("nodemaps:", ", ".join(described["node_maps"]))
    for step in described["steps"]:
        detail = step.get("action") or step.get("from") or step.get("of") or ""
        print(f"  {step['step']:<5} {step['label']:<28} {detail}")


async def a_flow_that_arrived_as_a_string() -> None:
    """The same compiler, on source that never touched the filesystem."""
    registry = make_registry()
    source = """
    flow one-line-answer {
      in  question: string required
      out answer:   string

      search = run web-search(query: question, limit: 1)
      for hit in search.hits {
        page = run web-fetch(url: hit.url)
        page.text | truncate 80 -> answer
        skip page.bytes
      }
      skip search.debug
    }
    """
    program = flow.register(source, registry, "from-the-wire.flow")
    print()
    print("--- a flow received at runtime ---------------------------------")
    print("registered:", registry.list_registered_actions())
    result = await program["one-line-answer"].invoke(
        question="what are fibers", registry=registry
    )
    print("answer :", result["answer"])


# --- Against a real gateway ---------------------------------------------------


async def connect(url: str) -> tuple[a11.Session, Any, a11.ActionRegistry]:
    """A session on the gateway, and the registry these flows run against.

    The registry is the interesting part: the toy actions from `actions.py`
    with their handlers, and the gateway's actions with only their schemas. A
    flow calls both the same way and the difference decides where each one
    runs.
    """
    options = a11.WebSocketClientOptions()
    # The gateway speaks HTTP/1.1 for WebSocket; a client that insists on h2
    # never finishes the handshake.
    options.http2_options.client_preference = a11.HttpProtocolPreference.HTTP11
    stream = a11.WebSocketWireStream.connect(url, websocket_options=options)

    registry = make_registry()
    for schema in REMOTE_ACTIONS:
        registry.register(schema.name, schema)

    session = a11.Session(action_registry=registry)
    await session.add_stream(stream, mode="start")
    return session, stream, registry


def llm_headers() -> dict[str, str] | None:
    """The headers the model flows forward, or None if no key is set."""
    provider = FLAGS.provider
    key = next(
        (
            os.environ[name]
            for name in API_KEY_ENV.get(provider, ())
            if os.environ.get(name)
        ),
        "",
    )
    if provider in API_KEY_ENV and not key:
        return None
    return {
        LlmHeaders.PROVIDER.value: provider,
        LlmHeaders.MODEL.value: FLAGS.model or DEFAULT_MODEL.get(provider, ""),
        LlmHeaders.API_KEY.value: key,
        LlmHeaders.BASE_URL.value: DEFAULT_BASE_URL.get(provider, ""),
    }


async def run_on_the_gateway(url: str) -> None:
    session, stream, registry = await connect(url)
    where = {
        "registry": registry,
        "session": session,
        "node_map": session.node_map,
        "dispatch_stream": stream,
    }
    headers = llm_headers()

    print()
    print("--- count-changes (the gateway's shell, composed) --------------")
    program = flow.load(HERE / "ops.flow")
    program.register_all(registry)
    counted = await program["count-changes"].invoke({}, **where)
    print(f"changed: {counted['changed']} file(s)")
    for line in counted["files"][:5]:
        print(f"  {line}")

    if headers is None:
        expected = " or ".join(API_KEY_ENV[FLAGS.provider])
        print()
        print(f"skipping the model flows: set {expected}")
        return

    print()
    print("--- ask-the-pages (local retrieval, remote model) --------------")
    assistant = flow.load(HERE / "assistant.flow")
    assistant.register_all(registry)
    try:
        answered = await assistant["ask-the-pages"].invoke(
            {"question": "how do nodes and actions stream"},
            headers=headers,
            **where,
        )
    except StatusException as error:
        # Whatever the provider said, said here: the flow reports the peer's
        # own status rather than "something went wrong over there".
        print(f"the model refused: {first_line(error)}")
        print("(the retrieval half of this flow needs no model:")
        quoted = await assistant["quote-the-pages"].invoke(
            {"question": "how do nodes and actions stream"}, **where
        )
        print(f" {quoted['answer'][:70]}...)")
        return

    print("answer:", answered["answer"])
    print("cited :", ", ".join(answered["cited"]))
    print(f"prompt: {len(answered['prompt'])} characters, built by the flow")

    print()
    print("--- explain-a-command (shell and model, one step) --------------")
    explained = await program["explain-a-command"].invoke(
        {"command": "git status --porcelain | head -5"},
        headers=headers,
        **where,
    )
    print("verdict:", explained["verdict"])

    print()
    print("--- chat-turn (a conversation, two turns) ----------------------")
    # The only state between turns is what the flow handed back. `then` is
    # what keeps it in order: the turns so far, and then the new question.
    history: list = []
    conversation = (
        "what is a fiber?",
        "and how is that different to a thread?",
    )
    for question in conversation:
        turn = await assistant["chat-turn"].invoke(
            {"history": history, "question": question},
            headers=headers,
            **where,
        )
        print(f"  you  : {question}")
        print(f"  model: {turn['reply'][:100]}")
        history = history + turn["turn"]
    print(f"  ({len(history)} interactions carried, by the caller, in order)")


def first_line(error: StatusException) -> str:
    """A provider's failure is often a whole traceback; one line will do."""
    message = error.status.message.strip().splitlines()
    return f"{error.status.code.name}: {message[-1] if message else ''}"[:120]


async def run_dictation(url: str) -> None:
    """The microphone one, which needs a person to say something."""
    from a11.cli.voice import ensure_vad_model, ensure_voice_model
    from a11.sdk.audio import SpeechRecognizerOptions
    from rich.console import Console

    headers = llm_headers()
    if headers is None:
        print("skipping dictation: no API key for the model")
        return

    console = Console()
    voice_model = await ensure_voice_model("base.en", console)
    vad_model = await ensure_vad_model(console)
    session, stream, registry = await connect(url)

    program = flow.load(HERE / "dictate.flow")
    program.register_all(registry)
    plan = program["dictate-a-note"]

    # The stream is for the flow's calls, not for the flow: see `invoke`, which
    # takes the same two apart as `stream` and `dispatch_stream`.
    action = a11.Action(
        plan.schema,
        handler=make_handler(plan, dispatch_stream=stream),
        registry=registry,
        session=session,
        node_map=session.node_map,
    )
    for name, value in headers.items():
        action.set_header(name, value.encode())

    said = action.get_output("said", bind_stream=False)
    note = action.get_output("note", bind_stream=False)
    action.run()

    for name, value in (
        (
            "asr",
            SpeechRecognizerOptions(
                model=str(voice_model),
                vad_model=str(vad_model),
                language="en",
            ),
        ),
        ("capture", {}),
    ):
        node = action.get_input(name, bind_stream=False)
        await (await node.put(value))
        await (await node.put_null_final())
        await node.drain_and_close()

    print()
    print(
        f"--- dictate-a-note (say something for {FLAGS.listen_seconds}s) ---"
    )

    async def show() -> None:
        async for sentence in said:
            print("  heard:", str(sentence))

    watching = asyncio.ensure_future(show())
    await asyncio.sleep(FLAGS.listen_seconds)

    # The caller holds the off switch, which is why the flow takes one.
    control = action.get_input("control", bind_stream=False)
    await (await control.put({"command": "stop"}))
    await (await control.put_null_final())
    await control.drain_and_close()

    tidied = [str(value) async for value in note]
    await watching
    await action.wait()
    print("note :", "".join(tidied))


async def main(_argv: Sequence[str]) -> None:
    await run_research()
    await run_triage()
    await run_recovery()
    await show_a_flow_as_data()
    await a_flow_that_arrived_as_a_string()

    if not FLAGS.gateway:
        print()
        print(
            "--- and three more ---------------------------------------------"
        )
        print(
            "assistant.flow, ops.flow and dictate.flow compose a gateway's own"
            "\nactions. Start one and pass --gateway to run them:"
        )
        print("    a11 gateway run")
        print(
            "    python examples/003-flow-dsl/main.py --gateway"
            f" {DEFAULT_GATEWAY_URL}"
        )
        return

    await run_on_the_gateway(FLAGS.gateway)
    if FLAGS.listen:
        await run_dictation(FLAGS.gateway)


def sync_main(argv: Sequence[str]) -> None:
    a11.enable_logging("warning")
    asyncio.run(main(argv))


if __name__ == "__main__":
    absl_app.run(sync_main)
