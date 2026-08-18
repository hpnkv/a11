"""The backend the browser guides talk to, over one WebSocket.

Run with ``python -m a11.demos.web_demos_server``. It serves, on
``ws://127.0.0.1:9010/a11-demos``:

* ``interact_with_llm`` — one action for every provider, routed by the
  ``x-a11-llm-*`` headers the browser sets, with each turn recorded in a SQLite
  chunk store (see [a11.gateway.conversations][a11.gateway.conversations]).
* ``get_conversation`` / ``get_conversations`` — how a reloaded page picks a
  conversation up where it left off.
* ``deep-research`` and the flows it is made of, compiled from
  ``deep_research.flow`` beside this file.
* ``text_to_image``, when [a11.demos.text_to_image][a11.demos.text_to_image] can
  reach a Stable Diffusion checkpoint.
* ``__register_tools__`` — the handshake a page uses to announce actions *it*
  serves, so the model's tool calls run in the page (see
  [a11.gateway.tool_bridge][a11.gateway.tool_bridge]).

This is a trimmed `a11 gateway`: the same conversation store, the same tool
bridge, without the shell, audio and flow-authoring actions a gateway also
serves. Anyone already running ``a11 gateway run`` can point the demos at it
instead — the actions the guides call are the ones it has — and everything but
``deep-research`` and ``text_to_image`` will answer. Guides:
``doc/docs/guides/chat-sessions.md``, ``deep-research.md``, ``browser-tools.md``
and ``generative-media.md``.
"""

from __future__ import annotations

import argparse
import asyncio
import pathlib

from absl import logging

import a11
from a11 import flow, net
from a11.demos import split_lines
from a11.gateway import conversation_actions, conversations
from a11.gateway.tool_bridge import RemoteToolBridge
from a11.sdk.interact_with_llm import interact_with_llm
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.service.serving import serving, websocket
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

#: The deep-research composition, beside this file.
DEEP_RESEARCH_FLOW = HERE / "deep_research.flow"

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
    deep_research_program().register_all(registry)

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


async def serve(
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    path: str = DEFAULT_PATH,
    conversation_root: pathlib.Path | None = None,
    *,
    text_to_image: bool = True,
    certificate: str = "",
    private_key: str = "",
) -> None:
    """Serve the demo actions until interrupted.

    With a certificate the listener is `wss://`, which is what a page loaded
    over HTTPS needs: a browser refuses a plaintext WebSocket from a secure
    origin, so a documentation page on HTTPS can only reach a TLS backend.
    """

    store = conversations.ConversationStore(conversation_root)
    service = make_service(make_registry(store, text_to_image=text_to_image))

    options = net.WebSocketServerOptions()
    options.bind_address = host
    options.port = port
    options.path = path
    if certificate:
        # Both halves: `enable_tls` is what the listener reads, and the HTTP/2
        # options carry the material. Assigning the sub-objects back is
        # deliberate -- the native options hand out copies, so mutating
        # `options.http2_options.tls` in place would change nothing.
        options.enable_tls = True
        http2_options = options.http2_options
        tls_options = http2_options.tls
        tls_options.enabled = True
        tls_options.certificate_pem_file = certificate
        tls_options.key_pem_file = private_key
        http2_options.tls = tls_options
        # HTTP/1.1 only, which is what makes a *browser* able to connect.
        # Over TLS the server fixes its protocol from configuration rather than
        # from what ALPN agreed (`tls_http1 = enable_http1 && !enable_h2`, see
        # a11/net/http2.cc), so a listener with h2 left on drives every accepted
        # connection as HTTP/2 and drops the RFC 6455 upgrade a browser sends --
        # the page sees the socket hang up. A11's own clients negotiate either
        # way, so nothing else is given up here.
        http2_options.enable_h2 = False
        http2_options.enable_http1 = True
        options.http2_options = http2_options

    async with serving(service, websocket(options)) as (listener,):
        logging.info(
            "demo server listening at %s://%s:%d%s (conversations in %s)",
            "wss" if certificate else "ws",
            host,
            listener.port,
            path,
            store.root,
        )
        await asyncio.Event().wait()


def main() -> None:
    a11.enable_logging("info")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", default=DEFAULT_PORT, type=int)
    parser.add_argument("--path", default=DEFAULT_PATH)
    parser.add_argument(
        "--conversation-root",
        default=None,
        type=pathlib.Path,
        help="Where to keep the SQLite conversation store.",
    )
    parser.add_argument(
        "--no-text-to-image",
        action="store_true",
        help="Do not serve text_to_image, which needs a diffusion model.",
    )
    parser.add_argument(
        "--certificate",
        default="",
        help="TLS certificate PEM; serves wss:// rather than ws://.",
    )
    parser.add_argument(
        "--private-key", default="", help="TLS private-key PEM."
    )
    args = parser.parse_args()
    if bool(args.certificate) != bool(args.private_key):
        parser.error(
            "--certificate and --private-key must be supplied together"
        )

    try:
        asyncio.run(
            serve(
                args.host,
                args.port,
                args.path,
                args.conversation_root,
                text_to_image=not args.no_text_to_image,
                certificate=args.certificate,
                private_key=args.private_key,
            )
        )
    except KeyboardInterrupt:
        logging.info("demo server stopped")


if __name__ == "__main__":
    main()
