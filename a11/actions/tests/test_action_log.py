"""``Action.log`` and ``Action.logf``: what they write and where it goes.

The Python half of the log surface is thin on purpose -- the port, the metadata
and the lifecycle are the native layer's, pinned by
``cpp/tests/action_log_test.cc`` -- so what is worth pinning here is the part
that is Python's: that a ``str`` is
text and a ``dict`` is JSON without being told, that the source location is the
caller's rather than A11's, and that the records reach the ``a11.action`` logger
exactly once.
"""

import asyncio
import json
import logging
import pathlib

import pytest

import a11
from a11 import _native
from a11.actions import Action, ActionPortSchema, ActionSchema
from a11.data import types
from a11 import logging as a11_logging
from a11.logging import ACTION_LOGGER_NAME
from a11.nodes.async_node import NodeMap
from a11.status import StatusException


def _schema(name: str = "quiet") -> ActionSchema:
    return ActionSchema(
        name=name,
        outputs={"out": ActionPortSchema(name="out", type="text/plain")},
    )


class _Sink:
    """Collects what the process action-log sink is handed, then puts it back.

    The slot is process-wide, so a test that takes it has to give it back even
    when an assertion fails partway.
    """

    def __init__(self) -> None:
        self.records: list[dict[str, object]] = []

    def __enter__(self) -> "_Sink":
        a11_logging.set_action_log_sink(self._record)
        return self

    def __exit__(self, *_: object) -> None:
        # Back to A11's own default, not to the native one: `None` through the
        # module restores the Python sink the process was configured with.
        a11_logging.set_action_log_sink(None)

    def _record(
        self,
        action_name: str,
        action_id: str,
        level: str,
        channel: str,
        file: str,
        lineno: int | None,
        internal: bool,
        mimetype: str,
        data: bytes,
        unix_seconds: float,
    ) -> None:
        self.records.append(
            {
                "action": action_name,
                "action_id": action_id,
                "level": level,
                "channel": channel,
                "file": file,
                "lineno": lineno,
                "internal": internal,
                "mimetype": mimetype,
                "data": data,
                "unix_seconds": unix_seconds,
            }
        )


async def _run(action: Action, body) -> None:
    """Run ``body(action)`` as the action's handler and wait for it."""

    async def handler(running: Action) -> None:
        await body(running)

    action.bind_handler(handler)
    action.run_in_background()
    await asyncio.wait_for(action.wait(), timeout=5)


@pytest.mark.asyncio
async def test_a_string_is_text_and_a_mapping_is_json() -> None:
    with _Sink() as sink:
        action = Action(_schema(), "kinds")

        async def body(running: Action) -> None:
            await running.log("a line")
            await running.log({"pages": 3})
            await running.log(b"\xff\xfe")

        await _run(action, body)

    assert [record["mimetype"] for record in sink.records] == [
        _native.TEXT_MIMETYPE,
        _native.JSON_MIMETYPE,
        _native.BYTES_MIMETYPE,
    ]
    assert sink.records[0]["data"] == b"a line"


@pytest.mark.asyncio
async def test_the_location_is_the_callers() -> None:
    with _Sink() as sink:
        action = Action(_schema(), "located")

        async def body(running: Action) -> None:
            await running.log("here")

        await _run(action, body)

    assert sink.records[0]["file"] == __file__
    assert sink.records[0]["lineno"] > 0
    # And an explicit location wins, which is what lets a helper report the
    # handler's line rather than its own.
    with _Sink() as sink:
        action = Action(_schema(), "told")

        async def body(running: Action) -> None:
            await running.log("there", file="other.py", lineno=42)

        await _run(action, body)

    assert sink.records[0]["file"] == "other.py"
    assert sink.records[0]["lineno"] == 42


@pytest.mark.asyncio
async def test_logf_interpolates_percent_style() -> None:
    with _Sink() as sink:
        action = Action(_schema(), "formatted")

        async def body(running: Action) -> None:
            await running.logf("read %d of %d pages", 3, 12)
            await running.logf("no arguments at all")

        await _run(action, body)

    assert [record["data"] for record in sink.records] == [
        b"read 3 of 12 pages",
        b"no arguments at all",
    ]


@pytest.mark.asyncio
async def test_the_level_channel_and_internal_flag_travel() -> None:
    with _Sink() as sink:
        action = Action(_schema(), "labelled")

        async def body(running: Action) -> None:
            await running.log(
                "quietly", level="debug", channel="fetch", internal=True
            )

        await _run(action, body)

    record = sink.records[0]
    assert record["level"] == "debug"
    assert record["channel"] == "fetch"
    assert record["internal"] is True
    assert record["action"] == "quiet"


@pytest.mark.asyncio
async def test_an_explicit_level_beats_one_in_the_metadata_map() -> None:
    with _Sink() as sink:
        action = Action(_schema(), "precedence")

        async def body(running: Action) -> None:
            await running.log(
                "escalated",
                level="error",
                metadata={_native.LOG_LEVEL_ATTRIBUTE: "info", "request": "42"},
            )

        await _run(action, body)

    assert sink.records[0]["level"] == "error"


