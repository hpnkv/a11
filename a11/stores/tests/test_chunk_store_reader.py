import asyncio
from collections import defaultdict

import pytest

from a11 import timing
from a11.data import types
from a11.status import Status, StatusCode, StatusException
from a11.stores.chunk_store_reader import (
    ChunkStoreReader,
    ChunkStoreReaderOptions,
)
from a11.stores.local_chunk_store import LocalChunkStore


def _fragment(seq: int, *, final: bool = False) -> types.NodeFragment:
    return types.NodeFragment(
        data=types.Chunk(data=str(seq)),
        seq=seq,
        continued=not final,
    )


class _ObservedGetStore(LocalChunkStore):
    def __init__(self):
        super().__init__("observed")
        self.get_started = asyncio.Event()
        self.release_get = asyncio.Event()
        self.get_calls = 0

    async def get(self, seq, deadline=timing.infinite_future()):
        self.get_calls += 1
        self.get_started.set()
        await self.release_get.wait()
        return await super().get(seq, deadline)


class _RuntimeErrorStore(LocalChunkStore):
    async def get(self, seq, deadline=timing.infinite_future()):
        raise RuntimeError("injected read failure")


class _GatedGetStore(LocalChunkStore):
    """A store whose per-sequence reads block until individually released."""

    def __init__(self, node_id):
        super().__init__(node_id)
        self.started = defaultdict(asyncio.Event)
        self.gates = defaultdict(asyncio.Event)

    async def get(self, seq, deadline=timing.infinite_future()):
        self.started[seq].set()
        await self.gates[seq].wait()
        return await super().get(seq, deadline)


@pytest.mark.asyncio
async def test_many_readers_complete_through_native_futures():
    readers = [
        ChunkStoreReader(
            LocalChunkStore(f"thin-reader-{index}"),
            ChunkStoreReaderOptions(max_chunks_to_read=0),
        )
        for index in range(200)
    ]

    completions = [reader.wait() for reader in readers]
    assert all(
        isinstance(completion, asyncio.Future)
        and not isinstance(completion, asyncio.Task)
        for completion in completions
    )
    await asyncio.gather(*completions)
    assert all(reader.get_status().is_ok() for reader in readers)


@pytest.mark.asyncio
async def test_blocked_reader_does_not_hold_up_another_shared_reader():
    blocked_store = _ObservedGetStore()
    blocked = ChunkStoreReader(
        blocked_store, ChunkStoreReaderOptions(num_chunks_to_buffer=1)
    )
    await asyncio.wait_for(blocked_store.get_started.wait(), timeout=1)

    ready_store = LocalChunkStore("shared-reader-ready")
    await ready_store.put(_fragment(0, final=True))
    ready = ChunkStoreReader(
        ready_store, ChunkStoreReaderOptions(num_chunks_to_buffer=1)
    )

    assert (await asyncio.wait_for(ready.next(), timeout=1)).seq == 0
    assert await ready.next() is None

    blocked.cancel()
    await blocked.wait()


@pytest.mark.asyncio
async def test_reader_starts_store_read_in_background_before_next():
    store = _ObservedGetStore()
    await store.put(_fragment(0, final=True))

    reader = ChunkStoreReader(
        store, ChunkStoreReaderOptions(num_chunks_to_buffer=1)
    )
    await asyncio.wait_for(store.get_started.wait(), timeout=1)

    store.release_get.set()
    fragment = await reader.next()
    assert fragment is not None
    assert fragment.seq == 0
    assert await reader.next() is None
    await reader.wait()


@pytest.mark.asyncio
async def test_zero_buffer_reads_only_when_a_caller_is_waiting():
    store = _ObservedGetStore()
    await store.put(_fragment(0, final=True))
    reader = ChunkStoreReader(
        store, ChunkStoreReaderOptions(num_chunks_to_buffer=0)
    )
    await asyncio.sleep(0)
    assert store.get_calls == 0

    waiting = reader.next()
    await asyncio.wait_for(store.get_started.wait(), timeout=1)
    store.release_get.set()

    assert (await waiting).seq == 0
    assert await reader.next() is None
    await reader.wait()


