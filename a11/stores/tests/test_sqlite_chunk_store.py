"""SQLite-backend behaviour beyond the shared ChunkStore contract.

The contract itself is exercised for this backend by
`test_chunk_store.py`, which is parametrized over both implementations. What
follows is only what is specific to SQLite: durability, the on-disk layout,
payload externalization, node-reference traversal, and the options surface.
"""

import asyncio

import pytest

from a11 import timing
from a11.data import types
from a11.status import Status, StatusCode, StatusException
from a11.stores.sqlite_chunk_store import (
    SQLiteChunkStore,
    SQLiteChunkStoreFactory,
    SQLiteChunkStoreOptions,
    SQLiteSynchronous,
)


def _deadline() -> timing.Time:
    return timing.now() + timing.Duration.seconds(5)


def _chunk(seq: int, payload: bytes, *, final: bool = False):
    return types.NodeFragment(
        data=types.Chunk(data=payload), seq=seq, continued=not final
    )


@pytest.mark.asyncio
async def test_fragments_survive_reopening_the_root(tmp_path):
    root = str(tmp_path)
    store = SQLiteChunkStoreFactory(root).open("durable")
    await store.put_many([_chunk(0, b"one"), _chunk(1, b"two", final=True)])

    # A brand-new factory and store object over the same directory: nothing is
    # carried over in memory, so anything visible here came off disk.
    reopened = SQLiteChunkStoreFactory(root).open("durable")
    assert await reopened.size() == 2
    assert await reopened.get_final_seq() == 1
    assert (
        await reopened.get(1, deadline=_deadline())
    ).get_chunk().data == b"two"


@pytest.mark.asyncio
async def test_uses_the_documented_on_disk_layout(tmp_path):
    root = tmp_path
    store = SQLiteChunkStore("layout", str(root), {"inline_data_threshold": 16})
    await store.put(_chunk(0, b"x" * 1024, final=True))

    assert (root / "store.sqlite").is_file()
    blobs = sorted((root / "blobs").iterdir())
    assert len(blobs) == 1
    # Blob filenames are UUIDs, as in ./blobs/939f2184-db19-4dd0-b949-...
    name = blobs[0].name
    assert len(name) == 36 and name.count("-") == 4
    assert blobs[0].read_bytes() == b"x" * 1024


@pytest.mark.asyncio
async def test_inline_threshold_is_exclusive_and_round_trips(tmp_path):
    threshold = 64
    store = SQLiteChunkStore(
        "threshold", str(tmp_path), {"inline_data_threshold": threshold}
    )
    at = b"a" * threshold
    over = b"b" * (threshold + 1)
    await store.put_many([_chunk(0, at), _chunk(1, over, final=True)])

    # Only the payload strictly larger than the threshold leaves the row.
    assert len(list((tmp_path / "blobs").iterdir())) == 1
    assert (await store.get(0, deadline=_deadline())).get_chunk().data == at
    assert (await store.get(1, deadline=_deadline())).get_chunk().data == over


@pytest.mark.asyncio
async def test_clear_data_drops_the_blob_but_keeps_the_slot(tmp_path):
    store = SQLiteChunkStore(
        "clear", str(tmp_path), {"inline_data_threshold": 8}
    )
    payload = b"z" * 512
    await store.put(_chunk(0, payload, final=True))
    assert len(list((tmp_path / "blobs").iterdir())) == 1

    cleared = await store.clear_data(0)
    assert cleared.get_chunk().data == payload
    assert len(list((tmp_path / "blobs").iterdir())) == 0
    # The slot survives so sequence numbering is undisturbed.
    assert await store.size() == 1
    assert (await store.get(0, deadline=_deadline())).get_chunk().data == b""


@pytest.mark.asyncio
async def test_stores_node_references_and_traverses_them(tmp_path):
    factory = SQLiteChunkStoreFactory(str(tmp_path))
    target = factory.open("target")
    referrer = factory.open("referrer")

    # Unlike the other backends, a NodeRef is a first-class payload here.
    await referrer.put(
        types.NodeFragment(
            data=types.NodeRef(id="target", offset=8, length=64),
            seq=0,
            continued=False,
        )
    )

    stored = await referrer.get(0, deadline=_deadline())
    node_ref = stored.get_node_ref()
    assert (node_ref.id, node_ref.offset, node_ref.length) == ("target", 8, 64)

    referrers = await target.find_referrers()
    assert [fragment.id for fragment in referrers] == ["referrer"]

    # A tombstone is chunk-shaped, so clearing a reference is refused rather
    # than quietly changing the payload's type.
    with pytest.raises(StatusException) as raised:
        await referrer.clear_data(0)
    assert raised.value.status.code == StatusCode.UNIMPLEMENTED


@pytest.mark.asyncio
async def test_metadata_reports_cursors_and_ownership(tmp_path):
    factory = SQLiteChunkStoreFactory(str(tmp_path), {"owner_id": "helena"})
    store = factory.open("described")
    await store.put_many([_chunk(0, b"one"), _chunk(1, b"two", final=True)])

    metadata = await store.get_metadata()
    assert metadata.id == "described"
    assert metadata.owner_id == "helena"
    assert metadata.closed is False
    assert metadata.status is None
    assert metadata.size == 2
    assert metadata.total_chunks_put == 2
    assert metadata.final_seq == 1
    assert metadata.max_seq == 1
    assert metadata.revision > 0
    assert metadata.data_bytes == 6

    await store.close_writes_with_status(Status(code=StatusCode.OK))
    closed = await store.get_metadata()
    assert closed.closed is True
    assert closed.status is not None
    assert closed.status.code == StatusCode.OK


