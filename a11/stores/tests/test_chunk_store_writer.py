import asyncio

import pytest

from a11.actions import is_close_status_chunk, status_from_chunk
from a11.data import types
from a11.net.wire_stream import WireStream
from a11.status import Status, StatusCode, StatusException
from a11.stores.chunk_store_writer import (
    ChunkStoreWriter,
    ChunkStoreWriterOptions,
)
from a11.stores.local_chunk_store import LocalChunkStore


def _chunk(value: int) -> types.Chunk:
    return types.Chunk(data=str(value))


class _BlockingPutStore(LocalChunkStore):
    def __init__(self):
        super().__init__("blocking")
        self.put_started = asyncio.Event()
        self.release_put = asyncio.Event()

    async def put_many(self, fragments):
        self.put_started.set()
        await self.release_put.wait()
        return await super().put_many(fragments)


class _RecordingStore(LocalChunkStore):
    def __init__(self):
        super().__init__("recording")
        self.batch_sizes = []

    async def put_many(self, fragments):
        self.batch_sizes.append(len(fragments))
        return await super().put_many(fragments)


class _StatusFailStore(LocalChunkStore):
    async def put_many(self, fragments):
        raise Status(
            code=StatusCode.UNAVAILABLE, message="injected store failure"
        ).to_exception()


class _RuntimeFailStore(LocalChunkStore):
    async def put_many(self, fragments):
        raise RuntimeError("injected runtime failure")


class _BadSequenceStore(LocalChunkStore):
    async def put_many(self, fragments):
        await super().put_many(fragments)
        return []


class _CloseFailStore(LocalChunkStore):
    def __init__(self, node_id: str, failure: Status):
        super().__init__(node_id)
        self.failure = failure
        self.close_statuses: list[Status] = []

    async def close_writes_with_status(
        self,
        status: Status,
        return_status_if_already_closed: bool = False,
    ) -> Status:
        self.close_statuses.append(status)
        if len(self.close_statuses) == 1:
            raise self.failure.to_exception()
        return await super().close_writes_with_status(
            status, return_status_if_already_closed
        )


class _NonzeroImplicitSequenceStore(LocalChunkStore):
    async def put_many(self, fragments):
        fragments = [fragment.model_copy(deep=True) for fragment in fragments]
        for index, fragment in enumerate(fragments, start=10):
            if fragment.seq is None:
                fragment.seq = index
        return await super().put_many(fragments)


class _FailingStream(WireStream):
    def __init__(self):
        super().__init__()

    def get_id(self):
        return "failing-stream"

    def send(self, message):
        raise RuntimeError("injected tee failure")


class _RecordingStream(WireStream):
    def __init__(self):
        super().__init__()
        self.fragments: list[types.NodeFragment] = []

    def get_id(self):
        return "recording-stream"

    def send(self, message):
        self.fragments.extend(message.node_fragments)


@pytest.mark.asyncio
async def test_many_writers_cancel_through_native_futures():
    writers = [
        ChunkStoreWriter(LocalChunkStore(f"thin-writer-{index}"))
        for index in range(200)
    ]

    cancellations = [writer.cancel() for writer in writers]
    assert all(
        isinstance(cancellation, asyncio.Future)
        and not isinstance(cancellation, asyncio.Task)
        for cancellation in cancellations
    )
    await asyncio.gather(*cancellations)
    assert all(
        writer.get_status().code == StatusCode.ABORTED for writer in writers
    )


@pytest.mark.asyncio
async def test_blocked_writer_does_not_hold_up_another_shared_writer():
    blocked_store = _BlockingPutStore()
    blocked = ChunkStoreWriter(blocked_store)
    blocked_result = await blocked.put_chunk(_chunk(0), final=True)
    await asyncio.wait_for(blocked_store.put_started.wait(), timeout=1)

    ready_store = LocalChunkStore("shared-writer-ready")
    ready = ChunkStoreWriter(ready_store)
    ready_result = await ready.put_chunk(_chunk(0), final=True)

    assert await asyncio.wait_for(ready_result, timeout=1) == 0
    await ready.drain_and_close()

    await blocked.abort_with_status(
        Status(code=StatusCode.ABORTED, message="test cleanup")
    )
    with pytest.raises(StatusException):
        await blocked_result


