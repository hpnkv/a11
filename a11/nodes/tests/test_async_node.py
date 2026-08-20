import asyncio
import threading
from dataclasses import dataclass

import pytest

from a11.data import types
from a11.data.serialization import SerializationRegistry
from a11.nodes.async_node import AsyncNode, NodeMap
from a11.status import Status, StatusCode, StatusException
from a11.stores.chunk_store_reader import ChunkStoreReaderOptions
from a11.stores.chunk_store_writer import ChunkStoreWriterOptions
from a11.stores.local_chunk_store import LocalChunkStore


async def _confirm(
    *writes: asyncio.Future[int],
) -> list[int]:
    return list(await asyncio.gather(*writes))


@pytest.mark.asyncio
async def test_accessors_are_lazy_singletons_and_options_are_configurable():
    node = AsyncNode(
        LocalChunkStore("options"),
        reader_options={"max_chunks_to_read": 0},
        writer_options=ChunkStoreWriterOptions(offset=7),
    )

    assert node.get_reader_options().max_chunks_to_read == 0
    assert node.get_writer_options().offset == 7

    node.set_reader_options(ChunkStoreReaderOptions(max_chunks_to_read=0))
    node.set_writer_options({"offset": 9})
    reader = node.reader
    writer = node.writer

    assert node.reader is reader
    assert node.writer is writer
    assert reader.options.max_chunks_to_read == 0
    assert writer.options.offset == 9

    with pytest.raises(StatusException) as raised:
        node.set_reader_options(ChunkStoreReaderOptions())
    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION

    with pytest.raises(StatusException) as raised:
        node.set_writer_options(ChunkStoreWriterOptions())
    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION

    reader.cancel()
    await reader.wait()
    await writer.cancel()


@pytest.mark.asyncio
async def test_writer_and_reader_options_offsets_apply_to_the_stream():
    node = AsyncNode(
        LocalChunkStore("offsets"),
        reader_options=ChunkStoreReaderOptions(offset=7),
        writer_options=ChunkStoreWriterOptions(offset=7),
    )

    confirmation = await node.put({"value": 7}, final=True)

    assert await confirmation == 7
    fragment = await node.next_fragment()
    assert fragment is not None
    assert fragment.seq == 7
    assert await asyncio.to_thread(
        node.serialization_registry.from_chunk,
        fragment.get_chunk(),
    ) == {"value": 7}
    assert await node.next_fragment() is None


@pytest.mark.asyncio
async def test_put_accepts_raw_chunks_and_fragments():
    chunk_node = AsyncNode(LocalChunkStore("raw-chunk"))
    chunk = types.Chunk(
        metadata=types.ChunkMetadata(mimetype="text/plain"),
        data="payload",
    )

    chunk_write = await chunk_node.put_chunk(chunk, final=True)
    assert await chunk_write == 0
    assert await chunk_node.next_chunk() == chunk
    assert await chunk_node.next_fragment() is None

    fragment_node = AsyncNode(LocalChunkStore("raw-fragment"))
    fragment = types.NodeFragment(data=chunk, continued=False)

    fragment_write = await fragment_node.put(fragment)
    assert await fragment_write == 0
    restored = await fragment_node.next_fragment()
    assert restored is not None
    assert restored.get_chunk() == chunk
    assert restored.continued is False


@pytest.mark.asyncio
async def test_async_iteration_deserializes_objects_and_preserves_none():
    node = AsyncNode(LocalChunkStore("iteration"))
    expected = [1, None, {"answer": 42}]
    writes = [
        await node.put(value, final=index == len(expected) - 1)
        for index, value in enumerate(expected)
    ]
    assert await _confirm(*writes) == [0, 1, 2]

    restored = [value async for value in node]

    assert restored == expected


@pytest.mark.asyncio
async def test_async_iteration_skips_a_null_final_marker():
    node = AsyncNode(LocalChunkStore("iteration-null"))
    value_write = await node.put(("only", "value"))
    assert await _confirm(value_write) == [0]
    await node.finalize(wait=True)

    # The language-neutral JSON ``array`` tag deliberately does not retain
    # whether Python supplied a list or tuple; default JSON decoding uses list.
    assert [value async for value in node] == [["only", "value"]]