@pytest.mark.asyncio
async def test_an_unknown_level_is_refused() -> None:
    with _Sink() as sink:
        action = Action(_schema(), "bad-level")
        raised: Exception | None = None

        async def body(running: Action) -> None:
            nonlocal raised
            try:
                await running.log("noisy", level="verbose")
            except StatusException as error:
                raised = error

        await _run(action, body)

    assert raised is not None
    assert "verbose" in str(raised)
    assert sink.records == []


@pytest.mark.asyncio
async def test_only_a_running_action_may_log() -> None:
    action = Action(_schema(), "before-run")
    with pytest.raises(StatusException):
        await action.log("too early")


@pytest.mark.asyncio
async def test_the_log_port_is_in_no_schema_and_cannot_be_declared() -> None:
    action = Action(_schema(), "hidden")
    assert _native.ACTION_LOG_OUTPUT not in action.get_schema().outputs
    assert [port.name for port in action.get_action_message().outputs] == [
        "out"
    ]

    with pytest.raises(StatusException):
        ActionSchema(
            name="declared",
            outputs={
                _native.ACTION_LOG_OUTPUT: ActionPortSchema(
                    name=_native.ACTION_LOG_OUTPUT, type="text/plain"
                )
            },
        ).validate()


@pytest.mark.asyncio
async def test_an_unclaimed_local_log_materialises_no_node() -> None:
    with _Sink() as sink:
        node_map = NodeMap()
        action = Action(_schema(), "no-node", node_map=node_map)

        async def body(running: Action) -> None:
            await running.log("into the sink")

        await _run(action, body)

    assert len(sink.records) == 1
    node_id = Action.make_node_id("no-node", _native.ACTION_LOG_OUTPUT)
    assert node_map.get_if_exists(node_id) is None


@pytest.mark.asyncio
async def test_a_claimed_log_port_carries_the_chunks_and_closes_itself() -> (
    None
):
    with _Sink() as sink:
        action = Action(_schema(), "claimed")
        logs = action.get_log_node()

        async def body(running: Action) -> None:
            await running.log("first", channel="work")
            await running.log("second", level="warning")

        await _run(action, body)

        # A claimed port owns presentation, so the sink is not also told.
        assert sink.records == []

    seen: list[types.Chunk] = []
    while True:
        chunk = await asyncio.wait_for(logs.next_chunk(), timeout=5)
        if chunk is None:
            # Closed with the action's other outputs; nobody did it by hand.
            break
        if _native.is_status_chunk(chunk):
            continue
        seen.append(chunk)

    assert [chunk.data for chunk in seen] == [b"first", b"second"]
    assert seen[0].metadata.timestamp is not None
    attributes = seen[0].metadata.attributes
    assert attributes[_native.LOG_CHANNEL_ATTRIBUTE] == b"work"
    assert attributes[_native.LOG_LEVEL_ATTRIBUTE] == b"info"
    assert (
        seen[1].metadata.attributes[_native.LOG_LEVEL_ATTRIBUTE] == b"warning"
    )


@pytest.mark.asyncio
async def test_an_unclaimed_nested_log_flows_through_its_parent() -> None:
    child_id = ""

    async def child_handler(child: Action) -> None:
        await child.log({"step": 1}, channel="progress")

    async def parent_handler(parent: Action) -> None:
        nonlocal child_id
        child = parent.make_nested(_schema("child"), propagate_io=False)
        child.bind_handler(child_handler)
        child_id = child.id
        child.run()
        await child.wait()

    with _Sink() as sink:
        parent = Action(_schema("parent"), handler=parent_handler)
        logs = parent.get_log_node()
        parent.run()
        chunk = await asyncio.wait_for(logs.next_chunk(), timeout=5)
        await asyncio.wait_for(parent.wait(), timeout=5)

    assert chunk is not None
    assert a11.from_chunk(chunk) == {"step": 1}
    assert chunk.metadata.attributes[_native.LOG_CHANNEL_ATTRIBUTE] == b"progress"
    assert (
        chunk.metadata.attributes[_native.LOG_CHILD_ACTION_ATTRIBUTE]
        == b"child"
    )
    assert (
        chunk.metadata.attributes[_native.LOG_CHILD_CALL_ID_ATTRIBUTE]
        == child_id.encode()
    )
    assert sink.records == []


