import asyncio
from typing import Any

import pytest

from a11 import timing
from a11.data import msgpack_utils, types
from a11.net.wire_stream import OnDone, OnMessage, WireStream
from a11.nodes.async_node import NodeMap
from a11.service.session import (
    SESSION_STATUS_HEADER,
    Session,
    SessionOptions,
    SessionWithRecv,
)
from a11.status import Status, StatusCode, StatusException


class _FakeWireStream(WireStream):
    def __init__(self, stream_id: str) -> None:
        super().__init__()
        self.stream_id = stream_id
        self.on_message: OnMessage | None = None
        self.on_done: OnDone | None = None
        self.sent: list[types.WireMessage] = []
        self.half_close_trailers: list[dict[str, bytes] | None] = []
        self.abort_statuses: list[Status] = []
        self.status = Status.ok()
        self.trailers: dict[str, Any] | None = None
        self.send_exception: Exception | None = None

    def send(self, message: types.WireMessage) -> None:
        if self.send_exception is not None:
            raise self.send_exception
        self.sent.append(message)

    async def start(self, on_message: OnMessage, on_done: OnDone) -> None:
        self.on_message = on_message
        self.on_done = on_done

    async def accept(self, on_message: OnMessage, on_done: OnDone) -> None:
        await self.start(on_message, on_done)

    def half_close(self, trailers: dict[str, bytes] | None = None) -> None:
        self.half_close_trailers.append(trailers)

    async def drain_outgoing_messages(self) -> None:
        if not self.half_close_trailers:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="The fake stream has not been half-closed.",
            ).to_exception()

    def abort(self, status: Status) -> None:
        self.abort_statuses.append(status)
        self.status = Status(
            code=StatusCode.ABORTED,
            message=status.message,
            details=status.details,
        )

    def set_deadline(self, deadline: timing.Time | None = None) -> None:
        return

    @property
    def deadline(self) -> timing.Time:
        return timing.infinite_future()

    def get_status(self) -> Status:
        return self.status

    def get_trailers(self) -> dict[str, Any] | None:
        return self.trailers

    def get_id(self) -> str:
        return self.stream_id

    def get_impl(self) -> Any:
        return None


async def _noop(*args) -> None:
    return


def _options(**kwargs) -> SessionOptions:
    return SessionOptions(
        no_stream_timeout=timing.infinite_duration(), **kwargs
    )


@pytest.mark.asyncio
async def test_session_owns_or_accepts_a_node_map_and_dispatches_fragments():
    supplied = NodeMap()
    session = Session(
        on_stream_message=_noop,
        on_stream_done=_noop,
        options=_options(),
        node_map=supplied,
    )
    fragment = types.NodeFragment(
        id="dispatched-node",
        seq=0,
        data=types.Chunk(data="payload"),
        continued=False,
    )

    assert session.node_map is supplied
    assert session.get_node_map() is supplied
    assert await session.dispatch_node_fragment(fragment) == 0

    restored = await supplied.get("dispatched-node").next_fragment()
    assert restored is not None
    assert restored == fragment

    default_session = Session(
        on_stream_message=_noop,
        on_stream_done=_noop,
        options=_options(),
    )
    assert isinstance(default_session.node_map, NodeMap)
    assert default_session.node_map is default_session.get_node_map()


@pytest.mark.asyncio
async def test_dispatch_ignores_fragments_already_failed_by_node_abort():
    session = Session(options=_options())
    node = session.node_map.get("aborted-node")
    failure = Status(code=StatusCode.CANCELLED, message="consumer cancelled")
    await node.abort_with_status(failure)
    fragment = types.NodeFragment(
        id=node.get_id(),
        seq=7,
        data=types.Chunk(data="late payload"),
        continued=False,
    )

    assert await session.dispatch_node_fragment(fragment) == 7
    with pytest.raises(StatusException) as raised:
        await node.next_fragment()
    assert raised.value.status == failure


@pytest.mark.asyncio
async def test_action_dispatch_validates_registry_and_local_action_type():
    session = Session(
        on_stream_message=_noop,
        on_stream_done=_noop,
        options=_options(),
    )
    action_message = types.ActionMessage(id="action", name="test")

    with pytest.raises(StatusException) as raised:
        await session.dispatch_action_message(action_message)
    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION

    with pytest.raises(StatusException) as raised:
        await session.dispatch_action(action_message)
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT


@pytest.mark.asyncio
async def test_wire_dispatch_tries_every_element_and_aggregates_failures():
    node_map = NodeMap()
    session = Session(
        on_stream_message=_noop,
        on_stream_done=_noop,
        options=_options(),
        node_map=node_map,
    )
    invalid = types.NodeFragment(
        id="",
        data=types.Chunk(data="invalid"),
        continued=False,
    )
    valid = types.NodeFragment(
        id="valid-node",
        seq=0,
        data=types.Chunk(data="valid"),
        continued=False,
    )
    message = types.WireMessage(
        node_fragments=[invalid, valid],
        actions=[
            types.ActionMessage(id="first-action", name="test"),
            types.ActionMessage(id="second-action", name="test"),
        ],
    )

    with pytest.raises(StatusException) as raised:
        await session.dispatch_wire_message(message)

    aggregate = raised.value.status
    assert aggregate.code == StatusCode.UNKNOWN
    assert "3 of 4" in aggregate.message
    # Action messages are dispatched before node fragments so a receiver can
    # apply its own input autofills ahead of fragments that target them.
    assert [
        (detail["element_type"], detail["element_index"])
        for detail in aggregate.details
    ] == [
        ("action_message", 0),
        ("action_message", 1),
        ("node_fragment", 0),
    ]
    assert [detail["status"]["code"] for detail in aggregate.details] == [
        StatusCode.FAILED_PRECONDITION,
        StatusCode.FAILED_PRECONDITION,
        StatusCode.INVALID_ARGUMENT,
    ]

    # The valid fragment was still dispatched after the invalid one.
    restored = await node_map.get("valid-node").next_fragment()
    assert restored is not None
    assert restored == valid


@pytest.mark.asyncio
async def test_wire_dispatch_preserves_a_common_failure_code():
    session = Session(
        on_stream_message=_noop,
        on_stream_done=_noop,
        options=_options(),
    )
    message = types.WireMessage(
        actions=[
            types.ActionMessage(id="first-action", name="test"),
            types.ActionMessage(id="second-action", name="test"),
        ]
    )

    with pytest.raises(StatusException) as raised:
        await session.dispatch_wire_message(message)

    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION
    assert len(raised.value.status.details) == 2


@pytest.mark.asyncio
async def test_default_stream_message_callback_dispatches_wire_messages():
    node_map = NodeMap()
    session = Session(
        "dispatch-session",
        options=_options(),
        node_map=node_map,
    )
    assert session.get_id() == "dispatch-session"
    stream = _FakeWireStream("dispatch-stream")
    await session.add_stream(stream)
    assert stream.on_message is not None
    message = types.WireMessage(
        node_fragments=[
            types.NodeFragment(
                id="callback-node",
                seq=0,
                data=types.Chunk(data="from-stream"),
                continued=False,
            )
        ]
    )

    await stream.on_message(message)

    restored = await asyncio.wait_for(
        node_map.get("callback-node").next_fragment(), timeout=1
    )
    assert restored is not None
    assert restored == message.node_fragments[0]


@pytest.mark.asyncio
async def test_action_messages_from_one_stream_keep_arrival_order():
    first_started = asyncio.Event()
    release_first = asyncio.Event()
    received: list[bytes] = []

    async def on_message(message, _stream, _session) -> None:
        assert message is not None
        value = message.headers["message"]
        received.append(value)
        if value == b"first":
            first_started.set()
            await release_first.wait()

    session = Session(
        on_stream_message=on_message,
        on_stream_done=_noop,
        options=_options(),
    )
    stream = _FakeWireStream("ordered-stream")
    await session.add_stream(stream)
    assert stream.on_message is not None

    await stream.on_message(
        types.WireMessage(
            actions=[types.ActionMessage(id="first", name="test")],
            headers={"message": b"first"},
        )
    )
    await stream.on_message(
        types.WireMessage(
            actions=[types.ActionMessage(id="second", name="test")],
            headers={"message": b"second"},
        )
    )
    await asyncio.wait_for(first_started.wait(), timeout=1)
    await asyncio.sleep(0.01)
    assert received == [b"first"]

    release_first.set()
    for _ in range(100):
        if received == [b"first", b"second"]:
            break
        await asyncio.sleep(0.01)
    assert received == [b"first", b"second"]

    session.half_close()
    assert stream.on_done is not None
    await stream.on_done()


