import asyncio
import functools

import pytest

from a11.data import types
from a11.net.in_process_wire_stream import InProcessWireStream
from a11.net.wire_stream import WireStreamOptions
from a11.status import Status, StatusCode, StatusException


def _message(message_id: str) -> types.WireMessage:
    return types.WireMessage(
        actions=[types.ActionMessage(id=message_id, name="test")]
    )


async def _noop(*args) -> None:
    return


async def _set_event(event: asyncio.Event) -> None:
    event.set()


@pytest.mark.asyncio
async def test_pair_exchanges_messages_half_closes_and_trailers():
    first, second = InProcessWireStream.create_pair()
    first_messages: list[types.WireMessage | None] = []
    second_messages: list[types.WireMessage | None] = []
    first_done = asyncio.Event()
    second_done = asyncio.Event()

    async def on_first(message):
        first_messages.append(message)

    async def on_second(message):
        second_messages.append(message)
        if message is None:
            second.half_close({"Second-Trailer": b"second"})

    await first.start(on_first, functools.partial(_set_event, first_done))
    await second.accept(on_second, functools.partial(_set_event, second_done))

    first.send(_message("one"))
    first.send(_message("two"))
    first.half_close({"First-Trailer": b"first"})
    await asyncio.wait_for(first.drain_outgoing_messages(), timeout=1)

    await asyncio.wait_for(first_done.wait(), timeout=1)
    await asyncio.wait_for(second_done.wait(), timeout=1)

    assert first.get_id() == second.get_id()
    assert [
        None if message is None else message.actions[0].id
        for message in second_messages
    ] == ["one", "two", None]
    assert first_messages == [None]
    assert first.get_trailers() == {"second-trailer": b"second"}
    assert second.get_trailers() == {"first-trailer": b"first"}
    assert first.get_status().is_ok()
    assert second.get_status().is_ok()


@pytest.mark.asyncio
async def test_drain_waits_for_half_close_to_enter_peer_transport_buffer():
    options = WireStreamOptions(max_buffered_incoming_messages=1)
    first, second = InProcessWireStream.create_pair(options)
    second_messages: list[types.WireMessage | None] = []
    second_half_closed = asyncio.Event()

    async def on_second(message):
        second_messages.append(message)
        if message is None:
            second_half_closed.set()
            second.half_close()

    await first.start(_noop, _noop)
    first.send(_message("fills-buffer"))
    first.half_close()

    drain = asyncio.ensure_future(first.drain_outgoing_messages())
    await asyncio.sleep(0)
    await asyncio.sleep(0)
    assert not drain.done()

    await second.accept(on_second, _noop)
    await asyncio.wait_for(drain, timeout=1)
    await asyncio.wait_for(second_half_closed.wait(), timeout=1)
    assert [
        None if message is None else message.actions[0].id
        for message in second_messages
    ] == ["fills-buffer", None]

    await asyncio.wait_for(
        asyncio.gather(first.wait(), second.wait()), timeout=1
    )


@pytest.mark.asyncio
async def test_drain_and_context_manager_require_explicit_half_close():
    first, _ = InProcessWireStream.create_pair()

    with pytest.raises(StatusException) as raised:
        await first.drain_outgoing_messages()
    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION

    with pytest.raises(StatusException) as raised:
        async with first:
            pass
    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION
    assert first.get_status().is_ok()


@pytest.mark.asyncio
async def test_context_manager_drains_but_does_not_half_close():
    first, second = InProcessWireStream.create_pair()
    second_received_half_close = asyncio.Event()

    async def on_second(message):
        if message is None:
            second_received_half_close.set()
            second.half_close()

    await first.start(_noop, _noop)
    await second.accept(on_second, _noop)

    async with first:
        first.send(_message("inside-context"))
        first.half_close()

    await asyncio.wait_for(second_received_half_close.wait(), timeout=1)
    await asyncio.wait_for(
        asyncio.gather(first.wait(), second.wait()), timeout=1
    )


@pytest.mark.asyncio
async def test_abort_is_communicated_but_does_not_satisfy_drain():
    first, second = InProcessWireStream.create_pair()
    first_done = asyncio.Event()
    second_done = asyncio.Event()
    abort_status = Status(
        code=StatusCode.INVALID_ARGUMENT,
        message="invalid in-process request",
        details=[{"source": "test"}],
    )

    first.abort(abort_status)
    await first.start(_noop, functools.partial(_set_event, first_done))
    await second.accept(_noop, functools.partial(_set_event, second_done))
    await asyncio.wait_for(first_done.wait(), timeout=1)
    await asyncio.wait_for(second_done.wait(), timeout=1)

    assert first.get_status().code == StatusCode.ABORTED
    assert second.get_status() == abort_status
    with pytest.raises(StatusException) as raised:
        await first.drain_outgoing_messages()
    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION
