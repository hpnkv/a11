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

from typing import Awaitable, cast, Callable

from absl import logging

from a11 import actions
from a11 import timing
from a11.gateway import conversations
from a11.nodes.async_node import AsyncNode
from a11.sdk.interact_with_llm import interact_with_llm
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.sdk.llm import Interaction
from a11.sdk.usage import parse_update

#: Maximum wait while harvesting a turn's input ports.
_HARVEST_TIMEOUT = timing.Duration(30_000_000_000)

GET_CONVERSATIONS_SCHEMA = actions.ActionSchema(
    name="get_conversations",
    description="List the stored conversations, most recently active first.",
    outputs={
        "conversations": actions.ActionPortSchema(
            name="conversations",
            type="application/json",
            description="One `{id, title, started_at}` per conversation.",
            typeinfo=dict,
            required=True,
        )
    },
)


async def get_conversations(
    action: actions.Action,
    store: conversations.ConversationStore | None = None,
) -> None:
    store = store or conversations.get_conversation_store()

    conversations_node = action["conversations"]
    for summary in await store.list():
        await conversations_node.put(summary)
    await conversations_node.finalize()


GET_CONVERSATION_SCHEMA = actions.ActionSchema(
    name="get_conversation",
    description="Stream one conversation's interactions, oldest first.",
    inputs={
        "id": actions.ActionPortSchema(
            name="id",
            type="text/plain",
            description="The conversation's id: its first interaction's id.",
            typeinfo=str,
            unary=True,
            required=True,
        )
    },
    outputs={
        "interactions": actions.ActionPortSchema(
            name="interactions",
            type="application/json",
            description="The conversation, as the interactions it is made of.",
            typeinfo=Interaction,
            required=True,
        )
    },
)


async def get_conversation(
    action: actions.Action,
    store: conversations.ConversationStore | None = None,
) -> None:
    store = store or conversations.get_conversation_store()

    conversation_id = cast(str, await action["id"].consume(str))

    # Unknown ids produce empty streams.
    interactions_out = action["interactions"]
    for interaction in await store.read(conversation_id):
        await interactions_out.put(interaction)
    await interactions_out.finalize()


async def _persist(
    base_interactions_port: AsyncNode,
    new_interactions_port: AsyncNode,
    store: conversations.ConversationStore | None = None,
) -> None:
    # Bound reads because an input port may remain open if its caller stops
    # writing before draining the end marker.
    base_interactions = await conversations.read_interactions(
        base_interactions_port.get_chunk_store(), _HARVEST_TIMEOUT
    )
    new_interactions = await conversations.read_interactions(
        new_interactions_port.get_chunk_store(), _HARVEST_TIMEOUT
    )

    # Tool logs already travel in interaction metadata with the turn.
    store = store or conversations.get_conversation_store()
    await store.record([*base_interactions, *new_interactions])


async def interact_with_llm_and_persist(
    action: actions.Action,
    store: conversations.ConversationStore | None = None,
) -> None:
    # Grab the ports before delegating; reading their stores afterwards is
    # safe because nothing clears them (`ActionSettings.clear_*_after_run`
    # and `ChunkStoreReaderOptions.pop_chunks` all default to False).
    base_interactions_port = action.get_port("interactions")
    new_interactions_port = action.get_port("new_interactions")
    store = store or conversations.get_conversation_store()
    base_interactions = None

    async def record_event(event) -> None:
        nonlocal base_interactions
        update = parse_update(event)
        if update is None:
            return
        if base_interactions is None:
            base_interactions = await conversations.read_interactions(
                base_interactions_port.get_chunk_store(), _HARVEST_TIMEOUT
            )
        await store.record_usage(base_interactions, update)

    await interact_with_llm(action, event_observer=record_event)

    # Record successful turns; the client replays failed turns on its next call.
    try:
        await _persist(base_interactions_port, new_interactions_port, store)
    except Exception:
        # A completed answer is worth more than its history entry.
        logging.warning("failed to record the conversation", exc_info=True)


_HandlerWithStore = Callable[
    [actions.Action, conversations.ConversationStore | None], Awaitable[None]
]


def bind_store(
    handler: _HandlerWithStore,
    store: conversations.ConversationStore,
) -> actions.ActionHandler:
    async def _inner(action: actions.Action) -> None:
        return await handler(action, store)

    return _inner


def install(
    registry: actions.ActionRegistry,
    store: conversations.ConversationStore | None = None,
) -> None:
    store = store or conversations.get_conversation_store()

    registry.register(
        INTERACT_WITH_LLM_SCHEMA.name,
        INTERACT_WITH_LLM_SCHEMA,
        bind_store(interact_with_llm_and_persist, store),
    )

    registry.register(
        GET_CONVERSATION_SCHEMA.name,
        GET_CONVERSATION_SCHEMA,
        bind_store(get_conversation, store),
    )

    registry.register(
        GET_CONVERSATIONS_SCHEMA.name,
        GET_CONVERSATIONS_SCHEMA,
        bind_store(get_conversations, store),
    )