@pytest.mark.asyncio
async def test_default_stream_reports_action_dispatch_failure_without_abort():
    session = Session(options=_options())
    stream = _FakeWireStream("failed-dispatch-stream")
    await session.add_stream(stream)
    assert stream.on_message is not None

    await stream.on_message(
        types.WireMessage(
            actions=[types.ActionMessage(id="action", name="test")]
        )
    )
    for _ in range(100):
        if stream.sent:
            break
        await asyncio.sleep(0.01)

    assert stream.abort_statuses == []
    assert len(stream.sent) == 1
    assert len(stream.sent[0].node_fragments) == 2


@pytest.mark.asyncio
async def test_default_stream_callback_converts_unexpected_dispatch_errors():
    session = Session(options=_options())

    async def fail_dispatch(
        _message: types.WireMessage,
        origin_stream: WireStream | None = None,
    ) -> None:
        raise ValueError("unexpected dispatch failure")

    session.dispatch_wire_message = fail_dispatch  # type: ignore[method-assign]
    stream = _FakeWireStream("unexpected-dispatch-error-stream")
    await session.add_stream(stream)
    assert stream.on_message is not None

    await stream.on_message(types.WireMessage())
    for _ in range(100):
        if stream.abort_statuses:
            break
        await asyncio.sleep(0.01)

    assert len(stream.abort_statuses) == 1
    assert stream.abort_statuses[0].code == StatusCode.UNKNOWN
    assert stream.abort_statuses[0].message == "unexpected dispatch failure"


@pytest.mark.asyncio
async def test_custom_stream_message_callback_replaces_default_dispatch():
    callback_called = asyncio.Event()

    async def on_message(*_args) -> None:
        callback_called.set()

    session = Session(
        on_stream_message=on_message,
        on_stream_done=_noop,
        options=_options(),
    )
    stream = _FakeWireStream("custom-callback-stream")
    await session.add_stream(stream)
    assert stream.on_message is not None

    await stream.on_message(
        types.WireMessage(
            actions=[types.ActionMessage(id="action", name="test")]
        )
    )
    await asyncio.wait_for(callback_called.wait(), timeout=1)

    assert stream.abort_statuses == []


@pytest.mark.asyncio
async def test_send_is_round_robin_and_close_carries_ok_session_status():
    session = Session(
        on_stream_message=_noop,
        on_stream_done=_noop,
        headers={"X-Session": b"value"},
        options=_options(),
    )
    first = _FakeWireStream("first")
    second = _FakeWireStream("second")
    await session.add_stream(first)
    await session.add_stream(second, mode="accept")
    assert not session.is_closed()

    messages = [
        types.WireMessage(headers={"sequence": str(i).encode()})
        for i in range(3)
    ]
    for message in messages:
        session.send(message)

    assert first.sent == [messages[0], messages[2]]
    assert second.sent == [messages[1]]

    session.half_close()
    assert session.get_status().is_ok()
    assert session.is_closed()
    for stream in (first, second):
        assert len(stream.half_close_trailers) == 1
        trailers = stream.half_close_trailers[0]
        assert trailers is not None
        assert trailers["x-session"] == b"value"
        assert msgpack_utils.unpack_status(
            trailers[SESSION_STATUS_HEADER]
        ).is_ok()

    assert first.on_done is not None
    assert second.on_done is not None
    await first.on_done()
    await second.on_done()
    assert session.done.is_set()


@pytest.mark.asyncio
async def test_done_waits_for_every_stream_done_callback_to_finish():
    second_started = asyncio.Event()
    release_second = asyncio.Event()
    callbacks: list[str] = []

    async def on_done(stream: WireStream, _session: Session) -> None:
        callbacks.append(stream.get_id())
        if stream.get_id() == "second":
            second_started.set()
            await release_second.wait()

    session = Session(
        on_stream_message=_noop,
        on_stream_done=on_done,
        options=_options(),
    )
    first = _FakeWireStream("first")
    second = _FakeWireStream("second")
    await session.add_stream(first)
    await session.add_stream(second)

    assert session.done is session.done
    assert not session.done.is_set()
    session.half_close()
    assert not session.done.is_set()

    assert first.on_done is not None
    await first.on_done()
    assert callbacks == ["first"]
    assert not session.done.is_set()

    assert second.on_done is not None
    finishing_second = asyncio.create_task(second.on_done())
    await asyncio.wait_for(second_started.wait(), timeout=1)
    assert not session.done.is_set()

    release_second.set()
    await finishing_second
    await asyncio.wait_for(session.done.wait(), timeout=1)
    assert callbacks == ["first", "second"]
    assert session.done.is_set()