@pytest.mark.asyncio
async def test_stores_under_one_root_share_a_database(tmp_path):
    first = SQLiteChunkStoreFactory(str(tmp_path))
    second = SQLiteChunkStoreFactory(str(tmp_path))
    assert first.root == second.root

    await first.open("shared").put(_chunk(0, b"written", final=True))
    # A store from the other factory sees the committed write immediately.
    assert await second.open("shared").size() == 1


@pytest.mark.asyncio
async def test_a_committing_writer_wakes_a_parked_reader(tmp_path):
    store = SQLiteChunkStoreFactory(str(tmp_path)).open("waiting")

    pending = asyncio.ensure_future(store.get(0, deadline=_deadline()))
    await asyncio.sleep(0.01)
    assert not pending.done()

    await store.put(_chunk(0, b"arrived", final=True))
    assert (await pending).get_chunk().data == b"arrived"


@pytest.mark.asyncio
async def test_sweeping_reclaims_only_unreferenced_blobs(tmp_path):
    factory = SQLiteChunkStoreFactory(
        str(tmp_path),
        {
            "inline_data_threshold": 8,
            # No grace, so the sweep treats everything as old enough.
            "blob_grace_period": timing.zero_duration(),
        },
    )
    store = factory.open("sweep")
    await store.put(_chunk(0, b"q" * 256, final=True))

    orphan = tmp_path / "blobs" / "00000000-0000-4000-8000-000000000000"
    orphan.write_bytes(b"left behind by a crash")

    assert await factory.sweep_orphan_blobs() == 1
    assert not orphan.exists()
    # The referenced payload is untouched.
    assert (await store.get(0, deadline=_deadline())).get_chunk().data == (
        b"q" * 256
    )


@pytest.mark.asyncio
async def test_backs_an_async_node_and_persists_the_stream(tmp_path):
    import a11

    factory = SQLiteChunkStoreFactory(str(tmp_path))
    # The factory is callable, so it drops straight into chunk_store_factory.
    node = a11.AsyncNode.create("stream", chunk_store_factory=factory)

    async def produce():
        for index in range(3):
            await node.put(f"chunk-{index}".encode())
        await node.finalize(b"last", wait=True)

    received = []

    async def consume():
        async for chunk in node:
            received.append(bytes(chunk))

    await asyncio.gather(produce(), consume())
    assert received == [b"chunk-0", b"chunk-1", b"chunk-2", b"last"]

    store = SQLiteChunkStoreFactory(str(tmp_path)).open("stream")
    assert await store.size() == 4
    assert await store.get_final_seq() == 3


def test_default_root_follows_the_cache_convention(monkeypatch):
    monkeypatch.delenv("A11_SQLITE_CHUNK_STORE_ROOT", raising=False)
    monkeypatch.delenv("XDG_CACHE_HOME", raising=False)
    monkeypatch.setenv("HOME", "/home/example")
    assert (
        SQLiteChunkStoreFactory.default_root()
        == "/home/example/.cache/a11/chunks"
    )

    monkeypatch.setenv("XDG_CACHE_HOME", "/cache")
    assert SQLiteChunkStoreFactory.default_root() == "/cache/a11/chunks"

    monkeypatch.setenv("A11_SQLITE_CHUNK_STORE_ROOT", "/explicit")
    assert SQLiteChunkStoreFactory.default_root() == "/explicit"


def test_options_validate_and_read_the_environment(monkeypatch):
    options = SQLiteChunkStoreOptions()
    assert options.inline_data_threshold == 128 * 1024
    assert options.synchronous == SQLiteSynchronous.NORMAL

    monkeypatch.setenv(
        "A11_SQLITE_CHUNK_STORE_INLINE_DATA_THRESHOLD_BYTES", "4096"
    )
    monkeypatch.setenv("A11_SQLITE_CHUNK_STORE_SYNCHRONOUS", "full")
    monkeypatch.setenv("A11_SQLITE_CHUNK_STORE_OWNER_ID", "someone")
    from_env = SQLiteChunkStoreOptions.from_environment()
    assert from_env.inline_data_threshold == 4096
    assert from_env.synchronous == SQLiteSynchronous.FULL
    assert from_env.owner_id == "someone"

    monkeypatch.setenv("A11_SQLITE_CHUNK_STORE_SYNCHRONOUS", "sometimes")
    with pytest.raises(StatusException) as raised:
        SQLiteChunkStoreOptions.from_environment()
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT


def test_rejects_invalid_arguments(tmp_path):
    with pytest.raises(StatusException):
        SQLiteChunkStore("", str(tmp_path))
    with pytest.raises(StatusException):
        SQLiteChunkStoreFactory("")
    with pytest.raises(StatusException):
        SQLiteChunkStore(123, str(tmp_path))  # type: ignore[arg-type]
