import argparse
import logging
from typing import cast

from a11 import actions
from a11 import net
from a11.gateway import conversations
from a11.gateway import conversation_actions
from a11.gateway.tool_bridge import RemoteToolBridge
from a11.sdk import bash
from a11.sdk.audio import actions as audio_actions
from a11.service.session import Session, SessionOptions
from a11.status import Status

PING_SCHEMA = actions.ActionSchema(
    name="__ping",
    description=(
        "Ping the server to check if it is alive. Requires a single value on"
        " the port `input`, which it returns as a single value on the port"
        " `output`."
    ),
    inputs={
        "input": actions.ActionPortSchema(
            name="input",
            description="Ping input value",
            type="text/plain",
            typeinfo=str,
        ),
    },
    outputs={
        "output": actions.ActionPortSchema(
            name="output",
            description="Pong response value",
            type="text/plain",
            typeinfo=str,
        ),
    },
)


async def _ping(action: actions.Action):
    stream_str = "<no stream>"
    if action.get_stream():
        stream_str = str(action.get_stream().get_id())

    logging.info(f"[{stream_str}] running ping on stream {stream_str}")
    async with action["output"] as output_node:
        ping_value = cast(str, await action["input"].consume(str))
        await output_node.put_final(ping_value)

    logging.info(f"[{stream_str}] ping complete")


def _make_action_registry(
    args: argparse.Namespace,
    conversation_store: conversations.ConversationStore,
):
    registry = actions.ActionRegistry()

    registry.register(PING_SCHEMA.name, PING_SCHEMA, _ping)

    conversation_actions.install(registry, conversation_store)

    if not args.no_shell_tools:
        for schema, handler in bash.SHELL_ACTIONS:
            registry.register(schema.name, schema, handler)

    if not args.no_audio_capture:
        pass

    if not args.no_speech_recognition:
        audio_actions.register(registry)

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

    async def handle_stream(self, stream: net.WireStream):
        logging.info("incoming stream: %s", stream)

        # A copy per connection, because the tool bridge registers the caller's
        # own tools on it: those actions belong to this peer and must not be
        # visible (or callable) on anyone else's session.
        registry = self._action_registry.copy()
        bridge = RemoteToolBridge()
        bridge.install(registry)

        session = Session(
            action_registry=registry,
            options=SessionOptions(),
        )
        # The bridge reverse-dispatches the model's tool calls back over this
        # same stream, to the tools the peer announced on it.
        bridge.bind_session(session, stream)

        await session.add_stream(stream, mode="accept")
        await session.done.wait()

        status: Status = session.get_status()
        if not status.is_ok():
            logging.info("Session failed: %s", status)


def init_app(args: argparse.Namespace) -> A11Gateway:
    conversation_store = conversations.get_conversation_store(
        args.conversation_store_root
    )
    action_registry = _make_action_registry(args, conversation_store)

    return A11Gateway(conversation_store, action_registry)
