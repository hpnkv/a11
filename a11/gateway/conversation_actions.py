from typing import Awaitable, cast, Callable

from absl import logging

from a11 import actions
from a11 import timing
from a11.gateway import conversations
from a11.nodes.async_node import AsyncNode
from a11.sdk.interact_with_llm import interact_with_llm
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.sdk.llm import Interaction

#: Ports are terminated by the time they are read, so this only bounds a
#: pathology: 30s of a stuck port instead of a turn that never records.
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

    async with action["conversations"] as conversations_node:
        for summary in await store.list():
            await conversations_node.put(summary)
        await conversations_node.put_null_final()


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

    # An unknown id yields an empty conversation rather than an error.
    async with action["interactions"] as interactions_out:
        for interaction in await store.read(conversation_id):
            await interactions_out.put(interaction)
        await interactions_out.put_null_final()


async def _persist(
    base_interactions_port: AsyncNode,
    new_interactions_port: AsyncNode,
    store: conversations.ConversationStore | None = None,
) -> None:
    # Both ports are terminated by the time the handler returns -- it read the
    # conversation to the end and closed what it produced -- so this only reads
    # what is already there. The read is still bounded rather than checked
    # against the writers: an input port's local writer closes when the caller
    # drains its end (the closure marker crosses the wire), but a caller that
    # simply stops writing without draining leaves it open, and a port that has
    # not ended should cost a timeout rather than a stuck turn.
    base_interactions = await conversations.read_interactions(
        base_interactions_port.get_chunk_store(), _HARVEST_TIMEOUT
    )
    new_interactions = await conversations.read_interactions(
        new_interactions_port.get_chunk_store(), _HARVEST_TIMEOUT
    )

    # Tool run logs need nothing done here: a backend records them in the
    # metadata of the interaction that carries the tool results (see
    # `a11.sdk.llm.TOOL_LOGS_METADATA_KEY`), so they are already part of what
    # crossed these ports and are stored with the rest of the turn.
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

    await interact_with_llm(action)

    # Only a turn that got this far is recorded, which matches the client:
    # it appends to its own history on success only, so a failed turn is
    # replayed in full next time and recorded then.
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
