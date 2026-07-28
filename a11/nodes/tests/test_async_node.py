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
    null_write = await node.put_null_final()
    assert await _confirm(value_write, null_write) == [0, 1]

    assert [value async for value in node] == [("only", "value")]


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
    writes = [value_write]
    if trailing_null:
        writes.append(await node.put_null_final())
    await _confirm(*writes)

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
async def test_consume_rejects_invalid_terminal_shapes():
    null_node = AsyncNode(LocalChunkStore("consume-null"))
    assert await (await null_node.put_null_final()) == 0
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
    await missing_node.drain_and_close()
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

    confirmation = await node.put("value", final=True)
    await confirmation
    await node.drain_and_close()

    assert node.get_writer_status().is_ok()
    assert await node.next() == "value"
    assert await node.next() is None


@pytest.mark.asyncio
async def test_context_manager_closes_node_on_success():
    node = AsyncNode(LocalChunkStore("context-success"))

    async with node as output:
        assert output is node
        await (await output.put("value"))

    assert node.get_writer_status().is_ok()
    assert await node.next() == "value"
    assert await node.next() is None


@pytest.mark.asyncio
async def test_context_manager_aborts_node_with_body_error():
    node = AsyncNode(LocalChunkStore("context-error"))
    failure = Status(code=StatusCode.DATA_LOSS, message="producer failed")

    with pytest.raises(StatusException) as raised:
        async with node:
            raise failure.to_exception()

    assert raised.value.status == failure
    with pytest.raises(StatusException) as raised:
        await node.next_fragment()
    assert raised.value.status == failure


@pytest.mark.asyncio
async def test_context_manager_preserves_non_status_body_error():
    node = AsyncNode(LocalChunkStore("context-unknown-error"))

    with pytest.raises(ValueError, match="producer failed"):
        async with node:
            raise ValueError("producer failed")

    with pytest.raises(StatusException) as raised:
        await node.next_fragment()
    assert raised.value.status.code == StatusCode.UNKNOWN
    assert raised.value.status.message == "producer failed"


@pytest.mark.asyncio
async def test_context_manager_aborts_node_and_preserves_cancellation():
    node = AsyncNode(LocalChunkStore("context-cancelled"))
    entered = asyncio.Event()

    async def produce() -> None:
        async with node:
            entered.set()
            await asyncio.Event().wait()

    task = asyncio.create_task(produce())
    await entered.wait()
    task.cancel()

    with pytest.raises(asyncio.CancelledError):
        await task
    with pytest.raises(StatusException) as raised:
        await node.next_fragment()
    assert raised.value.status.code == StatusCode.CANCELLED
