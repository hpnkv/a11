"""The backend the browser guides talk to, and the demo agent on the exchange.

Run with ``python -m a11.demos.web_demos_server``. It serves, on
``ws://127.0.0.1:9010/a11-demos``:

* ``interact_with_llm`` — one action for every provider, routed by the
  ``x-a11-llm-*`` headers the browser sets, with each turn recorded in a SQLite
  chunk store (see [a11.gateway.conversations][a11.gateway.conversations]).
* ``get_conversation`` — how a reloaded page picks a conversation up where it
  left off, given its id.
* ``deep-research`` and the flows it is made of, compiled from
  ``deep_research.flow`` beside this file. Registered with `RESEARCH_HEADERS`,
  which is how they come with this server's own ollama already filled in: a
  composition declares no headers of its own, so without that a console has no
  field to offer and a call that names no provider cannot be answered.
* ``text_to_image``, when [a11.demos.text_to_image][a11.demos.text_to_image] can
  reach a Stable Diffusion checkpoint.
* ``echo``, for the browser-clients guide: the smallest action there is, so a
  page can watch a transport work without a model in the way.
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
a11 serve a11.demos.web_demos_server:SERVICE --ws --ws-port 9010 \
  --ws-path /a11-demos
```

All ``a11 serve`` options are therefore available here, including ``--hosted``:

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
import hashlib
import os
import pathlib
from typing import Any

from absl import logging

import a11
from a11 import flow, net
from a11.cli import backends
from a11.cli.commands import serve as serve_command
from a11.demos import echo_server, split_lines
from a11.demos.rate_limit import RateLimiter
from a11.gateway import conversation_actions, conversations
from a11.gateway.tool_bridge import RemoteToolBridge
from a11.sdk.interact_with_llm import interact_with_llm
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.sdk.llm import LlmHeaders
from a11.service.service import Service, ServiceOptions
from a11.service.session import Session
from a11.status import Status, StatusCode

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

#: The environment variable holding the real API key for the demo backend.
DEMO_API_KEY_ENV = "A11_DEMO_API_KEY"

#: The placeholder key browsers send; the server swaps it for the real one.
DEMO_API_KEY_PLACEHOLDER = "use-a11-demo-resources"

#: The ``fp`` query parameter on the WebSocket URL carries the browser's
#: device fingerprint for rate-limit bucketing.

#: The deep-research composition, beside this file.
DEEP_RESEARCH_FLOW = HERE / "deep_research.flow"

#: Default backend configuration for the deep-research flows.
#:
#: A header schema's ``default`` is *applied*, not merely advertised: an
#: `Action` seeds its header map from its schema, so a call that names no
#: provider uses local Ollama. A11 forwards ``x-a11-`` headers to nested model
#: calls, so callers only need to provide the research topic.
#:
#: The demo server uses the hosted Ollama endpoint at ``https://ollama.com``
#: with the ``glm-5.3-flash:cloud`` model, so visitors can try the demos
#: without running a local model.
OLLAMA_DEFAULTS: dict[str, str] = {
    LlmHeaders.PROVIDER: "ollama",
    LlmHeaders.MODEL: "glm-5.3-flash:cloud",
    LlmHeaders.BASE_URL: "https://ollama.com",
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
        f" {OLLAMA_DEFAULTS[LlmHeaders.MODEL]}.",
        default=OLLAMA_DEFAULTS[LlmHeaders.MODEL].encode(),
    ),
    LlmHeaders.BASE_URL: a11.ActionHeaderSchema(
        LlmHeaders.BASE_URL,
        "Where that provider lives. Defaults to the hosted demo"
        " endpoint; a hosted provider needs no base URL.",
        default=OLLAMA_DEFAULTS[LlmHeaders.BASE_URL].encode(),
    ),
    LlmHeaders.API_KEY: a11.ActionHeaderSchema(
        LlmHeaders.API_KEY,
        "API key for the provider. The demo default is substituted on"
        " the server; bring your own for a different provider.",
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

#: The one rate limiter shared across all connections. In-memory for the
#: single-instance demo server; the ``RateLimiter`` interface is designed
#: so this can be swapped for a distributed store.
_RATE_LIMITER = RateLimiter(
    hourly_limit=5,
    hourly_period_seconds=3600.0,
    daily_limit=10,
    daily_period_seconds=86400.0,
)

#: LLM-enabled action names that count against the rate limit when
#: the caller is using the demo API key.
_LLM_ACTION_NAMES: set[str] = set()


def _real_api_key() -> str:
    """The actual API key the demo backend uses, from the environment."""
    return os.environ.get(DEMO_API_KEY_ENV, "")


def _is_demo_key(action: a11.Action) -> bool:
    """Whether the caller sent the demo placeholder key."""
    key = action.get_header(LlmHeaders.API_KEY.value, decode=True) or ""
    return key.strip() == DEMO_API_KEY_PLACEHOLDER


#: The only model the demo key is allowed to use.
DEMO_ALLOWED_MODEL = "glm-5.3-flash:cloud"

#: Hard server-side timeout for every LLM-enabled demo action.
_LLM_TIMEOUT = a11.Duration.seconds(120)


def _ip_identity(ip: str) -> str:
    """A hashed identity key for the IP-only rate-limit pool."""
    return hashlib.sha256(f"ip:{ip}".encode()).hexdigest()[:32]


def _device_identity(ip: str, fingerprint: str) -> str:
    """A hashed identity key for the per-device rate-limit pool."""
    raw = f"dev:{ip}|{fingerprint}"
    return hashlib.sha256(raw.encode()).hexdigest()[:32]


def _extract_ip(stream: net.WireStream) -> str:
    """Best-effort remote IP from the WebSocket request headers.

    Checks ``x-forwarded-for`` first (a reverse proxy in front), then
    ``x-real-ip``, then falls back to the stream id.
    """
    headers: dict[str, str] = {}
    if hasattr(stream, "request_headers"):
        for name, value in stream.request_headers:
            headers[name.lower()] = value
    forwarded = headers.get("x-forwarded-for", "")
    if forwarded:
        return forwarded.split(",")[0].strip()
    return headers.get("x-real-ip", stream.get_id())


def _wrap_llm_handler(
    handler: a11.ActionHandler,
    ip: str,
    fingerprint: str,
) -> a11.ActionHandler:
    """Wrap an LLM handler with API-key substitution and rate limiting.

    When the caller sends the demo placeholder key, the wrapper:

    1. Checks the rate limiter — and raises ``RESOURCE_EXHAUSTED`` if the
       caller has used their allowance.
    2. Substitutes the real API key so the downstream provider sees it.

    Callers who bring their own key skip both checks.

    A two-minute deadline is enforced for every call, regardless of
    key, so that a runaway model call cannot hold a connection open
    indefinitely.
    """

    async def _inner(action: a11.Action) -> None:
        # Server-enforced timeout: cap every LLM action at 2 minutes.
        a11.set_deadline_header(action, a11.now() + _LLM_TIMEOUT)
        if _is_demo_key(action):
            real_key = _real_api_key()
            if not real_key:
                raise Status(
                    code=StatusCode.FAILED_PRECONDITION,
                    message=(
                        "The demo server's API key is not configured."
                        " Please provide your own key."
                    ),
                ).to_exception()

            # Only the designated model is available with the demo key.
            model = (
                action.get_header(
                    LlmHeaders.MODEL.value, decode=True,
                ) or ""
            )
            if model and model != DEMO_ALLOWED_MODEL:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        f"The demo key only supports"
                        f" {DEMO_ALLOWED_MODEL}. Bring your own"
                        f" API key for a different model."
                    ),
                ).to_exception()

            ip_key = _ip_identity(ip)
            dev_key = _device_identity(ip, fingerprint)
            decision = _RATE_LIMITER.check(ip_key, dev_key)
            if not decision.allowed:
                raise Status(
                    code=StatusCode.RESOURCE_EXHAUSTED,
                    message=decision.reason,
                ).to_exception()

            action.set_header(LlmHeaders.API_KEY.value, real_key)

        return await handler(action)

    return _inner


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

    # `interact_with_llm` wrapped in the handler that records the turn, plus
    # `get_conversation` that reads the recording back. The provider is not
    # chosen here: it arrives per call as a header, which is what lets one
    # registration serve Gemini and Ollama alike.
    conversation_actions.install(registry, store)
    # The full conversation list is not public on this server: a page
    # resolves by id (from the URL) instead of browsing.
    registry.unregister(conversation_actions.GET_CONVERSATIONS_SCHEMA.name)

    # interact_with_llm is LLM-enabled.
    _LLM_ACTION_NAMES.add(INTERACT_WITH_LLM_SCHEMA.name)

    # The same action without the recording, under its own name. A composition's
    # model calls are steps, not chat turns: recorded, each one would arrive in
    # the conversation list as a conversation of its own. So the deep-research
    # flow asks for `ask_model` and the chat guide's page asks for
    # `interact_with_llm`, and only one of the two is history.
    registry.register(
        ASK_MODEL_SCHEMA.name, ASK_MODEL_SCHEMA, interact_with_llm
    )
    _LLM_ACTION_NAMES.add(ASK_MODEL_SCHEMA.name)

    # The one primitive the Flow language does not have (see the module).
    registry.register(
        split_lines.SPLIT_LINES_SCHEMA.name,
        split_lines.SPLIT_LINES_SCHEMA,
        split_lines.split_lines,
    )

    registry.register(
        echo_server.ECHO_SCHEMA.name, echo_server.ECHO_SCHEMA, echo_server.echo
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
        # Every flow entry is a composition that calls ask_model, so the
        # outer action is LLM-enabled.
        _LLM_ACTION_NAMES.add(flow_name)

    if text_to_image:
        from a11.demos import text_to_image as t2i

        registry.register(
            t2i.TEXT_TO_IMAGE_SCHEMA.name,
            t2i.TEXT_TO_IMAGE_SCHEMA,
            t2i.text_to_image,
        )

    return registry


def _extract_fingerprint(stream: net.WireStream) -> str:
    """The browser-supplied fingerprint from the WebSocket request path.

    Browsers cannot set custom WebSocket handshake headers, so the
    fingerprint arrives as the ``fp`` query parameter on the URL the
    client connected to. ``request_path`` includes the query string.
    """
    if hasattr(stream, "request_path"):
        from urllib.parse import parse_qs, urlparse

        parsed = urlparse(stream.request_path)
        values = parse_qs(parsed.query).get("fp", [])
        if values:
            return values[0]
    return ""


def make_service(registry: a11.ActionRegistry) -> Service:
    """A service whose every connection can serve the page's own actions back.

    Each stream gets a copy of the registry, because the tool bridge registers
    the *caller's* tools on it: those actions belong to one page and must not be
    visible on anyone else's session. Same reasoning as
    [a11.gateway.app.A11Gateway][a11.gateway.app.A11Gateway], which is where
    this shape comes from.

    LLM-enabled actions are wrapped per-connection with API-key substitution
    and rate limiting, keyed to the caller's IP and fingerprint.
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

        # Wrap every LLM-enabled Python handler with the demo-key proxy
        # and rate limiter, bound to this connection's identity.
        # NativeActionHandlers (flow-compiled compositions) are not wrapped
        # directly: they call `ask_model` internally, which is a Python
        # handler and gets its own wrapper.
        ip = _extract_ip(stream)
        fingerprint = _extract_fingerprint(stream)
        for action_name in _LLM_ACTION_NAMES:
            handler = per_connection.get_handler(action_name)
            if handler is not None and callable(handler):
                schema = per_connection.get_schema(action_name)
                per_connection.register(
                    action_name,
                    schema,
                    _wrap_llm_handler(handler, ip, fingerprint),
                )

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