@pytest.mark.asyncio
async def test_abort_discards_callbacks_and_sends_both_status_headers():
    callback_started = asyncio.Event()
    callback_cancelled = asyncio.Event()

    async def on_message(*args) -> None:
        callback_started.set()
        try:
            await asyncio.Event().wait()
        except asyncio.CancelledError:
            callback_cancelled.set()
            raise

    session = Session(
        on_stream_message=on_message,
        on_stream_done=_noop,
        options=_options(),
    )
    stream = _FakeWireStream("stream")
    await session.add_stream(stream)
    assert stream.on_message is not None
    await stream.on_message(types.WireMessage(headers={"message": b"one"}))
    await asyncio.wait_for(callback_started.wait(), timeout=1)

    logical_status = Status(
        code=StatusCode.DATA_LOSS, message="session data was lost"
    )
    session.abort(logical_status)
    assert session.get_status() == logical_status
    assert session.is_closed()
    await asyncio.wait_for(callback_cancelled.wait(), timeout=1)

    assert len(stream.sent) == 1
    terminal_headers = stream.sent[0].headers
    stream_status = msgpack_utils.unpack_status(
        terminal_headers["x-a11-abort-status"]
    )
    assert stream_status == Status(
        code=StatusCode.ABORTED,
        message="Session has aborted its streams",
    )
    assert (
        msgpack_utils.unpack_status(terminal_headers[SESSION_STATUS_HEADER])
        == logical_status
    )

    stream.status = stream_status
    assert stream.on_done is not None
    await stream.on_done()
    assert session.done.is_set()


@pytest.mark.asyncio
async def test_limits_apply_until_callbacks_finish_and_half_close_drains():
    gates = [asyncio.Event(), asyncio.Event()]
    received: list[types.WireMessage | None] = []

    async def on_message(message, *args) -> None:
        received.append(message)
        if message is not None:
            index = sum(item is not None for item in received) - 1
            await gates[index].wait()

    session = Session(
        on_stream_message=on_message,
        on_stream_done=_noop,
        options=_options(
            max_buffered_messages_total=1,
            max_buffered_messages_per_stream=1,
        ),
    )
    first = _FakeWireStream("first")
    second = _FakeWireStream("second")
    await session.add_stream(first)
    await session.add_stream(second)
    assert first.on_message is not None
    assert second.on_message is not None

    message = types.WireMessage(headers={"message": b"value"})
    await first.on_message(message)
    await asyncio.sleep(0.01)
    await asyncio.sleep(0.01)

    blocked = asyncio.create_task(second.on_message(message))
    remote_half_close = asyncio.create_task(first.on_message(None))
    await asyncio.sleep(0.01)
    assert not blocked.done()
    assert not remote_half_close.done()

    gates[0].set()
    await asyncio.wait_for(remote_half_close, timeout=1)
    await asyncio.wait_for(blocked, timeout=1)
    await asyncio.sleep(0.01)
    await asyncio.sleep(0.01)
    gates[1].set()

    assert first.on_done is not None
    assert second.on_done is not None
    await first.on_done()
    await second.on_done()
    session.half_close()


@pytest.mark.asyncio
async def test_explicit_remote_session_half_close_delivers_none_after_drain():
    message_finished = asyncio.Event()
    received: list[types.WireMessage | None] = []
    closed_during_callbacks: list[bool] = []

    async def on_message(message, _stream, session) -> None:
        received.append(message)
        if message is not None:
            await message_finished.wait()
        else:
            closed_during_callbacks.append(session.is_closed())

    session = Session(
        on_stream_message=on_message,
        on_stream_done=_noop,
        options=_options(),
    )
    stream = _FakeWireStream("stream")
    await session.add_stream(stream)
    assert stream.on_message is not None

    message = types.WireMessage(headers={"message": b"value"})
    await stream.on_message(message)
    await asyncio.sleep(0.01)
    await asyncio.sleep(0.01)

    stream.trailers = {
        SESSION_STATUS_HEADER.upper(): msgpack_utils.pack_status(Status.ok())
    }
    half_close = asyncio.create_task(stream.on_message(None))
    await asyncio.sleep(0.01)
    assert not half_close.done()
    assert received == [message]

    message_finished.set()
    await asyncio.wait_for(half_close, timeout=1)
    assert received == [message, None]
    assert closed_during_callbacks == [True]

    assert stream.on_done is not None
    await stream.on_done()
    assert session.done.is_set()
    session.half_close()


