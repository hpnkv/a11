# Copyright 2026 The A11 Authors.

"""The shared derivation from Interactions to renderable blocks.

What matters here is the contract every client depends on: which blocks a turn
produces, in what order, that a tool call is paired with the log that arrived in
a *later* interaction, and that the live and replay feeders agree.
"""

from __future__ import annotations

import json

import a11
from a11.sdk import presentation
from a11.sdk.llm import (
    TOOL_LOGS_METADATA_KEY,
    Interaction,
    Role,
    UsageMetadata,
)
from a11.sdk.presentation import (
    BlockKind,
    PresentationReducer,
    present_conversation,
    present_interaction,
)
from a11.status import Status, StatusCode


def _text_interaction(text: str, role: Role = Role.USER) -> Interaction:
    """An interaction as a *client* mints one: no backend tag at all."""
    return Interaction(
        role=role,
        content=[a11.to_chunk({"role": "user", "content": [
            {"type": "text", "text": text}
        ]})],
    )


def _call_interaction(call_id: str, name: str) -> Interaction:
    return Interaction(
        role=Role.ASSISTANT,
        content=[a11.to_chunk({"role": "model", "content": [
            {"type": "text", "text": "Let me check."}
        ]})],
        action_calls=[a11.ActionMessage(id=call_id, name=name)],
    )


def _result_interaction(call_id: str, log: str) -> Interaction:
    """The bookkeeping half of a tool round trip, carrying the log."""
    return Interaction(
        role=Role.USER,
        action_outputs={call_id: []},
        backend_specific_metadata={
            TOOL_LOGS_METADATA_KEY: json.dumps({call_id: log}).encode()
        },
    )


def test_an_untagged_client_interaction_still_presents():
    """The case that made every client hand-roll its own text extraction."""
    turn = present_interaction(_text_interaction("hello there"))

    assert [block.kind for block in turn.blocks] == [BlockKind.TEXT]
    assert turn.blocks[0].text == "hello there"
    assert turn.role == Role.USER


def test_plain_text_reads_content_without_a_backend_tag():
    assert presentation.plain_text(_text_interaction("a title")) == "a title"


def test_a_tool_call_is_paired_with_a_log_from_a_later_interaction():
    """The log arrives in the *next* interaction, and belongs to this block."""
    interactions = [
        _text_interaction("run something"),
        _call_interaction("call-1", "shell_execute"),
        _result_interaction("call-1", "$ pwd\n/home/helena"),
        _text_interaction("Done.", role=Role.ASSISTANT),
    ]

    turns = present_conversation(interactions)

    # The result carrier is not drawn: it is bookkeeping for the model.
    assert len(turns) == 3
    run = [
        block
        for turn in turns
        for block in turn.blocks
        if block.kind == BlockKind.TOOL_RUN
    ]
    assert len(run) == 1
    assert run[0].id == "call-1"
    assert run[0].tool_name == "shell_execute"
    assert run[0].text == "$ pwd\n/home/helena"


def test_replay_orders_text_before_tool_runs():
    """A stored interaction has no timeline, so the order is a stable choice."""
    turn = present_interaction(
        _call_interaction("call-1", "shell_execute"), {"call-1": "log"}
    )
    assert [block.kind for block in turn.blocks] == [
        BlockKind.TEXT,
        BlockKind.TOOL_RUN,
    ]


def test_a_result_carrier_is_recognised_only_when_it_has_no_text():
    assert presentation.is_tool_result_carrier(
        _result_interaction("call-1", "log")
    )
    # An interaction with both results and prose is worth drawing.
    with_text = _result_interaction("call-2", "log")
    with_text.content = [a11.to_chunk({"role": "user", "content": [
        {"type": "text", "text": "and also this"}
    ]})]
    assert not presentation.is_tool_result_carrier(with_text)


def test_usage_and_failure_become_their_own_blocks():
    interaction = _text_interaction("partial", role=Role.ASSISTANT)
    interaction.usage_metadata = UsageMetadata(input_tokens=10, output_tokens=2)
    interaction.status = Status(
        code=StatusCode.DEADLINE_EXCEEDED, message="took too long"
    )

    kinds = [block.kind for block in present_interaction(interaction).blocks]
    assert kinds == [BlockKind.TEXT, BlockKind.USAGE, BlockKind.ERROR]


def test_system_interactions_are_not_drawn():
    turns = present_conversation([
        _text_interaction("you are a helpful assistant", role=Role.SYSTEM),
        _text_interaction("hi"),
    ])
    assert len(turns) == 1
    assert turns[0].role == Role.USER


