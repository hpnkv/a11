"""The backend the browser guides talk to, and the demo agent on the exchange.

Run with ``python -m a11.demos.web_demos_server``. It serves, on
``ws://127.0.0.1:9010/a11-demos``:

* ``interact_with_llm`` — one action for every provider, routed by the
  ``x-a11-llm-*`` headers the browser sets, with each turn recorded in a SQLite
  chunk store (see [a11.gateway.conversations][a11.gateway.conversations]).
* ``get_conversation`` / ``get_conversations`` — how a reloaded page picks a
  conversation up where it left off.
* ``deep-research`` and the flows it is made of, compiled from
  ``deep_research.flow`` beside this file. Registered with `RESEARCH_HEADERS`,
  which is how they come with this server's own ollama already filled in: a
  composition declares no headers of its own, so without that a console has no
  field to offer and a call that names no provider cannot be answered.
* ``text_to_image``, when [a11.demos.text_to_image][a11.demos.text_to_image] can
  reach a Stable Diffusion checkpoint.
* `__list_actions__` — which every A11 peer answers, and which is how this
  server learns what a page serves without the page announcing anything.
  [a11.gateway.tool_bridge][a11.gateway.tool_bridge]).

This is a trimmed `a11 gateway`: the same conversation store, the same tool
bridge, without the shell, audio and flow-authoring actions a gateway also
serves. Anyone already running ``a11 gateway run`` can point the demos at it
instead — the actions the guides call are the ones it has — and everything but
``deep-research`` and ``text_to_image`` will answer. Guides:
``doc/docs/guides/chat-sessions.md``, ``deep-research.md``, ``browser-tools.md``
and ``generative-media.md``.

## It is served by ``a11 serve``

Every transport, TLS and hosting flag here is
[`a11 serve`][a11.cli.commands.serve]'s, declared once there and reused: this
module is a `SERVICE` symbol that command can serve, and ``python -m
a11.demos.web_demos_server`` is that command with the target filled in and the
WebSocket defaults the guides quote. The equivalent long way round is

```sh
a11 serve a11.demos.web_demos_server:SERVICE --ws --ws-port 9010 --ws-path /a11-demos
```

which is worth knowing because everything ``a11 serve`` grew is therefore
available here. The one that matters is ``--hosted``:

```sh
python -m a11.demos.web_demos_server --ws --hosted demoserver
```

takes a claim on the exchange, registers with signalling, and keeps both alive
--- so the same actions a guide's page reaches over its own WebSocket are
reachable at ``https://a11.to/ui/peer/demoserver`` by anyone logged in, with no
inbound port involved. A chat turn from that console is a call on
``interact_with_llm``, so the provider and model arrive the way they always do:
as ``x-a11-llm-*`` headers on the call.

Two things are configured by environment rather than by flag, because the
service is built when the symbol is first read --- before, in the ``a11 serve``
case, any of this module's own flags exist: ``A11_DEMOS_CONVERSATION_ROOT`` (the
SQLite conversation store, default `conversations.default_root`) and
``A11_DEMOS_TEXT_TO_IMAGE=0`` (do not serve ``text_to_image``, which needs a
diffusion checkpoint). ``main`` sets both from its flags.
"""

from __future__ import annotations

import argparse
import asyncio
import os
import pathlib
from typing import Any

from absl import logging

import a11
from a11 import flow, net
from a11.cli import backends
from a11.cli.commands import serve as serve_command
from a11.demos import split_lines
from a11.gateway import conversation_actions, conversations
from a11.gateway.tool_bridge import RemoteToolBridge
from a11.sdk.interact_with_llm import interact_with_llm
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.sdk.llm import LlmHeaders
from a11.service.service import Service, ServiceOptions
from a11.service.session import Session

# The flows name `a11.sdk.Interaction`, and a tag resolves only to a type this
# process has been told about; importing the module is what tells it.
import a11.sdk.llm  # noqa: F401  isort:skip

HERE = pathlib.Path(__file__).parent

#: Where the demos look for this server unless told otherwise.
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 9010
DEFAULT_PATH = "/a11-demos"

#: Where to keep the conversation store, read when the service is built.
CONVERSATION_ROOT_ENV = "A11_DEMOS_CONVERSATION_ROOT"

#: Set to ``0`` to leave ``text_to_image`` unregistered.
TEXT_TO_IMAGE_ENV = "A11_DEMOS_TEXT_TO_IMAGE"

#: The deep-research composition, beside this file.
DEEP_RESEARCH_FLOW = HERE / "deep_research.flow"

#: What the deep-research flows route to when the caller says nothing.
#:
#: A header schema's ``default`` is *applied*, not merely advertised: an
#: `Action` seeds its header map from its schema, so a call that names no
#: provider really does go to the local ollama -- and the value is forwarded to
#: every nested model call, because that is what A11 does with an ``x-a11-``
#: header. Which is the point: `deep-research` takes a topic and nothing else,
#: and a console that has to be told three header names before the interesting
#: action will answer is a console nobody tries it from.
#:
#: The values are the ones the guide and the console's own flow template already
#: quote, and the model comes from `a11.cli.backends`, which is where this
#: project decides what "ollama" means. ``127.0.0.1`` is right because it is
#: read *on the server*: this demo server runs on the box that has ollama.
OLLAMA_DEFAULTS: dict[str, str] = {
    LlmHeaders.PROVIDER: "ollama",
    LlmHeaders.MODEL: backends.PROVIDERS["ollama"].default_model,
    LlmHeaders.BASE_URL: "http://127.0.0.1:11434",
}

#: The headers the deep-research flows advertise, with those defaults on them.
#:
#: The flows themselves declare none -- a composition is configured by the
#: headers of the call that started it, so there is nothing for the *language*
#: to say. Declaring them here is what puts a labelled, pre-filled field in
#: front of somebody in the console, and `x-a11-llm-base-url` is not in
#: `a11.DEFAULT_ACTION_HEADERS` or in `interact_with_llm`'s own headers, so this
#: is the only way it appears at all.
RESEARCH_HEADERS = a11.DEFAULT_ACTION_HEADERS | {
    LlmHeaders.PROVIDER: a11.ActionHeaderSchema(
        LlmHeaders.PROVIDER,
        "Which provider answers every model call in this composition."
        f" Defaults to {OLLAMA_DEFAULTS[LlmHeaders.PROVIDER]}.",
        default=OLLAMA_DEFAULTS[LlmHeaders.PROVIDER].encode(),
    ),
    LlmHeaders.MODEL: a11.ActionHeaderSchema(
        LlmHeaders.MODEL,
        "The model to ask. Defaults to"
        f" {OLLAMA_DEFAULTS[LlmHeaders.MODEL]}, which is what this server's"
        " own ollama serves.",
        default=OLLAMA_DEFAULTS[LlmHeaders.MODEL].encode(),
    ),
    LlmHeaders.BASE_URL: a11.ActionHeaderSchema(
        LlmHeaders.BASE_URL,
        "Where that provider lives. Defaults to the ollama on this server;"
        " a hosted provider needs no base URL and an API key instead.",
        default=OLLAMA_DEFAULTS[LlmHeaders.BASE_URL].encode(),
    ),
    LlmHeaders.API_KEY: a11.ActionHeaderSchema(
        LlmHeaders.API_KEY,
        "API key for the provider, where it needs one. Left empty for the"
        " local default.",
    ),
}

#: `interact_with_llm` under a second name: the same ports, the same headers,
#: and the plain handler rather than the one that records a conversation. It is
#: what a *composition's* model calls use -- see `make_registry`.
ASK_MODEL_SCHEMA = a11.ActionSchema(
    name="ask_model",
    description=(
        "Ask the configured model, without recording the exchange as a"
        " conversation. Same ports as interact_with_llm."
    ),
    inputs=INTERACT_WITH_LLM_SCHEMA.inputs,
    outputs=INTERACT_WITH_LLM_SCHEMA.outputs,
    headers=INTERACT_WITH_LLM_SCHEMA.headers,
)

_PROGRAM: flow.Program | None = None