@pytest.mark.asyncio
async def test_plain_stream_half_close_delivers_none_without_closing_session():
    received: list[types.WireMessage | None] = []
    closed_during_callbacks: list[bool] = []

    async def on_message(message, _stream, session) -> None:
        received.append(message)
        closed_during_callbacks.append(session.is_closed())

    session = Session(
        on_stream_message=on_message,
        on_stream_done=_noop,
        options=_options(),
    )
    stream = _FakeWireStream("stream")
    await session.add_stream(stream)
    assert stream.on_message is not None

    stream.trailers = {"ordinary-trailer": b"value"}
    await stream.on_message(None)
    assert received == [None]
    assert closed_during_callbacks == [False]

    assert stream.on_done is not None
    await stream.on_done()
    session.half_close()


@pytest.mark.asyncio
async def test_session_half_close_notifies_every_attached_stream_as_closed():
    callbacks: list[tuple[str, bool]] = []

    async def on_message(message, stream, session) -> None:
        assert message is None
        callbacks.append((stream.get_id(), session.is_closed()))

    session = Session(
        on_stream_message=on_message,
        on_stream_done=_noop,
        options=_options(),
    )
    streams = [_FakeWireStream(f"stream-{index}") for index in range(8)]
    for stream in streams:
        await session.add_stream(stream)
        stream.trailers = {
            SESSION_STATUS_HEADER: msgpack_utils.pack_status(Status.ok())
        }

    await asyncio.gather(
        *(stream.on_message(None) for stream in streams if stream.on_message)
    )

    assert {stream_id for stream_id, _ in callbacks} == {
        stream.get_id() for stream in streams
    }
    assert len(callbacks) == 8
    assert all(closed for _, closed in callbacks)
    assert session.is_closed()

    session.half_close()
    for stream in streams:
        assert stream.on_done is not None
        await stream.on_done()


@pytest.mark.asyncio
async def test_remote_session_abort_sets_status_before_every_done_callback():
    callbacks: list[tuple[str, Status, bool]] = []

    async def on_done(stream, session) -> None:
        callbacks.append(
            (stream.get_id(), session.get_status(), session.is_closed())
        )

    session = Session(
        on_stream_message=_noop,
        on_stream_done=on_done,
        options=_options(),
    )
    streams = [_FakeWireStream(f"stream-{index}") for index in range(8)]
    session_abort = Status(
        code=StatusCode.ABORTED,
        message="Session has aborted its streams",
    )
    for stream in streams:
        await session.add_stream(stream)
        stream.status = session_abort

    for stream in streams:
        assert stream.on_done is not None
        await stream.on_done()

    assert {stream_id for stream_id, _, _ in callbacks} == {
        stream.get_id() for stream in streams
    }
    assert len(callbacks) == 8
    assert all(status == session_abort for _, status, _ in callbacks)
    assert all(closed for _, _, closed in callbacks)
    assert session.get_status() == session_abort
    assert session.is_closed()
    assert session.done.is_set()


@pytest.mark.asyncio
async def test_done_sees_stream_and_message_errors_abort_only_stream():
    done_saw_stream = False

    async def failing_message(*args) -> None:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="bad incoming message",
        ).to_exception()

    async def on_done(stream: WireStream, session: Session) -> None:
        nonlocal done_saw_stream
        done_saw_stream = session.get_stream(stream.get_id()) is stream

    session = Session(
        on_stream_message=failing_message,
        on_stream_done=on_done,
        options=_options(),
    )
    stream = _FakeWireStream("stream")
    await session.add_stream(stream)
    assert stream.on_message is not None
    await stream.on_message(types.WireMessage(headers={"message": b"bad"}))
    for _ in range(100):
        if stream.abort_statuses:
            break
        await asyncio.sleep(0.01)

    assert stream.abort_statuses == [
        Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="bad incoming message",
        )
    ]
    assert stream.on_done is not None
    await stream.on_done()
    assert done_saw_stream
    with pytest.raises(StatusException) as exc_info:
        session.get_stream("stream")
    assert exc_info.value.status.code == StatusCode.NOT_FOUND
    session.half_close()


@pytest.mark.asyncio
async def test_timeouts_and_ordinary_errors_use_status_exceptions():
    session = Session(
        on_stream_message=_noop,
        on_stream_done=_noop,
        options=SessionOptions(
            no_stream_timeout=timing.Duration.milliseconds(1)
        ),
    )
    await asyncio.sleep(0.01)
    with pytest.raises(StatusException) as exc_info:
        session.add_stream(_FakeWireStream("late"))
    assert exc_info.value.status.code == StatusCode.FAILED_PRECONDITION

    session = Session(
        on_stream_message=_noop,
        on_stream_done=_noop,
        options=_options(),
    )
    stream = _FakeWireStream("stream")
    await session.add_stream(stream)
    stream.send_exception = ValueError("send failed")
    with pytest.raises(StatusException) as exc_info:
        session.send(types.WireMessage())
    assert exc_info.value.status.code == StatusCode.UNKNOWN

    stream.send_exception = None
    session.set_deadline(timing.now() - timing.Duration.milliseconds(1))
    assert len(stream.sent) == 1
    assert (
        msgpack_utils.unpack_status(
            stream.sent[0].headers[SESSION_STATUS_HEADER]
        ).code
        == StatusCode.DEADLINE_EXCEEDED
    )
    stream.status = Status(code=StatusCode.ABORTED, message="deadline exceeded")
    assert stream.on_done is not None
    await stream.on_done()


@pytest.mark.asyncio
async def test_cancelling_startup_before_first_step_detaches_stream():
    session = Session(
        on_stream_message=_noop,
        on_stream_done=_noop,
        options=_options(),
    )
    startup = session.add_stream(_FakeWireStream("cancelled"))
    startup.cancel()
    with pytest.raises(asyncio.CancelledError):
        await startup
    await asyncio.sleep(0.01)

    assert list(session.streams()) == []
    session.half_close()
    assert session.done.is_set()


@pytest.mark.asyncio
async def test_session_with_recv_returns_messages_and_stream_ids():
    session = SessionWithRecv(options=_options())
    stream = _FakeWireStream("stream")
    await session.add_stream(stream)
    assert stream.on_message is not None

    first = types.WireMessage(headers={"message": b"first"})
    await stream.on_message(first)
    assert await session.receive_with_stream_id() == (first, "stream")

    second = types.WireMessage(headers={"message": b"second"})
    await stream.on_message(second)
    assert await session.receive() == second

    session.half_close()
    assert stream.on_done is not None
    await stream.on_done()


@pytest.mark.asyncio
async def test_session_with_recv_returns_remote_half_close_exactly_once():
    session = SessionWithRecv(options=_options())
    streams = [_FakeWireStream("first"), _FakeWireStream("second")]
    for stream in streams:
        await session.add_stream(stream)
        stream.trailers = {
            SESSION_STATUS_HEADER: msgpack_utils.pack_status(Status.ok())
        }

    receivers = [asyncio.create_task(session.receive()) for _ in range(3)]
    await asyncio.sleep(0.01)
    await asyncio.gather(
        *(stream.on_message(None) for stream in streams if stream.on_message)
    )
    results = await asyncio.gather(*receivers, return_exceptions=True)

    assert sum(result is None for result in results) == 1
    errors = [
        result for result in results if isinstance(result, StatusException)
    ]
    assert len(errors) == 2
    assert all(
        error.status.code == StatusCode.FAILED_PRECONDITION for error in errors
    )
    with pytest.raises(StatusException) as exc_info:
        await session.receive_with_stream_id()
    assert exc_info.value.status.code == StatusCode.FAILED_PRECONDITION

    session.half_close()
    for stream in streams:
        assert stream.on_done is not None
        await stream.on_done()