@pytest.mark.asyncio
async def test_put_returns_future_before_background_write_finishes():
    store = _BlockingPutStore()
    writer = ChunkStoreWriter(store)

    result = await writer.put(_chunk(0), final=True)
    assert isinstance(result, asyncio.Future)
    assert not result.done()

    await asyncio.wait_for(store.put_started.wait(), timeout=1)
    assert not result.done()
    store.release_put.set()

    assert await result == 0
    await writer.drain_and_close()
    assert writer.get_status().is_ok()


@pytest.mark.asyncio
async def test_writer_batches_available_chunks_and_resolves_each_future():
    store = _RecordingStore()
    writer = ChunkStoreWriter(
        store, ChunkStoreWriterOptions(max_chunks_to_write_at_once=3)
    )

    results = [
        await writer.put_chunk(_chunk(index), final=index == 2)
        for index in range(3)
    ]

    assert await asyncio.gather(*results) == [0, 1, 2]
    assert store.batch_sizes == [3]
    await writer.drain_and_close()


@pytest.mark.asyncio
async def test_writer_offset_assigns_sequences_starting_at_offset():
    store = LocalChunkStore("offset")
    writer = ChunkStoreWriter(store, ChunkStoreWriterOptions(offset=7))

    first = await writer.put_chunk(_chunk(7))
    second = await writer.put_chunk(_chunk(8), final=True)

    assert await asyncio.gather(first, second) == [7, 8]
    await writer.drain_and_close()
    assert (await store.get(7)).get_chunk().data == b"7"
    assert (await store.get(8)).get_chunk().data == b"8"


@pytest.mark.asyncio
async def test_explicit_sequence_is_returned_from_store():
    store = LocalChunkStore("explicit")
    writer = ChunkStoreWriter(store)

    result = await writer.put_chunk(_chunk(4), seq=4, final=True)

    assert await result == 4
    await writer.drain_and_close()


@pytest.mark.asyncio
async def test_sticky_mimetype_preserves_gap_anchors_and_other_metadata():
    store = LocalChunkStore("sticky-writer")
    writer = ChunkStoreWriter(
        store, ChunkStoreWriterOptions(sticky_mimetype=True)
    )

    def chunk(value: bytes, *, with_attribute: bool = False):
        return types.Chunk(
            metadata=types.ChunkMetadata(
                mimetype="text/plain",
                attributes={"role": b"assistant"} if with_attribute else {},
            ),
            data=value,
        )

    confirmations = [
        await writer.put_chunk(chunk(b"first")),
        await writer.put_chunk(chunk(b"gap-anchor"), seq=3),
        await writer.put_chunk(chunk(b"details", with_attribute=True), seq=4),
        await writer.put_chunk(chunk(b"stripped"), seq=5),
        await writer.put_chunk(chunk(b"second-gap-anchor"), seq=7, final=True),
    ]
    assert await asyncio.gather(*confirmations) == [0, 3, 4, 5, 7]
    await writer.drain_and_close()

    chunks = [(await store.get(seq)).get_chunk() for seq in [0, 3, 4, 5, 7]]
    assert chunks[0].get_mimetype() == "text/plain"
    assert chunks[1].get_mimetype() == "text/plain"
    assert chunks[2].metadata is not None
    assert chunks[2].metadata.mimetype == ""
    assert chunks[2].metadata.attributes == {"role": b"assistant"}
    assert chunks[3].metadata is None
    assert chunks[4].get_mimetype() == "text/plain"


@pytest.mark.asyncio
async def test_implicit_sequence_returned_by_store_is_preserved():
    store = _NonzeroImplicitSequenceStore("implicit")
    writer = ChunkStoreWriter(store)

    result = await writer.put_chunk(_chunk(10), final=True)

    assert await result == 10
    await writer.drain_and_close()


@pytest.mark.asyncio
async def test_mixed_implicit_and_explicit_writes_use_consistent_batches():
    store = _RecordingStore()
    writer = ChunkStoreWriter(store)

    implicit = await writer.put_chunk(_chunk(0))
    explicit = await writer.put_chunk(_chunk(5), seq=5, final=True)

    assert await asyncio.gather(implicit, explicit) == [0, 5]
    assert store.batch_sizes == [1, 1]
    await writer.drain_and_close()


