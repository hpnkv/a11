import logging

from a11 import actions
from a11 import net
from a11.gateway import conversations
from a11.gateway.config import GatewayConfig
from a11.gateway.ping import PING_SCHEMA, ping
from a11.gateway import conversation_actions
from a11.gateway.tool_bridge import RemoteToolBridge
from a11.sdk import bash
from a11.sdk.audio import actions as audio_actions
from a11.service.service import Service, ServiceOptions
from a11.service.session import Session


def _make_action_registry(
    config: GatewayConfig,
    conversation_store: conversations.ConversationStore,
):
    registry = actions.ActionRegistry()

    registry.register(PING_SCHEMA.name, PING_SCHEMA, ping)

    conversation_actions.install(registry, conversation_store)

    if config.shell_tools:
        for schema, handler in bash.SHELL_ACTIONS:
            registry.register(schema.name, schema, handler)

    # Both groups are gated independently. Until now `--no-audio-capture` did
    # nothing at all and `--no-speech-recognition` silently took the capture
    # actions down with it, because registration was all-or-nothing.
    audio_actions.register(
        registry,
        capture=config.audio_capture,
        recognition=config.speech_recognition,
    )

    return registry


class A11Gateway:
    """The gateway's shared state, and one Session per accepted stream.

    Which tools a turn may use is the *caller's* decision, expressed as the
    allowed-action patterns on its ``interact_with_llm`` call: a client that
    says ``shell_.*`` is offered this side's shell tools, and one that does not
    is not (see [collect_tools][a11.sdk.llm_tools.runner.collect_tools]). So the
    gateway registers everything it can serve and lets each call narrow it.
    """

    _conversation_store: conversations.ConversationStore
    _action_registry: actions.ActionRegistry

    def __init__(
        self,
        conversation_store: conversations.ConversationStore,
        action_registry: actions.ActionRegistry,
    ):
        self._conversation_store = conversation_store
        self._action_registry = action_registry
        self.service = Service(
            action_registry=action_registry,
            on_connection=self._on_connection,
            # False: the hook makes the copy itself, because the bridge must own
            # the very copy it registers the peer's tools on.
            options=ServiceOptions(copy_registry_per_connection=False),
        )

    @property
    def accept(self):
        """The service's on-stream callback, for any transport listener."""
        return self.service.accept

    async def _on_connection(
        self, session: Session, stream: net.WireStream
    ) -> None:
        """Specialise one connection before its session starts pumping."""
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

        # A copy per connection, because the tool bridge registers the caller's
        # own tools on it: those actions belong to this peer and must not be
        # visible (or callable) on anyone else's session.
        registry = self._action_registry.copy()
        bridge = RemoteToolBridge()
        bridge.install(registry)
        session.set_action_registry(registry)
        # The bridge reverse-dispatches the model's tool calls back over this
        # same stream, to the tools the peer announced on it.
        bridge.bind_session(session, stream)

    async def handle_stream(self, stream: net.WireStream):
        """Serve one stream to completion.

        Kept as the name every existing caller uses; `accept` is the same thing.
        """
        await self.service.accept(stream)


def init_app(config: GatewayConfig | None = None) -> A11Gateway:
    """Build a gateway from `config`, defaulting to serving everything."""

    from a11 import logging as a11_logging

    a11_logging.enable("info")

    resolved = config if config is not None else GatewayConfig()
    conversation_store = conversations.get_conversation_store(
        resolved.conversation_store_root
    )
    action_registry = _make_action_registry(resolved, conversation_store)

    return A11Gateway(conversation_store, action_registry)
