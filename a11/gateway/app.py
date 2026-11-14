# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import logging

from a11 import actions, net
from a11.gateway import conversation_actions, conversations
from a11.gateway.config import GatewayConfig
from a11.gateway.tool_bridge import RemoteToolBridge
from a11.sdk import bash, flow_tools
from a11.sdk.audio import actions as audio_actions
from a11.sdk.llm_context.actions import register as register_llm_context
from a11.sdk.llm_context.config import (
    LlmContextServices,
    bind_services,
    configure_registry,
    decode_config,
)
from a11.service.service import Service, ServiceOptions
from a11.service.session import Session


def _make_action_registry(
    config: GatewayConfig,
    conversation_store: conversations.ConversationStore,
):
    registry = actions.ActionRegistry()

    conversation_actions.install(registry, conversation_store)
    if config.llm_context_config is not None:
        configure_registry(
            registry,
            decode_config(config.llm_context_config),
            services=config.llm_context_services,
        )
        register_llm_context(registry, services=config.llm_context_services)

    if config.coding_tools:
        from a11.cli.coding_agent import ApprovalMode, CodingAgent, SandboxMode

        CodingAgent.install(
            registry,
            cwd=config.coding_cwd,
            add_dirs=config.coding_add_dirs,
            approval_mode=ApprovalMode(config.coding_approval),
            sandbox_mode=SandboxMode(config.coding_sandbox),
            approve=None,
            command_tools=config.shell_tools,
            flow_tools=config.flow_tools,
        )
    elif config.shell_tools:
        for schema, handler in bash.SHELL_ACTIONS:
            registry.register(schema.name, schema, handler)

    # Capture and recognition are registered independently.
    audio_actions.register(
        registry,
        capture=config.audio_capture,
        recognition=config.speech_recognition,
    )

    # Register Flow last so compositions can call the actions above.
    if config.flow_tools:
        flow_tools.register(registry)

    return registry


class A11Gateway:
    """The gateway's shared state, and one Session per accepted stream.

    Each ``interact_with_llm`` call selects its available tools through
    allowed-action patterns. See
    [collect_tools][a11.sdk.llm_tools.runner.collect_tools].
    """

    _conversation_store: conversations.ConversationStore
    _action_registry: actions.ActionRegistry

    def __init__(
        self,
        conversation_store: conversations.ConversationStore,
        action_registry: actions.ActionRegistry,
        *,
        llm_context_services: LlmContextServices | None = None,
    ):
        self._conversation_store = conversation_store
        self._action_registry = action_registry
        self._llm_context_services = llm_context_services
        self.service = Service(
            action_registry=action_registry,
            on_connection=self._on_connection,
            # The hook copies the registry that receives the peer's tools.
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
        if self._llm_context_services is not None:
            bind_services(registry, self._llm_context_services)
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

    resolved = config if config is not None else GatewayConfig()
    conversation_store = conversations.get_conversation_store(
        resolved.conversation_store_root
    )
    action_registry = _make_action_registry(resolved, conversation_store)

    return A11Gateway(
        conversation_store,
        action_registry,
        llm_context_services=resolved.llm_context_services,
    )