def test_tool_logs_survives_malformed_metadata():
    interaction = _text_interaction("x")
    interaction.backend_specific_metadata = {
        TOOL_LOGS_METADATA_KEY: b"not json at all"
    }
    # A broken log is not a reason to fail drawing a conversation.
    assert presentation.tool_logs(interaction) == {}


# -- the reducer: live and replay through one path -------------------------


def test_deltas_coalesce_into_one_block_per_run():
    reducer = PresentationReducer()
    for piece in ("Hel", "lo ", "world"):
        reducer.on_text(piece)
    reducer.end_turn()

    assert [block.kind for block in reducer.blocks] == [BlockKind.TEXT]
    assert reducer.blocks[0].text == "Hello world"
    # Closed, so a client knows not to draw a cursor.
    assert not reducer.blocks[0].partial


def test_a_block_is_partial_until_the_turn_ends():
    reducer = PresentationReducer()
    reducer.on_text("streaming")
    assert reducer.blocks[0].partial
    reducer.end_turn()
    assert not reducer.blocks[0].partial


def test_thoughts_and_text_form_separate_blocks_in_arrival_order():
    reducer = PresentationReducer()
    reducer.on_thought("I should check the time")
    reducer.on_text("It is noon")
    reducer.on_thought("second thought")
    reducer.end_turn()

    assert [block.kind for block in reducer.blocks] == [
        BlockKind.THOUGHT,
        BlockKind.TEXT,
        BlockKind.THOUGHT,
    ]


def test_streamed_text_is_not_repeated_by_the_interaction_that_follows():
    """The same prose arrives twice on the live path; it must be drawn once."""
    reducer = PresentationReducer()
    reducer.on_text("Let me check.")
    reducer.on_interaction(_call_interaction("call-1", "shell_execute"))
    reducer.on_interaction(_result_interaction("call-1", "$ pwd"))
    reducer.end_turn()

    kinds = [block.kind for block in reducer.blocks]
    assert kinds == [BlockKind.TEXT, BlockKind.TOOL_RUN]
    assert reducer.blocks[0].text == "Let me check."
    # The log landed in the following interaction and was folded back in.
    assert reducer.blocks[1].text == "$ pwd"


def test_the_same_tool_call_is_never_drawn_twice():
    reducer = PresentationReducer()
    call = _call_interaction("call-1", "shell_execute")
    reducer.on_interaction(call)
    reducer.on_interaction(call)
    reducer.end_turn()

    runs = [b for b in reducer.blocks if b.kind == BlockKind.TOOL_RUN]
    assert len(runs) == 1


def test_an_error_closes_the_turn_with_its_status():
    reducer = PresentationReducer()
    reducer.on_text("partial answer")
    reducer.on_error(Status(code=StatusCode.INTERNAL, message="boom"))

    assert reducer.blocks[-1].kind == BlockKind.ERROR
    assert reducer.blocks[-1].status.message == "boom"
    # The text that did arrive is kept, and closed.
    assert reducer.blocks[0].text == "partial answer"
    assert not reducer.blocks[0].partial


def test_a_sink_sees_open_append_and_close():
    events: list[tuple[str, str]] = []

    class Recorder:
        def on_block_opened(self, block):
            events.append(("open", block.kind.value))

        def on_block_appended(self, block, delta):
            events.append(("append", delta))

        def on_block_closed(self, block):
            events.append(("close", block.kind.value))

    reducer = PresentationReducer(Recorder())
    reducer.on_text("ab")
    reducer.on_text("cd")
    reducer.end_turn()

    assert events == [
        ("open", "text"),
        ("append", "ab"),
        ("append", "cd"),
        ("close", "text"),
    ]


def test_the_reducer_and_present_conversation_agree_on_a_replayed_turn():
    """One model, two feeders: replay through the reducer matches the pure fn."""
    interactions = [
        _call_interaction("call-1", "shell_execute"),
        _result_interaction("call-1", "$ ls"),
    ]

    reducer = PresentationReducer()
    for interaction in interactions:
        reducer.on_interaction(interaction)
    reducer.end_turn()

    direct = [
        block for turn in present_conversation(interactions)
        for block in turn.blocks
    ]
    assert [b.kind for b in reducer.blocks] == [b.kind for b in direct]
    assert [b.text for b in reducer.blocks] == [b.text for b in direct]