@pytest.mark.asyncio
async def test_ordered_reader_honours_offset_and_exact_maximum():
    store = LocalChunkStore("ordered")
    await store.put_many([_fragment(2, final=True), _fragment(0), _fragment(1)])
    reader = ChunkStoreReader(
        store,
        ChunkStoreReaderOptions(offset=1, max_chunks_to_read=2),
    )

    assert [(await reader.next()).seq for _ in range(2)] == [1, 2]
    assert await reader.next() is None
    await reader.wait()


@pytest.mark.asyncio
async def test_arrival_order_reader_does_not_stop_at_early_final_arrival():
    store = LocalChunkStore("arrival")
    await store.put_many([_fragment(2, final=True), _fragment(0), _fragment(1)])
    await store.close_writes_with_status(Status.ok())
    reader = ChunkStoreReader(
        store, ChunkStoreReaderOptions(ordered=False, num_chunks_to_buffer=4)
    )

    await reader.wait()
    assert [(await reader.next()).seq for _ in range(3)] == [2, 0, 1]
    assert await reader.next() is None


@pytest.mark.asyncio
async def test_prefetched_fragment_is_delivered_before_a_later_fetch():
    # A fetch completing while an earlier fragment already sits in the prefetch
    # buffer must not hand the later fragment to a waiting caller ahead of the
    # buffered one. The caller's wake-up is coalesced away while a fetch is in
    # flight, so ordered reads have to stay serial regardless.
    store = _GatedGetStore("serial-order")
    await store.put_many([_fragment(0), _fragment(1), _fragment(2, final=True)])
    reader = ChunkStoreReader(
        store, ChunkStoreReaderOptions(num_chunks_to_buffer=8)
    )

    # Prefetch seq 0 and let it land in the buffer.
    await asyncio.wait_for(store.started[0].wait(), timeout=1)
    store.gates[0].set()
    while reader.buffer_size < 1:
        await asyncio.sleep(0)

    # With seq 0 buffered and the seq 1 fetch already in flight, a fresh read
    # request cannot be matched yet - its Wake() is coalesced by the active
    # fetch. It must still receive the earliest fragment.
    await asyncio.wait_for(store.started[1].wait(), timeout=1)
    pending = reader.next()
    store.gates[1].set()
    store.gates[2].set()

    assert (await asyncio.wait_for(pending, timeout=1)).seq == 0
    assert [(await reader.next()).seq for _ in range(2)] == [1, 2]
    assert await reader.next() is None
    await reader.wait()


@pytest.mark.asyncio
async def test_pending_next_futures_each_receive_a_distinct_fragment():
    store = LocalChunkStore("pending")
    reader = ChunkStoreReader(
        store, ChunkStoreReaderOptions(num_chunks_to_buffer=0)
    )
    pending = [reader.next() for _ in range(3)]

    await store.put_many([_fragment(0), _fragment(1), _fragment(2, final=True)])

    assert [fragment.seq for fragment in await asyncio.gather(*pending)] == [
        0,
        1,
        2,
    ]
    assert await reader.next() is None
    await reader.wait()


@pytest.mark.asyncio
async def test_next_timeout_does_not_cancel_background_store_read():
    store = LocalChunkStore("timeout")
    reader = ChunkStoreReader(
        store, ChunkStoreReaderOptions(num_chunks_to_buffer=0)
    )

    with pytest.raises(StatusException) as raised:
        await reader.next(timing.Duration.milliseconds(10))
    assert raised.value.status.code == StatusCode.DEADLINE_EXCEEDED

    await store.put(_fragment(0, final=True))
    fragment = await asyncio.wait_for(reader.next(), timeout=1)
    assert fragment.seq == 0
    assert await reader.next() is None
    await reader.wait()


@pytest.mark.asyncio
async def test_cancel_distributes_buffer_then_raises_aborted():
    store = LocalChunkStore("cancel")
    await store.put_many([_fragment(0), _fragment(1)])
    reader = ChunkStoreReader(
        store, ChunkStoreReaderOptions(num_chunks_to_buffer=2)
    )
    while reader.buffer_size < 2:
        await asyncio.sleep(0)

    reader.cancel()

    assert [(await reader.next()).seq for _ in range(2)] == [0, 1]
    with pytest.raises(StatusException) as raised:
        await reader.next()
    assert raised.value.status.code == StatusCode.ABORTED
    await reader.wait()