def deep_research_program() -> flow.Program:
    """The compiled `deep_research.flow`, read once.

    Public because a caller wants the *schemas* as much as the handlers: the
    browser declares `deep-research`'s ports by hand, and this is what they are
    checked against.
    """
    global _PROGRAM
    if _PROGRAM is None:
        _PROGRAM = flow.load(DEEP_RESEARCH_FLOW)
    return _PROGRAM


def research_schema(schema: a11.ActionSchema) -> a11.ActionSchema:
    """`schema` with `RESEARCH_HEADERS` on it, ports and prose untouched.

    Rebuilt rather than `model_copy(update=...)`-ed: that round-trips through
    validation, which wants headers as plain mappings and refuses the schema
    objects this already holds. The same five fields `ASK_MODEL_SCHEMA` names,
    for the same reason -- a schema is small enough to say outright.
    """
    return a11.ActionSchema(
        name=schema.name,
        description=schema.description,
        inputs=schema.inputs,
        outputs=schema.outputs,
        headers=RESEARCH_HEADERS,
    )


def make_registry(
    store: conversations.ConversationStore,
    *,
    text_to_image: bool = True,
) -> a11.ActionRegistry:
    """Everything the four guides call, in one registry."""

    registry = a11.ActionRegistry()

    # `interact_with_llm` wrapped in the handler that records the turn, plus the
    # two actions that read the recording back. The provider is not chosen here:
    # it arrives per call as a header, which is what lets one registration serve
    # Gemini and Ollama alike.
    conversation_actions.install(registry, store)

    # The same action without the recording, under its own name. A composition's
    # model calls are steps, not chat turns: recorded, each one would arrive in
    # the conversation list as a conversation of its own. So the deep-research
    # flow asks for `ask_model` and the chat guide's page asks for
    # `interact_with_llm`, and only one of the two is history.
    registry.register(
        ASK_MODEL_SCHEMA.name, ASK_MODEL_SCHEMA, interact_with_llm
    )

    # The one primitive the Flow language does not have (see the module).
    registry.register(
        split_lines.SPLIT_LINES_SCHEMA.name,
        split_lines.SPLIT_LINES_SCHEMA,
        split_lines.split_lines,
    )

    # A composition, as text. The deep-research guide's whole backend is the
    # file beside this one -- the actions it names are the ones above.
    #
    # Registered one at a time rather than with `register_all`, because each
    # goes in under a schema with `RESEARCH_HEADERS` on it. The amendment is
    # *this server's* and not the language's: a flow declares no headers because
    # a composition is configured by the call that started it, and which
    # provider this particular deployment should fall back to is a hosting
    # decision. So `deep_research_program()` stays exactly the compiled file,
    # and the browser can keep checking its ports against it.
    program = deep_research_program()
    for flow_name in program.names:
        entry = program[flow_name]
        registry.register(
            flow_name, research_schema(entry.schema), entry.handler
        )

    if text_to_image:
        from a11.demos import text_to_image as t2i

        registry.register(
            t2i.TEXT_TO_IMAGE_SCHEMA.name,
            t2i.TEXT_TO_IMAGE_SCHEMA,
            t2i.text_to_image,
        )

    return registry


def make_service(registry: a11.ActionRegistry) -> Service:
    """A service whose every connection can serve the page's own actions back.

    Each stream gets a copy of the registry, because the tool bridge registers
    the *caller's* tools on it: those actions belong to one page and must not be
    visible on anyone else's session. Same reasoning as
    [a11.gateway.app.A11Gateway][a11.gateway.app.A11Gateway], which is where
    this shape comes from.
    """

    async def on_connection(session: Session, stream: net.WireStream) -> None:
        logging.info(
            "connection accepted: stream %s, session %s",
            stream.get_id(),
            session.get_id(),
        )
        session.add_done_callback(
            lambda finished: logging.info(
                "connection closed: session %s: %s",
                finished.get_id(),
                finished.get_status(),
            )
        )

        per_connection = registry.copy()
        bridge = RemoteToolBridge()
        bridge.install(per_connection)
        session.set_action_registry(per_connection)
        bridge.bind_session(session, stream)

    return Service(
        action_registry=registry,
        on_connection=on_connection,
        # False: the hook makes the copy itself, because the bridge must own the
        # very copy it registers the page's tools on.
        options=ServiceOptions(copy_registry_per_connection=False),
    )