@pytest.mark.asyncio
async def test_bounded_buffer_counts_the_in_flight_write():
    store = _BlockingPutStore()
    writer = ChunkStoreWriter(
        store, ChunkStoreWriterOptions(num_chunks_to_buffer=1)
    )
    first = await writer.put_chunk(_chunk(0))
    await asyncio.wait_for(store.put_started.wait(), timeout=1)

    second_put = asyncio.create_task(writer.put_chunk(_chunk(1), final=True))
    await asyncio.sleep(0)
    assert not second_put.done()

    store.release_put.set()
    assert await first == 0
    second = await asyncio.wait_for(second_put, timeout=1)
    assert await second == 1
    await writer.drain_and_close()


@pytest.mark.asyncio
async def test_drain_and_close_waits_for_in_flight_write():
    store = _BlockingPutStore()
    writer = ChunkStoreWriter(store)
    result = await writer.put_chunk(_chunk(0), final=True)
    await asyncio.wait_for(store.put_started.wait(), timeout=1)

    closing = asyncio.ensure_future(writer.drain_and_close())
    await asyncio.sleep(0)
    assert not closing.done()
    assert store._status is None

    store.release_put.set()
    await asyncio.wait_for(closing, timeout=1)
    assert await result == 0
    assert store._status.is_ok()


@pytest.mark.asyncio
async def test_failed_graceful_close_can_be_retried_as_abort():
    close_failure = Status(
        code=StatusCode.INTERNAL, message="graceful close failed"
    )
    abort_status = Status(code=StatusCode.DATA_LOSS, message="producer failed")
    store = _CloseFailStore("close-retry", close_failure)
    writer = ChunkStoreWriter(store)

    with pytest.raises(StatusException) as raised:
        await writer.drain_and_close()
    assert raised.value.status == close_failure

    await writer.abort_with_status(abort_status)
    assert store.close_statuses == [Status.ok(), abort_status]
    assert store._status == abort_status
    assert writer.get_status() == abort_status


@pytest.mark.asyncio
async def test_store_status_fails_batch_futures_and_writer():
    writer = ChunkStoreWriter(_StatusFailStore("status-failure"))
    first = await writer.put_chunk(_chunk(0))
    second = await writer.put_chunk(_chunk(1), final=True)

    outcomes = await asyncio.gather(first, second, return_exceptions=True)
    assert all(isinstance(outcome, StatusException) for outcome in outcomes)
    assert [outcome.status.code for outcome in outcomes] == [
        StatusCode.UNAVAILABLE,
        StatusCode.UNAVAILABLE,
    ]
    assert writer.get_status().code == StatusCode.UNAVAILABLE

    with pytest.raises(StatusException) as raised:
        await writer.drain_and_close()
    assert raised.value.status.code == StatusCode.UNAVAILABLE


@pytest.mark.asyncio
async def test_unexpected_store_exception_is_converted_for_future_and_status():
    writer = ChunkStoreWriter(_RuntimeFailStore("runtime-failure"))
    result = await writer.put_chunk(_chunk(0), final=True)

    with pytest.raises(StatusException) as raised:
        await result
    assert raised.value.status.code == StatusCode.UNKNOWN
    assert "injected runtime failure" in raised.value.status.message
    assert writer.get_status().code == StatusCode.UNKNOWN


@pytest.mark.asyncio
async def test_invalid_store_sequence_response_fails_with_data_loss():
    store = _BadSequenceStore("bad-sequences")
    writer = ChunkStoreWriter(store)
    result = await writer.put_chunk(_chunk(0), final=True)

    with pytest.raises(StatusException) as raised:
        await result
    assert raised.value.status.code == StatusCode.DATA_LOSS
    assert writer.get_status().code == StatusCode.DATA_LOSS
    await store.close_writes_with_status(Status.ok())


@pytest.mark.asyncio
async def test_drain_tees_a_closure_marker_after_the_data():
    store = LocalChunkStore("close-tee")
    writer = ChunkStoreWriter(store)
    stream = _RecordingStream()
    writer.attach_stream(stream)  # type: ignore[arg-type]
    # Deliberately no final fragment: the marker is all a peer gets.
    assert await (await writer.put_chunk(_chunk(0))) == 0
    await writer.drain_and_close()

    assert len(stream.fragments) == 2
    assert not is_close_status_chunk(stream.fragments[0].data)
    marker = stream.fragments[1]
    assert marker.id == "close-tee"
    assert marker.continued is False
    assert is_close_status_chunk(marker.data)
    assert status_from_chunk(marker.data).is_ok()


@pytest.mark.asyncio
async def test_closure_marker_failure_still_closes_the_store():
    store = LocalChunkStore("close-tee-failure")
    writer = ChunkStoreWriter(store)
    writer.attach_stream(_FailingStream())  # type: ignore[arg-type]

    with pytest.raises(StatusException) as raised:
        await writer.drain_and_close()
    assert raised.value.status.code == StatusCode.UNKNOWN
    assert writer.get_status().code == StatusCode.UNKNOWN
    assert not writer.is_writable()
    # The store closed regardless, so a second close reports the first status.
    assert (
        await store.close_writes_with_status(
            Status.ok(), return_status_if_already_closed=True
        )
    ).is_ok()


@pytest.mark.asyncio
async def test_tee_failure_does_not_change_confirmed_store_future():
    store = LocalChunkStore("tee")
    writer = ChunkStoreWriter(store)
    writer.attach_stream(_FailingStream())  # type: ignore[arg-type]
    result = await writer.put_chunk(_chunk(0), final=True)

    assert await result == 0
    with pytest.raises(StatusException) as raised:
        await writer.wait_for_buffer_to_drain()
    assert raised.value.status.code == StatusCode.UNKNOWN
    await store.close_writes_with_status(Status.ok())


@pytest.mark.asyncio
async def test_abort_fails_unconfirmed_future_with_supplied_status():
    store = _BlockingPutStore()
    writer = ChunkStoreWriter(store)
    result = await writer.put_chunk(_chunk(0), final=True)
    await asyncio.wait_for(store.put_started.wait(), timeout=1)
    abort_status = Status(
        code=StatusCode.DATA_LOSS, message="source data was corrupt"
    )

    await writer.abort_with_status(abort_status)

    with pytest.raises(StatusException) as raised:
        await result
    assert raised.value.status.code == StatusCode.DATA_LOSS
    assert writer.get_status().code == StatusCode.DATA_LOSS
    assert store._status.code == StatusCode.DATA_LOSS


@pytest.mark.asyncio
async def test_cancel_fails_unconfirmed_future_with_aborted_status():
    store = _BlockingPutStore()
    writer = ChunkStoreWriter(store)
    result = await writer.put_chunk(_chunk(0), final=True)
    await asyncio.wait_for(store.put_started.wait(), timeout=1)

    cancellation = writer.cancel()

    with pytest.raises(StatusException) as raised:
        await asyncio.wait_for(result, timeout=1)
    assert raised.value.status.code == StatusCode.ABORTED
    await cancellation
    assert writer.get_status().code == StatusCode.ABORTED
    await store.close_writes_with_status(Status.ok())


@pytest.mark.asyncio
async def test_explicit_out_of_order_write_can_follow_final_arrival():
    store = LocalChunkStore("out-of-order-final")
    writer = ChunkStoreWriter(store)
    final_result = await writer.put_chunk(_chunk(2), seq=2, final=True)
    first_result = await writer.put_chunk(_chunk(0), seq=0)
    second_result = await writer.put_chunk(_chunk(1), seq=1)

    assert await asyncio.gather(final_result, first_result, second_result) == [
        2,
        0,
        1,
    ]
    await writer.drain_and_close()


@pytest.mark.asyncio
async def test_invalid_inputs_raise_status_exceptions():
    store = LocalChunkStore("invalid")
    writer = ChunkStoreWriter(store)

    with pytest.raises(StatusException) as unsupported:
        await writer.put(object())
    assert unsupported.value.status.code == StatusCode.UNIMPLEMENTED

    with pytest.raises(StatusException) as bad_seq:
        await writer.put_chunk(_chunk(0), seq=-1)
    assert bad_seq.value.status.code == StatusCode.INVALID_ARGUMENT

    await writer.abort_with_status(
        Status(code=StatusCode.ABORTED, message="test cleanup")
    )
