import asyncio
import contextlib
import os
import uuid

import pytest
import pytest_asyncio

from a11 import timing
from a11.data.types import Chunk, ChunkMetadata, NodeFragment
from a11.redis.client import RedisClient, RedisClientOptions
from a11.status import Status, StatusCode, StatusException
from a11.stores.redis_chunk_store import (
    RedisChunkStore,
    RedisChunkStoreOptions,
)


@pytest_asyncio.fixture
async def redis_client():
    configured_url = os.environ.get("A11_TEST_REDIS_URL")
    options = (
        RedisClientOptions.from_url(configured_url)
        if configured_url
        else RedisClientOptions()
    )
    options.connect_timeout = timing.Duration.milliseconds(250)
    options.command_timeout = timing.Duration.seconds(2)
    client = RedisClient(options)
    try:
        await client.ready()
    except StatusException as error:
        client.close()
        if configured_url:
            raise
        pytest.skip(f"Redis is unavailable: {error}")
    try:
        yield client
    finally:
        client.close()


def _fragment(
    value: str | bytes,
    seq: int | None,
    *,
    final: bool = False,
    metadata: ChunkMetadata | None = None,
) -> NodeFragment:
    return NodeFragment(
        data=Chunk(data=value, metadata=metadata),
        seq=seq,
        continued=not final,
    )


@contextlib.asynccontextmanager
async def _store(redis_client, *, inline_threshold: int = 256 * 1024):
    token = uuid.uuid4().hex
    options = {
        "key_prefix": f"a11:test:python-store:{token}:",
        "inline_data_threshold": inline_threshold,
    }
    store = RedisChunkStore(f"redis-{token}", redis_client, options)
    try:
        yield store
    finally:
        await redis_client.command(["DEL", *store.keys.script_keys()])


@pytest.mark.asyncio
async def test_ordering_implicit_sequences_and_atomic_batches(redis_client):
    async with _store(redis_client) as store:
        with pytest.raises(StatusException) as raised:
            await store.put_many(
                [_fragment("first", 0), _fragment("duplicate", 0)]
            )
        assert raised.value.status.code is StatusCode.INVALID_ARGUMENT
        assert await store.size() == 0

        assert await store.put(_fragment("four", 4, final=True)) == 4
        assert await store.put(_fragment("one", None)) == 1
        assert await store.put_many(
            [_fragment("zero", 0), _fragment("two", 2)]
        ) == [0, 2]

        assert [
            (await store.get(seq)).get_chunk().data for seq in range(3)
        ] == [b"zero", b"one", b"two"]
        assert [
            await store.get_seq_for_arrival_order(order) for order in range(4)
        ] == [4, 1, 0, 2]
        assert await store.get_final_seq() == 4


@pytest.mark.asyncio
async def test_waits_without_racing_writes_or_close(redis_client):
    async with _store(redis_client) as store:
        waiting_for_one = asyncio.ensure_future(
            store.get(1, deadline=timing.now() + timing.Duration.seconds(2))
        )
        waiting_for_arrival = asyncio.ensure_future(
            store.get_by_arrival_order(
                1, deadline=timing.now() + timing.Duration.seconds(2)
            )
        )
        await asyncio.sleep(0)
        await store.put(_fragment("zero", 0))
        assert not waiting_for_one.done()
        await store.put(_fragment("one", 1, final=True))
        assert (await waiting_for_one).seq == 1
        assert (await waiting_for_arrival).seq == 1

        missing = asyncio.ensure_future(
            store.get(7, deadline=timing.now() + timing.Duration.seconds(2))
        )
        terminal = Status(
            code=StatusCode.ABORTED,
            message="producer failed",
            details=[{"source": "redis-test"}],
        )
        returned = await store.close_writes_with_status(terminal)
        assert returned == terminal
        with pytest.raises(StatusException) as raised:
            await missing
        assert raised.value.status == terminal
        assert raised.value.status.details == [{"source": "redis-test"}]


@pytest.mark.asyncio
async def test_next_is_global_spmc_and_emits_final_sentinel(redis_client):
    async with _store(redis_client) as store:
        consumers = [
            asyncio.ensure_future(
                store.next(
                    deadline=timing.now() + timing.Duration.seconds(2),
                    limit=2,
                )
            )
            for _ in range(2)
        ]
        await asyncio.sleep(0)
        await store.put_many(
            [
                _fragment("two", 2, final=True),
                _fragment("zero", 0),
                _fragment("one", 1),
            ]
        )
        batches = await asyncio.gather(*consumers)
        sequences = sorted(
            item.seq for batch in batches for item in batch if item is not None
        )
        assert sequences == [0, 1, 2]
        assert any(item is None for batch in batches for item in batch)
        assert await store.next(
            deadline=timing.now() + timing.Duration.seconds(2)
        ) == [None]


@pytest.mark.asyncio
async def test_next_matches_partial_deadline_and_error_close_semantics(
    redis_client,
):
    async with _store(redis_client) as store:
        await store.put(_fragment("zero", 0))
        partial = await store.next(
            deadline=timing.now() + timing.Duration.milliseconds(20),
            limit=2,
        )
        assert [fragment.seq for fragment in partial] == [0]

        waiting = asyncio.create_task(
            store.next(
                deadline=timing.now() + timing.Duration.seconds(2), limit=2
            )
        )
        await asyncio.sleep(0)
        await store.put(_fragment("one", 1))
        terminal = Status(code=StatusCode.ABORTED, message="producer failed")
        await store.close_writes_with_status(terminal)
        assert [fragment.seq for fragment in await waiting] == [1]

        with pytest.raises(StatusException) as raised:
            await store.next(
                deadline=timing.now() + timing.Duration.seconds(2), limit=2
            )
        assert raised.value.status == terminal


@pytest.mark.asyncio
async def test_large_payload_is_separate_and_clear_preserves_metadata(
    redis_client,
):
    async with _store(redis_client, inline_threshold=16) as store:
        metadata = ChunkMetadata(
            mimetype="application/octet-stream",
            attributes={"source": b"test"},
        )
        await store.put(
            _fragment(b"x" * 1024, 0, final=True, metadata=metadata)
        )
        assert (
            await redis_client.command(["HLEN", store.keys.blobs])
        ).as_integer() == 1

        original = await store.clear_data(0)
        assert original.get_chunk().data == b"x" * 1024
        tombstone = (await store.get(0)).get_chunk()
        assert tombstone.data == b""
        assert tombstone.ref == "__tombstone__"
        assert tombstone.metadata == metadata
        assert (
            await redis_client.command(["HLEN", store.keys.blobs])
        ).as_integer() == 0

        state = await store.get_metadata()
        assert state.id == store.get_id()
        assert state.size == 1
        assert state.total_chunks_put == 1
        assert state.final_seq == 0


@pytest.mark.asyncio
async def test_elapsed_deadline_raises_public_status(redis_client):
    async with _store(redis_client) as store:
        with pytest.raises(StatusException) as raised:
            await store.get(
                0,
                deadline=timing.now() - timing.Duration.milliseconds(1),
            )
        assert raised.value.status.code is StatusCode.DEADLINE_EXCEEDED


@pytest.mark.asyncio
async def test_invalid_arguments_raise_public_status(redis_client):
    async with _store(redis_client) as store:
        operations = (
            lambda: store.get(-1),
            lambda: store.get_by_arrival_order(-1),
            lambda: store.next(limit=0),
            lambda: store.put(object()),
            lambda: store.put_many([object()]),
            lambda: store.put_many(42),
            lambda: store.clear_data(1 << 32),
            lambda: store.close_writes_with_status(object()),
        )
        for operation in operations:
            with pytest.raises(StatusException) as raised:
                await operation()
            assert raised.value.status.code is StatusCode.INVALID_ARGUMENT
        assert await store.size() == 0


@pytest.mark.asyncio
async def test_cancelled_get_does_not_consume_future_data(redis_client):
    async with _store(redis_client) as store:
        waiting = asyncio.create_task(store.get(0))
        await asyncio.sleep(0)
        waiting.cancel()
        with pytest.raises(asyncio.CancelledError):
            await waiting

        await store.put(_fragment("available", 0, final=True))
        assert (await store.get(0)).get_chunk().data == b"available"


def test_store_options_use_native_options_protocol():
    options = RedisChunkStoreOptions.model_validate(
        {"key_prefix": "custom:", "inline_data_threshold": 17}
    )
    assert options.model_dump() == {
        "key_prefix": "custom:",
        "inline_data_threshold": 17,
    }
    assert (
        options.model_json_schema()["properties"]["key_prefix"]["type"]
        == "string"
    )