@pytest.mark.asyncio
async def test_store_error_is_raised_after_buffered_fragments():
    store = LocalChunkStore("failed")
    await store.put(_fragment(0))
    await store.close_writes_with_status(
        Status(code=StatusCode.UNAVAILABLE, message="producer unavailable")
    )
    reader = ChunkStoreReader(
        store, ChunkStoreReaderOptions(num_chunks_to_buffer=2)
    )
    await reader.wait()

    assert (await reader.next()).seq == 0
    with pytest.raises(StatusException) as raised:
        await reader.next()
    assert raised.value.status.code == StatusCode.UNAVAILABLE


@pytest.mark.asyncio
async def test_exact_maximum_reports_a_final_fragment_that_is_too_early():
    store = LocalChunkStore("short")
    await store.put(_fragment(0, final=True))
    reader = ChunkStoreReader(
        store, ChunkStoreReaderOptions(max_chunks_to_read=2)
    )
    await reader.wait()

    assert (await reader.next()).seq == 0
    with pytest.raises(StatusException) as raised:
        await reader.next()
    assert raised.value.status.code == StatusCode.OUT_OF_RANGE


@pytest.mark.asyncio
async def test_zero_maximum_finishes_without_calling_store():
    store = _ObservedGetStore()
    reader = ChunkStoreReader(
        store, ChunkStoreReaderOptions(max_chunks_to_read=0)
    )

    await reader.wait()
    assert store.get_calls == 0
    assert await reader.next() is None


@pytest.mark.asyncio
async def test_pop_returns_original_chunk_and_leaves_tombstone():
    store = LocalChunkStore("pop")
    await store.put(_fragment(0, final=True))
    reader = ChunkStoreReader(store, ChunkStoreReaderOptions(pop_chunks=True))

    fragment = await reader.next()
    stored = await store.get(0)
    assert fragment.get_chunk().data == b"0"
    assert stored.get_chunk().ref == "__tombstone__"
    assert await reader.next() is None
    await reader.wait()


@pytest.mark.asyncio
async def test_sticky_mimetype_expands_ordered_chunk_metadata():
    store = LocalChunkStore("sticky-reader")
    await store.put_many([
        types.NodeFragment(
            data=types.Chunk(
                metadata=types.ChunkMetadata(mimetype="text/plain"),
                data=b"anchor",
            ),
            seq=0,
            continued=True,
        ),
        types.NodeFragment(
            data=types.Chunk(
                metadata=types.ChunkMetadata(
                    mimetype="", attributes={"role": b"assistant"}
                ),
                data=b"attributes",
            ),
            seq=1,
            continued=True,
        ),
        types.NodeFragment(
            data=types.Chunk(data=b"missing"),
            seq=2,
            continued=False,
        ),
    ])
    reader = ChunkStoreReader(
        store, ChunkStoreReaderOptions(sticky_mimetype=True)
    )

    chunks = [(await reader.next()).get_chunk() for _ in range(3)]
    assert [chunk.get_mimetype() for chunk in chunks] == [
        "text/plain",
        "text/plain",
        "text/plain",
    ]
    assert chunks[1].metadata.attributes == {"role": b"assistant"}
    assert chunks[2].metadata is not None
    assert await reader.next() is None
    await reader.wait()


@pytest.mark.asyncio
async def test_unexpected_read_exception_is_converted_to_status():
    reader = ChunkStoreReader(_RuntimeErrorStore("runtime"))

    with pytest.raises(StatusException) as raised:
        await reader.next()
    assert raised.value.status.code == StatusCode.UNKNOWN
    assert "injected read failure" in raised.value.status.message
    await reader.wait()


def test_next_rejects_non_duration_timeout_with_status():
    async def run():
        reader = ChunkStoreReader(
            LocalChunkStore("invalid-timeout"),
            ChunkStoreReaderOptions(max_chunks_to_read=0),
        )
        with pytest.raises(StatusException) as raised:
            reader.next(1)  # type: ignore[arg-type]
        assert raised.value.status.code == StatusCode.INVALID_ARGUMENT
        await reader.wait()

    asyncio.run(run())
