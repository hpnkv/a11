# Copyright 2026 The A11 Authors.

"""Recording a turn: the two ports it crossed become the stored conversation.

``interact_with_llm_and_persist`` harvests the turn from the ports the real
handler already used -- the replayed conversation on ``interactions``, the
model's own turns on ``new_interactions`` -- so this drives ``_persist`` with
those two ports directly. Whether the ports have ended is the part worth
pinning: an input port is filled from the wire and its local writer is never
closed, so the harvest may not depend on a writer's status.
"""

import pathlib

import pytest

import a11
from a11.gateway import conversation_actions
from a11.gateway.conversations import ConversationStore
from a11.nodes.async_node import AsyncNode
from a11.sdk.llm import Interaction, Role


def _interaction(text: str, previous: str = "") -> Interaction:
    return Interaction(
        role=Role.USER,
        previous_interaction_id=previous,
        content=[a11.to_chunk({"role": "user", "content": text})],
    )


async def _port(name: str, interactions: list[Interaction]) -> AsyncNode:
    """A terminated node carrying ``interactions``, as a finished port would."""
    node = AsyncNode.create(name)
    for interaction in interactions:
        await node.put(interaction, mimetype="application/json")
    await node.finalize()
    return node


@pytest.mark.asyncio
async def test_a_turn_is_recorded_from_both_of_its_ports(
    tmp_path: pathlib.Path,
):
    store = ConversationStore(tmp_path)
    question = _interaction("what file am I looking at?")
    answer = _interaction("the one in the editor.", previous=question.id)

    await conversation_actions._persist(
        await _port("base", [question]),
        await _port("new", [answer]),
        store,
    )

    # The conversation is named by its first interaction, and holds the turn
    # whole: the user's message only ever existed on the input port, the model's
    # only on the output one.
    listed = await store.list()
    assert [row["id"] for row in listed] == [question.id]
    assert [i.id for i in await store.read(question.id)] == [
        question.id,
        answer.id,
    ]


@pytest.mark.asyncio
async def test_recording_the_same_turn_twice_adds_nothing(
    tmp_path: pathlib.Path,
):
    """The client replays its history every turn, and may retry a failed one."""
    store = ConversationStore(tmp_path)
    question = _interaction("hello")
    answer = _interaction("hi", previous=question.id)

    for _ in range(2):
        await conversation_actions._persist(
            await _port("base", [question]),
            await _port("new", [answer]),
            store,
        )

    assert len(await store.read(question.id)) == 2
