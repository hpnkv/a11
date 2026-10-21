"""Verify usage revisions across failures, history changes, and persistence."""

import importlib
import sqlite3
import asyncio

import pytest
from pydantic_core import to_json

import a11
from a11.cli.chat_tokens import ChatTokens
from a11.cli.backends import make_user_interaction
from a11.client.turn import TurnConfig, run_turn
from a11.gateway.config import GatewayConfig
from a11.gateway.conversations import ConversationStore
from a11.gateway.embedded import embedded_gateway
from a11.sdk.llm import Interaction, Role, UsageMetadata
from a11.sdk.presentation import BlockKind, PresentationReducer
from a11.sdk.usage import (
    METADATA_KEY,
    UsageLedger,
    UsageReporter,
    UsageUpdate,
    interaction_updates,
    usage_delta,
    closing_stream,
)
from a11.status import Status, StatusCode, StatusException


def update(identity, revision, **counts):
    return UsageUpdate(
        id=identity,
        provider="test",
        revision=revision,
        usage=UsageMetadata(
            usage_id=identity, usage_revision=revision, **counts
        ),
    )


def test_revisions_preserve_consumption_and_latest_context():
    events = [
        update(
            "request", 1, output_tokens=10, total_tokens=100, complete=False
        ),
        update("compact", 1, context_invalidated=True, complete=False),
        update(
            "request",
            2,
            output_tokens=30,
            total_tokens=500,
            context_tokens=80,
            usage_scope="turn",
        ),
    ]
    reducer = PresentationReducer()
    ledger = UsageLedger()
    live_counter = ChatTokens()
    for event in [*events, events[0], events[2]]:
        ledger.observe(event)
        reducer.on_usage_event(event.model_dump(mode="json"))
        for block in reducer.blocks:
            live_counter.observe(block)
    live_counter.finish()
    assert ledger.totals().total_tokens == 500
    assert ledger.totals().complete
    assert live_counter._output == 30
    assert live_counter.context == 80
    replay = PresentationReducer()
    replay.on_interaction(
        Interaction(
            role=Role.USER,
            backend_specific_metadata={
                METADATA_KEY: to_json(
                    [e.model_dump(mode="json") for e in events]
                )
            },
        )
    )
    counter = ChatTokens()
    for block in replay.blocks:
        counter.observe(block)
    counter.finish()
    assert counter.output_label == live_counter.output_label
    assert counter.context == live_counter.context
    assert len([b for b in replay.blocks if b.kind == BlockKind.USAGE]) == 2


def test_incomplete_usage_remains_estimated_after_finish():
    reducer = PresentationReducer()
    reducer.on_usage_event(
        update("partial", 1, output_tokens=5, complete=False).model_dump(
            mode="json"
        )
    )
    counter = ChatTokens()
    for block in reducer.blocks:
        counter.observe(block)
    counter.finish()
    assert counter.output_label == "~5 tok"
    assert UsageLedger().totals().total_tokens is None


def test_deferred_usage_does_not_split_streamed_text():
    reducer = PresentationReducer()
    reducer.defer_usage()
    reducer.on_text("A")
    reducer.on_usage_event(
        update(
            "turn", 1, input_tokens=100, output_tokens=1, complete=False
        ).model_dump(mode="json")
    )
    reducer.on_text("11")
    assert len(reducer.blocks) == 1
    assert reducer.blocks[0].text == "A11"
    reducer.on_usage_event(
        update("turn", 2, input_tokens=100, output_tokens=20).model_dump(
            mode="json"
        )
    )
    reducer.end_turn()
    assert [b.kind for b in reducer.blocks] == [BlockKind.TEXT, BlockKind.USAGE]
    assert reducer.blocks[-1].usage.output_tokens == 20


def test_cumulative_usage_requires_a_verified_baseline_and_detects_resets():
    current = UsageMetadata(
        input_tokens=300, output_tokens=20, total_tokens=320
    )
    previous = UsageMetadata(
        input_tokens=100, output_tokens=7, total_tokens=107
    )
    assert usage_delta(current, previous).total_tokens == 213
    assert usage_delta(current, previous).complete
    assert usage_delta(current, None).total_tokens is None
    assert not usage_delta(current, None).complete
    reset = usage_delta(previous, current)
    assert reset.total_tokens is None
    assert not reset.complete


