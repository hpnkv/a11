import asyncio

import pytest

import a11
from a11 import timing
from a11.data import types
from a11.net.in_process_wire_stream import InProcessWireStream
from a11.net.wire_stream import WireStream, WireStreamWithRecv
from a11.status import Status, StatusCode, StatusException


def _message(message_id: str) -> types.WireMessage:
    return types.WireMessage(
        actions=[types.ActionMessage(id=message_id, name="test")]
    )


async def _wait_for_in_process_shutdown(*streams: WireStreamWithRecv) -> None:
    await asyncio.gather(*(stream.wrapped_stream.wait() for stream in streams))


@pytest.mark.asyncio
async def test_adapts_start_accept_receive_and_remote_half_close():
    first_raw, second_raw = InProcessWireStream.create_pair()
    first = WireStreamWithRecv(first_raw)
    second = WireStreamWithRecv(second_raw)

    assert a11.WireStreamWithRecv is WireStreamWithRecv
    assert first.get_id() == second.get_id()
    assert first.get_impl() is first_raw.get_impl()

    await first.start()
    await second.accept()

    message = _message("one")
    first.send(message)
    assert await second.receive(timing.Duration.seconds(1)) == message

    receivers = [asyncio.ensure_future(second.receive()) for _ in range(2)]
    first.half_close({"first-trailer": b"first"})
    results = await asyncio.wait_for(
        asyncio.gather(*receivers, return_exceptions=True), timeout=1
    )

    assert sum(result is None for result in results) == 1
    errors = [
        result for result in results if isinstance(result, StatusException)
    ]
    assert len(errors) == 1
    assert errors[0].status.code == StatusCode.FAILED_PRECONDITION
    assert second.get_trailers() == {"first-trailer": b"first"}

    second.half_close({"second-trailer": b"second"})
    assert await first.receive(timing.Duration.seconds(1)) is None
    assert first.get_trailers() == {"second-trailer": b"second"}
    await asyncio.wait_for(first.drain_outgoing_messages(), timeout=1)
    await asyncio.wait_for(second.drain_outgoing_messages(), timeout=1)
    await _wait_for_in_process_shutdown(first, second)
    assert first.get_status().is_ok()
    assert second.get_status().is_ok()


@pytest.mark.asyncio
async def test_abort_takes_priority_over_buffered_data_for_every_receiver():
    first_raw, second_raw = InProcessWireStream.create_pair()
    first = WireStreamWithRecv(first_raw)
    second = WireStreamWithRecv(second_raw)
    await first.start()
    await second.accept()

    first.send(_message("discarded"))
    abort_status = Status(
        code=StatusCode.DATA_LOSS,
        message="remote data loss",
        details=[{"source": "test"}],
    )
    first.abort(abort_status)

    for _ in range(100):
        if not second.get_status().is_ok():
            break
        await asyncio.sleep(0)
    assert second.get_status() == abort_status

    results = await asyncio.gather(
        second.receive(), second.receive(), return_exceptions=True
    )
    assert all(isinstance(result, StatusException) for result in results)
    assert all(result.status == abort_status for result in results)

    with pytest.raises(StatusException) as raised:
        await first.receive()
    assert raised.value.status.code == StatusCode.ABORTED
    await _wait_for_in_process_shutdown(first, second)


@pytest.mark.asyncio
@pytest.mark.parametrize("_iteration", range(20))
async def test_receive_timeout_and_cancellation_do_not_change_stream_state(
    _iteration,
):
    first_raw, second_raw = InProcessWireStream.create_pair()
    first = WireStreamWithRecv(first_raw)
    second = WireStreamWithRecv(second_raw)
    await first.start()
    await second.accept()

    with pytest.raises(StatusException) as raised:
        await second.receive(timing.Duration.milliseconds(1))
    assert raised.value.status.code == StatusCode.DEADLINE_EXCEEDED
    assert first.get_status().is_ok()
    assert second.get_status().is_ok()

    cancelled = asyncio.ensure_future(second.receive())
    await asyncio.sleep(0)
    cancelled.cancel()
    with pytest.raises(asyncio.CancelledError):
        await cancelled
    assert second.get_status().is_ok()

    message = _message("after-timeout")
    first.send(message)
    assert await second.receive(timing.Duration.seconds(1)) == message

    first.half_close()
    assert await second.receive(timing.Duration.seconds(1)) is None
    second.half_close()
    assert await first.receive(timing.Duration.seconds(1)) is None
    await asyncio.gather(
        first.drain_outgoing_messages(), second.drain_outgoing_messages()
    )
    await _wait_for_in_process_shutdown(first, second)


@pytest.mark.asyncio
async def test_receive_validates_timeout_and_wraps_implementation_errors():
    raw, _ = InProcessWireStream.create_pair()
    stream = WireStreamWithRecv(raw)

    with pytest.raises(StatusException) as raised:
        await stream.receive("invalid")  # type: ignore[arg-type]
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT

    with pytest.raises(StatusException) as raised:
        await stream.receive(timing.Duration(-1))
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT

    class FailingStartStream(WireStream):
        def get_id(self) -> str:
            return "failing-start"

        async def start(self, *_args) -> None:
            raise ValueError("start failed")

    failing = WireStreamWithRecv(FailingStartStream())
    with pytest.raises(StatusException) as raised:
        await failing.start()
    assert raised.value.status.code == StatusCode.UNKNOWN
    assert raised.value.status.message == "start failed"


def test_constructor_requires_a_wire_stream():
    with pytest.raises(StatusException) as raised:
        WireStreamWithRecv(object())  # type: ignore[arg-type]
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT
