"""Verify usage discovery, independent readers, and nested accounting."""

import asyncio

import pytest

import a11
from a11.sdk.llm import UsageMetadata
from a11.sdk.usage import (
    UsageAccounting,
    UsageLedger,
    UsageReporter,
    UsageUpdate,
    enable_usage_tracking,
)


def schema(name, llm=False):
    return a11.ActionSchema(
        name=name,
        outputs=(
            {
                "usage_sources": a11.ActionPortSchema(
                    "usage_sources", "application/json", typeinfo=dict
                )
            }
            if llm
            else {}
        ),
    )


@pytest.mark.asyncio
async def test_nested_usage_crosses_unaware_actions_and_private_maps():
    async def external(action):
        reporter = UsageReporter(
            action,
            "external",
            usage_source={
                "type": "external_tool",
                "id": "external-call",
                "name": "search",
            },
        )
        await reporter.report(
            UsageMetadata(total_tokens=40, context_tokens=4000)
        )

    async def unaware(action):
        child = action.make_nested(schema("external"), propagate_io=False)
        child.bind_handler(external).run()

    async def root(action):
        action.expose_stream_sources("usage_sources", "llm_usage")
        reporter = UsageReporter(action, "test")
        await reporter.report(UsageMetadata(total_tokens=5), complete=False)
        child = action.make_nested(schema("unaware"))
        child.bind_handler(unaware).run()
        await reporter.report(
            UsageMetadata(total_tokens=100, context_tokens=100)
        )

    action = a11.Action(schema("root", True)).bind_handler(root)
    accounting = UsageAccounting(action)
    collector = asyncio.create_task(accounting.collect())
    action.run()
    await action.wait(a11.Duration.seconds(5))
    await asyncio.wait_for(collector, 5)
    assert len(accounting.sources) == 2
    assert accounting.direct_usage.total_tokens == 100
    assert accounting.descendant_usage.total_tokens == 40
    assert accounting.total_usage.total_tokens == 140
    assert accounting.total_usage.context_tokens == 100
    assert accounting.coverage == "reported_sources"
    replay = await UsageAccounting(action).collect()
    assert replay.total_usage.total_tokens == 140
    public = [value async for value in action["usage_sources"]]
    assert len(public) == 2
    assert {r["source"]["type"] for r in public} == {
        "a11_action",
        "external_tool",
    }


@pytest.mark.asyncio
async def test_missing_report_registers_an_unknown_source():
    async def handler(action):
        reporter = UsageReporter(action, "external")
        await reporter.report(None)

    action = a11.Action(schema("unknown")).bind_handler(handler)
    enable_usage_tracking(action)
    action.run()
    await action.wait(a11.Duration.seconds(5))
    accounting = await UsageAccounting(action).collect()
    assert len(accounting.sources) == 1
    assert accounting.total_usage.total_tokens is None
    assert not accounting.total_usage.complete
    assert accounting.coverage == "incomplete"


def observation(identity, tokens, *, revision=1, covers=()):
    return UsageUpdate(
        id=identity,
        provider="test",
        revision=revision,
        covers=list(covers),
        usage=UsageMetadata(
            usage_id=identity, usage_revision=revision, total_tokens=tokens
        ),
    )


def test_aggregate_coverage_revisions_and_replayed_reports():
    ledger = UsageLedger()
    child = observation("child", 40)
    ledger.observe(child)
    ledger.observe(observation("parent", 100, covers=["child"]))
    assert ledger.totals().total_tokens == 100
    assert not ledger.observe(child)
    ledger.observe(observation("parent", 120, revision=2, covers=["child"]))
    assert ledger.totals().total_tokens == 120
    assert len(ledger.records) == 2


def test_coverage_cycles_are_rejected():
    ledger = UsageLedger()
    ledger.observe(observation("a", 10, covers=["b"]))
    ledger.observe(observation("b", 10, covers=["a"]))
    with pytest.raises(ValueError, match="cycle"):
        ledger.totals()


@pytest.mark.asyncio
@pytest.mark.parametrize("explicit", [False, True])
async def test_disabled_tracking_creates_no_producer_or_reference_nodes(
    explicit,
):
    async def handler(action):
        reporter = UsageReporter(action, "test")
        await reporter.report(UsageMetadata(total_tokens=27))
        assert reporter.usage.total_tokens == 27
        assert reporter.stream_reference is None

    action = a11.Action(schema("disabled", True)).bind_handler(handler)
    if explicit:
        enable_usage_tracking(action, False)
    action.run()
    await action.wait(a11.Duration.seconds(5))
    assert (
        action.get_node_map().get_if_exists(
            a11.Action.make_node_id(action.id, "__streams__")
        )
        is None
    )
    accounting = await UsageAccounting(action).collect()
    assert accounting.coverage == "disabled"
    assert not accounting.total_usage.complete


@pytest.mark.asyncio
async def test_descendant_can_disable_an_inherited_request():
    async def child(action):
        reporter = UsageReporter(action, "test")
        await reporter.report(UsageMetadata(total_tokens=40))
        assert reporter.stream_reference is None

    async def root(action):
        reporter = UsageReporter(action, "test")
        await reporter.report(UsageMetadata(total_tokens=100))
        nested = action.make_nested(schema("child"))
        enable_usage_tracking(nested, False)
        nested.bind_handler(child).run()

    action = a11.Action(schema("root")).bind_handler(root)
    accounting = UsageAccounting(action)
    action.run()
    await action.wait(a11.Duration.seconds(5))
    await accounting.collect()
    assert len(accounting.sources) == 1
    assert accounting.total_usage.total_tokens == 100


@pytest.mark.asyncio
async def test_usage_reads_a_host_resolved_storage_namespace():
    namespace = a11.NodeMap(storage=a11.StorageIdentity("usage-store"))
    host = a11.NodeMap()
    host.register_storage("usage-store", namespace)

    async def leaf(action):
        reporter = UsageReporter(action, "external")
        await reporter.report(UsageMetadata(total_tokens=27))

    async def root(action):
        nested = action.make_nested(schema("leaf"), propagate_io=False)
        nested.bind_node_map(namespace).bind_handler(leaf).run()

    action = a11.Action(schema("root"), node_map=host).bind_handler(root)
    accounting = UsageAccounting(action)
    action.run()
    await action.wait(a11.Duration.seconds(5))
    await accounting.collect()
    assert accounting.total_usage.total_tokens == 27
    assert (
        next(iter(accounting.sources.values())).node.storage.id == "usage-store"
    )


@pytest.mark.asyncio
async def test_cancelling_a_collector_preserves_producer_and_replay():
    release = asyncio.Event()

    async def handler(action):
        reporter = UsageReporter(action, "test")
        await reporter.report(UsageMetadata(total_tokens=21), complete=False)
        await release.wait()
        await reporter.report(UsageMetadata(total_tokens=27))

    action = a11.Action(schema("root")).bind_handler(handler)
    accounting = UsageAccounting(action)
    collecting = asyncio.create_task(accounting.collect())
    action.run()
    while not any(ledger.records for ledger in accounting.ledgers.values()):
        await asyncio.sleep(0)
    collecting.cancel()
    with pytest.raises(asyncio.CancelledError):
        await collecting
    assert not action.cancelled()
    assert accounting.coverage == "incomplete"
    release.set()
    await action.wait(a11.Duration.seconds(5))
    replay = await UsageAccounting(action).collect()
    assert replay.total_usage.total_tokens == 27