def _json_chunk(data: bytes) -> types.Chunk:
    return types.Chunk(
        metadata=types.ChunkMetadata(mimetype="application/json"),
        data=data,
    )


@pytest.mark.asyncio
async def test_expected_types_apply_to_consumption_and_async_iteration():
    consumed = AsyncNode(LocalChunkStore("expected-consume"))
    assert consumed.set_expected_types("application/json", dict) is consumed
    assert (
        await (
            await consumed.put_chunk(_json_chunk(b'{"value": 1}'), final=True)
        )
        == 0
    )
    assert await consumed.consume() == {"value": 1}

    iterated = AsyncNode(LocalChunkStore("expected-iteration"))
    iterated.set_expected_types(["application/json"], dict)
    writes = [
        await iterated.put_chunk(_json_chunk(b'{"value": 2}')),
        await iterated.put_chunk(_json_chunk(b'{"value": 3}'), final=True),
    ]
    await _confirm(*writes)

    assert [value async for value in iterated] == [
        {"value": 2},
        {"value": 3},
    ]


@pytest.mark.asyncio
async def test_expect_types_temporarily_overrides_and_restores_defaults():
    node = AsyncNode(LocalChunkStore("expected-context"))
    node.set_expected_types("application/json", dict)
    writes = [
        await node.put_chunk(_json_chunk(b"[]")),
        await node.put_chunk(_json_chunk(b"{}"), final=True),
    ]
    await _confirm(*writes)

    with node.expect_types(("application/json",), list) as scoped:
        assert scoped is node
        assert await node.next() == []

    assert await node.next() == {}


@pytest.mark.asyncio
async def test_raw_iterators_ignore_expected_deserialization_types():
    fragment_node = AsyncNode(LocalChunkStore("raw-fragment-iteration"))
    fragment_node.set_expected_types("application/x-does-not-exist", int)
    fragment_chunks = [_json_chunk(b"[]"), _json_chunk(b"{}")]
    await _confirm(
        await fragment_node.put_chunk(fragment_chunks[0]),
        await fragment_node.put_chunk(fragment_chunks[1], final=True),
    )

    fragments = [fragment async for fragment in fragment_node.iter_fragments()]
    assert [fragment.get_chunk() for fragment in fragments] == fragment_chunks
    assert all(fragment.id == fragment_node.get_id() for fragment in fragments)

    chunk_node = AsyncNode(LocalChunkStore("raw-chunk-iteration"))
    chunk_node.set_expected_types("application/x-does-not-exist", int)
    expected_chunks = [_json_chunk(b"[1]"), _json_chunk(b"[2]")]
    await _confirm(
        await chunk_node.put_chunk(expected_chunks[0]),
        await chunk_node.put_chunk(expected_chunks[1], final=True),
    )

    assert [
        chunk async for chunk in chunk_node.iter_chunks()
    ] == expected_chunks


def test_expected_type_configuration_validates_arguments():
    node = AsyncNode(LocalChunkStore("invalid-expected-types"))

    with pytest.raises(StatusException) as raised:
        node.set_expected_types(1, dict)  # type: ignore[arg-type]
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT

    with pytest.raises(StatusException) as raised:
        node.set_expected_types(
            ["application/json", 1], dict  # type: ignore[list-item]
        )
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT

    with pytest.raises(StatusException) as raised:
        node.set_expected_types(
            "application/json", "dict"  # type: ignore[arg-type]
        )
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT


@dataclass(frozen=True)
class _ThreadedValue:
    value: int