@pytest.mark.asyncio
async def test_the_default_sink_reaches_the_action_logger_once(
    caplog: pytest.LogCaptureFixture,
) -> None:
    # The double-emit this is here to catch: the native default reports through
    # Abseil, which the native bridge already turns into Python records, so a
    # Python sink installed *beside* it rather than replacing it would report
    # every line twice.
    action = Action(_schema(), "logged")

    async def body(running: Action) -> None:
        await running.log("reached the logger", channel="work")

    with caplog.at_level(logging.DEBUG, logger=ACTION_LOGGER_NAME):
        await _run(action, body)

    matching = [
        record
        for record in caplog.records
        if record.getMessage() == "reached the logger"
    ]
    assert len(matching) == 1
    assert matching[0].name == ACTION_LOGGER_NAME
    assert matching[0].levelno == logging.INFO
    assert matching[0].a11_action == "quiet"
    assert matching[0].a11_channel == "work"
    assert matching[0].a11_internal is False


@pytest.mark.asyncio
async def test_a_chatty_action_nobody_drains_still_finishes() -> None:
    action = Action(_schema(), "chatty")
    action.get_log_node()  # Claimed, then never read.

    async def body(running: Action) -> None:
        for index in range(256):
            await running.logf("line %d", index)

    await _run(action, body)
    assert action.get_status().ok()


@pytest.mark.asyncio
async def test_an_object_is_logged_as_the_object_and_never_as_its_repr() -> None:
    """``log`` serialises; only ``logf`` makes a string.

    The distinction the API rests on: a handler logging a record wants the record
    on the far side, and a consumer that renders it decides how. A log path that
    stringified on the way out would have taken that decision away -- and a
    Python ``dict`` rendered by ``str`` is not even JSON, so it would not decode.
    """
    action = Action(_schema(), "not-coerced")
    logs = action.get_log_node()

    async def body(running: Action) -> None:
        await running.log({"pages": 3, "ok": True})
        await running.log([1, 2, 3])
        await running.log(b"\xff\xfe")
        await running.logf("read %d pages", 3)

    await _run(action, body)

    seen: list[types.Chunk] = []
    while True:
        chunk = await asyncio.wait_for(logs.next_chunk(), timeout=5)
        if chunk is None:
            break
        if _native.is_status_chunk(chunk):
            continue
        seen.append(chunk)

    assert [chunk.metadata.mimetype for chunk in seen] == [
        _native.JSON_MIMETYPE,
        _native.JSON_MIMETYPE,
        _native.BYTES_MIMETYPE,
        _native.TEXT_MIMETYPE,
    ]
    # Real JSON, decodable by anything: `{"pages": 3, ...}` and not `{'pages': 3}`.
    assert json.loads(seen[0].data) == {"pages": 3, "ok": True}
    assert json.loads(seen[1].data) == [1, 2, 3]
    # Bytes travel as themselves rather than as a base64 string or a repr.
    assert seen[2].data == b"\xff\xfe"
    # And only the formatted one is text.
    assert seen[3].data == b"read 3 pages"


@pytest.mark.asyncio
async def test_a_non_textual_log_is_described_rather_than_dumped() -> None:
    # What a sink shows for a payload that is not characters. The bytes are still
    # on the record for a consumer that wants them.
    with _Sink() as sink:
        action = Action(_schema(), "described")

        async def body(running: Action) -> None:
            await running.log(b"\x00\x01\x02")
            await running.log({"k": 1})

        await _run(action, body)

    assert _native.is_textual_log_mimetype(_native.JSON_MIMETYPE)
    assert _native.is_textual_log_mimetype("text/plain")
    assert not _native.is_textual_log_mimetype(_native.BYTES_MIMETYPE)
    assert sink.records[0]["data"] == b"\x00\x01\x02"


_FIXTURE = json.loads(
    (
        pathlib.Path(__file__).parents[3] / "testdata" / "log_chunk.json"
    ).read_text()
)


def test_the_reserved_port_and_its_metadata_match_the_fixture() -> None:
    # The log port and its attribute names are a cross-language contract: a peer
    # in another language reads these chunks, so the words have to be the same
    # words. Pinned beside the status chunk for the same reason.
    assert _FIXTURE["port"] == _native.ACTION_LOG_OUTPUT
    attributes = _FIXTURE["attributes"]
    assert attributes["level"] == _native.LOG_LEVEL_ATTRIBUTE
    assert attributes["internal"] == _native.LOG_INTERNAL_ATTRIBUTE
    assert attributes["channel"] == _native.LOG_CHANNEL_ATTRIBUTE
    assert attributes["file"] == _native.LOG_FILE_ATTRIBUTE
    assert attributes["lineno"] == _native.LOG_LINENO_ATTRIBUTE
    assert _FIXTURE["internal_true"] == _native.LOG_INTERNAL_TRUE
    assert _FIXTURE["internal_false"] == _native.LOG_INTERNAL_FALSE
    assert _FIXTURE["levels"] == list(_native.LOG_LEVELS)
    assert _FIXTURE["default_level"] == _native.DEFAULT_LOG_LEVEL
    # Every level maps onto a standard one, so a record can be filtered by level
    # without the sink knowing A11's names.
    assert set(_FIXTURE["levels"]) == set(a11_logging._ACTION_LEVELS)