def test_reconciliation_rows_do_not_add_to_consumption_or_presentation():
    aggregate = update("aggregate", 1, total_tokens=100)
    detail = update("detail", 1, total_tokens=40)
    detail.contribution = "reconciliation"
    ledger = UsageLedger()
    reducer = PresentationReducer()
    for event in [aggregate, detail]:
        ledger.observe(event)
        reducer.on_usage_event(event)
    assert ledger.totals().total_tokens == 100
    assert len(reducer.blocks) == 1


@pytest.mark.asyncio
async def test_cancelled_provider_stream_closes_its_response():
    entered = asyncio.Event()
    closed = asyncio.Event()

    class Stream:
        async def close(self):
            closed.set()

    async def consume():
        async with closing_stream(Stream()):
            entered.set()
            await asyncio.Event().wait()

    task = asyncio.create_task(consume())
    await entered.wait()
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert closed.is_set()


@pytest.mark.asyncio
async def test_usage_survives_restart_and_replayed_history_without_extra_tables(
    tmp_path,
):
    store = ConversationStore(tmp_path)
    question = make_user_interaction("hello")
    first = update("request", 1, output_tokens=5, complete=False)
    final = update(
        "request", 2, input_tokens=20, output_tokens=10, total_tokens=30
    )
    await store.record_usage([question], first)
    await store.record_usage([question], final)
    await store.record([question])
    await store.record_usage([question], final)
    restored = await ConversationStore(tmp_path).read(question.id)
    assert len(restored) == 1
    assert [e.revision for e in interaction_updates(restored[0])] == [1, 2]
    with sqlite3.connect(tmp_path / "conversations.sqlite") as db:
        assert db.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        ).fetchall() == [("conversations",)]


@pytest.mark.asyncio
async def test_gateway_persists_reported_usage_before_consumed_token_failure(
    tmp_path, monkeypatch
):
    router = importlib.import_module("a11.sdk.interact_with_llm")
    gateway_store = importlib.import_module("a11.gateway.conversations")
    monkeypatch.setattr(gateway_store, "_STORE", None)

    async def failing_provider(action):
        _ = [i async for i in action["interactions"]]
        await action["config"].consume(dict, allow_none=True)
        _ = [tool async for tool in action["tools"]]
        reporter = UsageReporter(action, "gpt", "test")
        await reporter.report(
            UsageMetadata(input_tokens=20, output_tokens=7, total_tokens=27),
            {"input_tokens": 20, "output_tokens": 7, "total_tokens": 27},
        )
        raise Status(
            code=StatusCode.RESOURCE_EXHAUSTED, message="probe limit"
        ).to_exception()

    monkeypatch.setattr(router, "_load_handler", lambda _: failing_provider)
    settings = GatewayConfig(
        conversation_store_root=tmp_path,
        shell_tools=False,
        audio_capture=False,
        speech_recognition=False,
    )
    question = make_user_interaction("hello")
    reducer = PresentationReducer()
    async with embedded_gateway(settings) as connection:
        with pytest.raises(StatusException):
            await run_turn(
                connection,
                [],
                question,
                (),
                TurnConfig(provider="gpt", model="test"),
                reducer,
            )
        stored = await gateway_store.get_conversation_store().read(question.id)
        events = interaction_updates(stored[0])
        assert events[-1].usage.total_tokens == 27
        assert events[-1].source_usage.model_extra["total_tokens"] == 27


def test_codex_regressed_snapshot_keeps_observed_counts_and_marks_gap():
    from a11.sdk.openai.interact_with_codex import _TurnUsage

    state = _TurnUsage()
    state.fresh_thread = True
    state.turn_id = "turn"
    latest = {"inputTokens": 100, "outputTokens": 20, "totalTokens": 120}
    state.update(
        {"turnId": "turn", "tokenUsage": {"total": latest, "last": latest}}
    )
    older = {"inputTokens": 50, "outputTokens": 10, "totalTokens": 60}
    state.update(
        {"turnId": "turn", "tokenUsage": {"total": older, "last": older}}
    )
    assert state.metadata().total_tokens == 120
    assert state.metadata().complete is False
    assert state.metadata().context_tokens is None