@pytest.mark.asyncio
async def test_object_serialization_and_deserialization_run_in_worker_threads():
    event_loop_thread = threading.get_ident()
    serializer_threads: list[int] = []
    deserializer_threads: list[int] = []
    serializer_started = threading.Event()
    release_serializer = threading.Event()

    def serialize(value: _ThreadedValue) -> bytes:
        serializer_threads.append(threading.get_ident())
        serializer_started.set()
        assert release_serializer.wait(timeout=1)
        return str(value.value).encode()

    def deserialize(data: bytes, obj_type: type[_ThreadedValue]):
        deserializer_threads.append(threading.get_ident())
        return obj_type(int(data))

    registry = SerializationRegistry()
    registry.register(
        _ThreadedValue,
        "application/x-threaded-value",
        serialize,
        deserialize,
    )
    node = AsyncNode(
        LocalChunkStore("worker-thread"),
        serialization_registry=registry,
    )

    putting = asyncio.create_task(node.put(_ThreadedValue(12), final=True))
    while not serializer_started.is_set():
        await asyncio.sleep(0)

    # The serializer is blocked, but this event-loop task is still running.
    assert not putting.done()
    release_serializer.set()
    confirmation = await putting
    assert await confirmation == 0
    assert serializer_threads == [serializer_threads[0]]
    assert serializer_threads[0] != event_loop_thread

    assert await node.next() == _ThreadedValue(12)
    assert deserializer_threads == [deserializer_threads[0]]
    assert deserializer_threads[0] != event_loop_thread


@pytest.mark.asyncio
@pytest.mark.parametrize("trailing_null", [False, True])
async def test_consume_accepts_both_valid_terminal_shapes(trailing_null):
    node = AsyncNode(LocalChunkStore(f"consume-{trailing_null}"))
    value_write = await node.put(
        {"value": 1},
        final=not trailing_null,
    )
    await _confirm(value_write)
    if trailing_null:
        await node.finalize(wait=True)

    assert await node.consume() == {"value": 1}
    assert await node.next_fragment() is None


@pytest.mark.asyncio
async def test_consume_can_return_raw_chunk_or_fragment():
    chunk_node = AsyncNode(LocalChunkStore("consume-chunk"))
    chunk = types.Chunk(
        metadata=types.ChunkMetadata(mimetype="text/plain"),
        data="raw",
    )
    assert await (await chunk_node.put_chunk(chunk, final=True)) == 0
    assert await chunk_node.consume(types.Chunk) == chunk

    fragment_node = AsyncNode(LocalChunkStore("consume-fragment"))
    assert await (await fragment_node.put_chunk(chunk, final=True)) == 0
    fragment = await fragment_node.consume(types.NodeFragment)
    assert isinstance(fragment, types.NodeFragment)
    assert fragment.get_chunk() == chunk


@pytest.mark.asyncio
@pytest.mark.parametrize("closed_with_null", [False, True])
async def test_consume_treats_a_node_holding_no_value_as_none(closed_with_null):
    """A null chunk marks the end of a node; it is not a value in it.

    A caller closing an optional port it has nothing to put on writes either
    nothing at all or a bare null final, and both must read back the same. This
    is the shape a unary `config` port arrives in when the caller wants the
    backend's own defaults.
    """
    node = AsyncNode(LocalChunkStore(f"consume-empty-{closed_with_null}"))
    if closed_with_null:
        await node.finalize(wait=True)
    else:
        await node.close()

    assert await node.consume(allow_none=True) is None


@pytest.mark.asyncio
async def test_iteration_skips_a_null_marker_rather_than_failing():
    node = AsyncNode(LocalChunkStore("iterate-null"))
    await _confirm(
        await node.put("first"),
        await node.put("second"),
    )
    await node.finalize(wait=True)

    assert [value async for value in node] == ["first", "second"]


@pytest.mark.asyncio
async def test_consume_rejects_invalid_terminal_shapes():
    null_node = AsyncNode(LocalChunkStore("consume-null"))
    await null_node.finalize(wait=True)
    with pytest.raises(StatusException) as raised:
        await null_node.consume()
    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION

    extra_node = AsyncNode(LocalChunkStore("consume-extra"))
    first = await extra_node.put("first")
    second = await extra_node.put("second", final=True)
    await _confirm(first, second)
    with pytest.raises(StatusException) as raised:
        await extra_node.consume()
    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION

    missing_node = AsyncNode(LocalChunkStore("consume-missing"))
    value = await missing_node.put("value")
    assert await value == 0
    await missing_node.close()
    with pytest.raises(StatusException) as raised:
        await missing_node.consume()
    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION


@pytest.mark.asyncio
async def test_consume_rejects_unordered_reader():
    node = AsyncNode(
        LocalChunkStore("unordered-consume"),
        reader_options=ChunkStoreReaderOptions(ordered=False),
    )
    assert await (await node.put("value", final=True)) == 0

    with pytest.raises(StatusException) as raised:
        await node.consume()

    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION


@pytest.mark.asyncio
async def test_invalid_operations_raise_status_exceptions():
    node = AsyncNode(LocalChunkStore("invalid-node"))

    with pytest.raises(StatusException) as raised:
        await node.next_fragment(1)  # type: ignore[arg-type]
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT

    with pytest.raises(StatusException) as raised:
        await node.put(object(), final=True)
    assert raised.value.status.code == StatusCode.NOT_FOUND

    node_ref = types.NodeRef(id="another-node")
    with pytest.raises(StatusException) as raised:
        await node.put_fragment(
            types.NodeFragment(data=node_ref, continued=False)
        )
    assert raised.value.status.code == StatusCode.UNIMPLEMENTED

    node.cancel_reader()
    await node.reader.wait()


def test_node_map_caches_nodes_and_uses_its_factory():
    created: list[str] = []

    def factory(node_id: str):
        created.append(node_id)
        return LocalChunkStore(node_id)

    node_map = NodeMap(factory)

    assert node_map.get("alpha") is node_map["alpha"]
    assert node_map.get("alpha").get_id() == "alpha"
    assert created == ["alpha"]
    assert len(node_map) == 1

    with pytest.raises(StatusException) as raised:
        node_map.get("not a valid id")
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT


@pytest.mark.asyncio
async def test_lifecycle_and_status_methods_delegate_to_accessors():
    node = AsyncNode(LocalChunkStore("lifecycle"))

    assert node.get_reader_status().is_ok()
    assert node.get_writer_status().is_ok()

    await node.finalize("value", wait=True)

    assert node.get_writer_status().is_ok()
    assert await node.next() == "value"
    assert await node.next() is None


@pytest.mark.asyncio
async def test_finalize_ends_the_stream_without_waiting():
    """The ordinary producer's ending: one call, and nothing awaited for it.

    The write and the closure are the writer pump's work, which is what lets a
    handler finalise and return.
    """
    node = AsyncNode(LocalChunkStore("finalize-async"))

    await (await node.put("value"))
    await node.finalize()

    assert [value async for value in node] == ["value"]
    assert node.get_writer_status().is_ok()
    assert not await node.is_writable()


@pytest.mark.asyncio
async def test_finalize_writes_a_last_value_and_can_wait_for_it():
    node = AsyncNode(LocalChunkStore("finalize-value"))

    await node.finalize({"value": 1}, wait=True)

    assert await node.consume() == {"value": 1}
    assert not node.writer.is_writable()


@pytest.mark.asyncio
async def test_finalize_can_place_the_final_chunk_at_a_sequence():
    """A producer that knows which seq is last spends one chunk, not two."""
    store = LocalChunkStore("finalize-seq")
    node = AsyncNode(store)

    await node.finalize("last", seq=3, wait=True)

    assert await store.get_final_seq() == 3


@pytest.mark.asyncio
async def test_finalize_can_leave_the_writer_open():
    """Finality and closure are two facts; `close=False` writes only one."""
    node = AsyncNode(LocalChunkStore("finalize-open"))

    await node.finalize(wait=True, close=False)

    # The node reports unwritable because a final sequence exists -- that is
    # finality. The writer is still open until it is closed.
    assert not await node.is_writable()
    assert node.writer.is_writable()

    await node.close()
    assert not node.writer.is_writable()


@pytest.mark.asyncio
async def test_close_ends_a_stream_that_has_no_final_value():
    """The specialised half: a log says "no more", not "that was the last"."""
    node = AsyncNode(LocalChunkStore("close-only"))

    await (await node.put("line"))
    await node.close()

    assert [value async for value in node] == ["line"]
    with pytest.raises(StatusException) as raised:
        await node.consume()
    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION


@pytest.mark.asyncio
async def test_abort_with_status_fails_readers_instead_of_ending_them():
    node = AsyncNode(LocalChunkStore("aborted"))
    failure = Status(code=StatusCode.DATA_LOSS, message="producer failed")

    await node.abort_with_status(failure)

    with pytest.raises(StatusException) as raised:
        await node.next_fragment()
    assert raised.value.status == failure