@pytest.mark.asyncio
async def test_session_with_recv_drains_messages_before_remote_half_close():
    session = SessionWithRecv(options=_options())
    streams = [_FakeWireStream("first"), _FakeWireStream("second")]
    for stream in streams:
        await session.add_stream(stream)
        stream.trailers = {
            SESSION_STATUS_HEADER: msgpack_utils.pack_status(Status.ok())
        }

    receivers = [asyncio.create_task(session.receive()) for _ in range(4)]
    await asyncio.sleep(0.01)
    messages = [
        types.WireMessage(headers={"message": b"first"}),
        types.WireMessage(headers={"message": b"second"}),
    ]
    await asyncio.gather(
        *(
            stream.on_message(message)
            for stream, message in zip(streams, messages, strict=True)
            if stream.on_message
        )
    )
    await asyncio.gather(
        *(stream.on_message(None) for stream in streams if stream.on_message)
    )
    results = await asyncio.wait_for(
        asyncio.gather(*receivers, return_exceptions=True), timeout=1
    )

    received_messages = [
        result for result in results if isinstance(result, types.WireMessage)
    ]
    assert len(received_messages) == 2
    assert {message.headers["message"] for message in received_messages} == {
        b"first",
        b"second",
    }
    assert sum(result is None for result in results) == 1
    errors = [
        result for result in results if isinstance(result, StatusException)
    ]
    assert len(errors) == 1
    assert errors[0].status.code == StatusCode.FAILED_PRECONDITION

    session.half_close()
    for stream in streams:
        assert stream.on_done is not None
        await stream.on_done()


@pytest.mark.asyncio
async def test_session_with_recv_abort_fails_all_pending_receivers():
    session = SessionWithRecv(options=_options())
    stream = _FakeWireStream("stream")
    await session.add_stream(stream)

    receivers = [
        asyncio.create_task(session.receive()),
        asyncio.create_task(session.receive_with_stream_id()),
        asyncio.create_task(session.receive()),
    ]
    await asyncio.sleep(0.01)
    abort_status = Status(
        code=StatusCode.DATA_LOSS,
        message="the Session failed",
        details=[{"source": "test"}],
    )
    session.abort(abort_status)
    results = await asyncio.gather(*receivers, return_exceptions=True)

    assert all(isinstance(result, StatusException) for result in results)
    assert all(result.status == abort_status for result in results)
    with pytest.raises(StatusException) as exc_info:
        await session.receive()
    assert exc_info.value.status == abort_status

    stream.status = Status(
        code=StatusCode.ABORTED,
        message="Session has aborted its streams",
    )
    assert stream.on_done is not None
    await stream.on_done()


@pytest.mark.asyncio
async def test_session_with_recv_remote_abort_fails_pending_receivers():
    session = SessionWithRecv(options=_options())
    stream = _FakeWireStream("stream")
    await session.add_stream(stream)

    receivers = [asyncio.create_task(session.receive()) for _ in range(2)]
    await asyncio.sleep(0.01)
    remote_abort = Status(
        code=StatusCode.ABORTED,
        message="Session has aborted its streams",
    )
    stream.status = remote_abort
    assert stream.on_done is not None
    await stream.on_done()
    results = await asyncio.gather(*receivers, return_exceptions=True)

    assert all(isinstance(result, StatusException) for result in results)
    assert all(result.status == remote_abort for result in results)
    assert session.get_status() == remote_abort


@pytest.mark.asyncio
async def test_session_with_recv_deadline_does_not_change_session_state():
    session = SessionWithRecv(options=_options())
    stream = _FakeWireStream("stream")
    await session.add_stream(stream)

    deadline = timing.now() + timing.Duration.milliseconds(5)
    with pytest.raises(StatusException) as exc_info:
        await session.receive(deadline=deadline)
    assert exc_info.value.status.code == StatusCode.DEADLINE_EXCEEDED
    assert session.get_status().is_ok()
    assert not session.is_closed()
    assert stream.abort_statuses == []

    cancelled = asyncio.create_task(session.receive())
    await asyncio.sleep(0.01)
    cancelled.cancel()
    with pytest.raises(asyncio.CancelledError):
        await cancelled
    assert session.get_status().is_ok()
    assert not session.is_closed()

    message = types.WireMessage(headers={"after": b"deadline"})
    assert stream.on_message is not None
    await stream.on_message(message)
    assert await session.receive() == message

    session.half_close()
    assert stream.on_done is not None
    await stream.on_done()