#: The service, built once, on demand. Not at import: the store it records
#: conversations in is a directory this module creates, and importing a module
#: should not create anything -- and `main` has environment to set before the
#: first read (see `__getattr__`).
_SERVICE: Service | None = None


def service() -> Service:
    """The one service this module serves, built on first use.

    Configured from the environment rather than from arguments, because the
    reader may be `a11 serve` resolving the `SERVICE` symbol, which has no idea
    this module has options. See the module docstring for the two variables.

    Call it with an event loop running. A native `Service` binds to the loop it
    was built on, so one built without one either raises or -- worse -- serves a
    first request that never completes; `a11 serve` reads the symbol from inside
    `asyncio.run`, which is why the lazy build is safe there.
    """
    global _SERVICE
    if _SERVICE is None:
        root = os.environ.get(CONVERSATION_ROOT_ENV, "")
        store = conversations.ConversationStore(
            pathlib.Path(root).expanduser() if root else None
        )
        wanted = os.environ.get(TEXT_TO_IMAGE_ENV, "1").strip()
        registry = make_registry(
            store, text_to_image=wanted not in ("0", "false", "no")
        )
        logging.info(
            "demo actions: %s (conversations in %s)",
            ", ".join(sorted(registry.list_registered_actions())),
            store.root,
        )
        _SERVICE = make_service(registry)
    return _SERVICE


def __getattr__(name: str) -> Any:
    """``SERVICE`` and ``REGISTRY``, built when something asks for them.

    PEP 562, so that ``a11 serve a11.demos.web_demos_server:SERVICE`` -- whose
    whole interface to this module is one `getattr` -- gets a service without
    this module building one for every importer. ``python -m`` imports it too,
    under ``__main__``, and never reads either symbol.
    """
    if name == "SERVICE":
        return service()
    if name == "REGISTRY":
        return service().action_registry
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


#: The target `main` serves. Spelled out rather than built from ``__name__``,
#: which is ``__main__`` under ``python -m`` -- resolvable, since that module is
#: this one, but reported as ``__main__`` and true only by luck.
TARGET = "a11.demos.web_demos_server:SERVICE"


def _configure(parser: argparse.ArgumentParser) -> None:
    """This module's own two flags, on top of every ``a11 serve`` flag.

    The transport, TLS and hosting flags are `a11 serve`'s own declaration, so
    there is one vocabulary for them however this is started. Only the WebSocket
    defaults differ: the guides quote 9010 and ``/a11-demos``, and a page that
    has to be told a different URL is a page whose instructions are wrong.
    """
    serve_command.configure(parser, with_target=False)
    parser.set_defaults(
        target=TARGET,
        ws_host=DEFAULT_HOST,
        ws_port=DEFAULT_PORT,
        ws_path=DEFAULT_PATH,
    )
    parser.add_argument(
        "--conversation-root",
        default=None,
        type=pathlib.Path,
        metavar="DIR",
        help=(
            "Where to keep the SQLite conversation store. Also"
            f" {CONVERSATION_ROOT_ENV}."
        ),
    )
    parser.add_argument(
        "--no-text-to-image",
        action="store_true",
        help="Do not serve text_to_image, which needs a diffusion model.",
    )


def main(argv: list[str] | None = None) -> int:
    """Parse this module's flags and hand them to ``a11 serve``."""
    parser = argparse.ArgumentParser(
        prog="python -m a11.demos.web_demos_server",
        description=(
            "Serve the browser guides' backend. Takes every a11 serve flag,"
            " including --hosted, with the WebSocket defaults the guides use."
        ),
    )
    _configure(parser)
    args = parser.parse_args(argv)

    # Before `serve` reads the symbol, which is what builds the service.
    if args.conversation_root is not None:
        os.environ[CONVERSATION_ROOT_ENV] = str(args.conversation_root)
    if args.no_text_to_image:
        os.environ[TEXT_TO_IMAGE_ENV] = "0"

    try:
        return asyncio.run(serve_command.run(args))
    except KeyboardInterrupt:
        logging.info("demo server stopped")
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
