"""Python conveniences for the native asynchronous node implementation."""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import AsyncIterator, Iterator, Sequence
from typing import Any, TypeVar, cast

from a11 import _native, timing
from a11.data import types
from a11.data.serialization import (
    SerializationRegistry,
    get_global_serialization_registry,
)
from a11.status import Status, StatusCode
from a11.stores.chunk_store import ChunkStore, ChunkStoreFactory
from a11.stores.chunk_store_reader import ChunkStoreReaderOptions
from a11.stores.chunk_store_writer import ChunkStoreWriterOptions
from a11.stores.local_chunk_store import LocalChunkStore
from a11 import timing

T = TypeVar("T")

_NativeAsyncNode = _native.AsyncNode
_NativeNodeMap = _native.NodeMap
_NativeReader = _native.ChunkStoreReader
_NativeWriter = _native.ChunkStoreWriter

# Save native descriptors before installing the language-native facade on the
# bound type. C++ always calls the implementation directly; these handles let
# Python conveniences do the same without maintaining duplicate stream state.
_native_node_init = _NativeAsyncNode.__init__
_native_node_reader = _NativeAsyncNode.reader
_native_node_writer = _NativeAsyncNode.writer
_native_reader_options = _NativeAsyncNode.__dict__["reader_options"]
_native_writer_options = _NativeAsyncNode.__dict__["writer_options"]
_native_reset_reader = _NativeAsyncNode.reset_reader
_native_writer_enqueue = _NativeWriter.enqueue_chunk


def _reader_options(
    options: ChunkStoreReaderOptions | dict[str, Any] | None,
) -> ChunkStoreReaderOptions:
    if options is None:
        return ChunkStoreReaderOptions()
    if isinstance(options, ChunkStoreReaderOptions):
        return options.model_copy(deep=True)
    return ChunkStoreReaderOptions.model_validate(options)


def _writer_options(
    options: ChunkStoreWriterOptions | dict[str, Any] | None,
) -> ChunkStoreWriterOptions:
    if options is None:
        return ChunkStoreWriterOptions()
    if isinstance(options, ChunkStoreWriterOptions):
        return options.model_copy(deep=True)
    return ChunkStoreWriterOptions.model_validate(options)


def _read_timeout(timeout: timing.Duration | None) -> timing.Duration:
    if timeout is None:
        return timing.infinite_duration()
    if not isinstance(timeout, timing.Duration):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="timeout must be a timing.Duration or None.",
        ).to_exception()
    return timeout


def _ensure_python_state(node: _NativeAsyncNode) -> None:
    state = node.__dict__
    state.setdefault(
        "_serialization_registry", get_global_serialization_registry()
    )
    state.setdefault("_expected_mimetype_patterns", "")
    state.setdefault("_expected_obj_type", None)


def _serialization_registry(node: _NativeAsyncNode) -> SerializationRegistry:
    _ensure_python_state(node)
    return cast(SerializationRegistry, node.__dict__["_serialization_registry"])


def _set_serialization_registry(
    node: _NativeAsyncNode, registry: SerializationRegistry
) -> None:
    if not isinstance(registry, SerializationRegistry):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="registry must be a SerializationRegistry.",
        ).to_exception()
    _ensure_python_state(node)
    node.__dict__["_serialization_registry"] = registry


def set_serialization_registry(
    node: _NativeAsyncNode, registry: SerializationRegistry
) -> _NativeAsyncNode:
    _set_serialization_registry(node, registry)
    return node


def _validate_expected_types(
    mimetype_patterns: str | Sequence[str], obj_type: type | None
) -> str | tuple[str, ...]:
    if isinstance(mimetype_patterns, str):
        patterns: str | tuple[str, ...] = mimetype_patterns
    elif isinstance(mimetype_patterns, Sequence) and not isinstance(
        mimetype_patterns, (bytes, bytearray, memoryview)
    ):
        if not all(isinstance(pattern, str) for pattern in mimetype_patterns):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="mimetype_patterns must contain only strings.",
            ).to_exception()
        patterns = tuple(mimetype_patterns)
    else:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                "mimetype_patterns must be a string or sequence of strings."
            ),
        ).to_exception()

    if obj_type is not None and not isinstance(obj_type, type):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="obj_type must be a type or None.",
        ).to_exception()
    return patterns


def set_expected_types(
    node: _NativeAsyncNode,
    mimetype_patterns: str | Sequence[str],
    obj_type: type | None,
) -> _NativeAsyncNode:
    patterns = _validate_expected_types(mimetype_patterns, obj_type)
    _ensure_python_state(node)
    node.__dict__["_expected_mimetype_patterns"] = patterns
    node.__dict__["_expected_obj_type"] = obj_type
    return node


@contextlib.contextmanager
def expect_types(
    node: _NativeAsyncNode,
    mimetype_patterns: str | Sequence[str],
    obj_type: type | None,
) -> Iterator[_NativeAsyncNode]:
    _ensure_python_state(node)
    previous_patterns = node.__dict__["_expected_mimetype_patterns"]
    previous_obj_type = node.__dict__["_expected_obj_type"]
    set_expected_types(node, mimetype_patterns, obj_type)
    try:
        yield node
    finally:
        node.__dict__["_expected_mimetype_patterns"] = previous_patterns
        node.__dict__["_expected_obj_type"] = previous_obj_type


def _resolve_expected_types(
    node: _NativeAsyncNode,
    mimetype_patterns: str | Sequence[str],
    obj_type: type[T] | None,
) -> tuple[str | Sequence[str], type[T] | None]:
    _ensure_python_state(node)
    resolved_patterns = (
        mimetype_patterns
        if mimetype_patterns
        else node.__dict__["_expected_mimetype_patterns"]
    )
    resolved_obj_type = (
        obj_type
        if obj_type is not None
        else cast(type[T] | None, node.__dict__["_expected_obj_type"])
    )
    return resolved_patterns, resolved_obj_type


def _reader(node: _NativeAsyncNode) -> _NativeReader:
    return _native_node_reader(node)


def _writer(node: _NativeAsyncNode) -> _NativeWriter:
    return _native_node_writer(node)


def get_reader_options(node: _NativeAsyncNode) -> ChunkStoreReaderOptions:
    return _native_reader_options.__get__(node, type(node)).model_copy(
        deep=True
    )


def set_reader_options(
    node: _NativeAsyncNode,
    options: ChunkStoreReaderOptions | dict[str, Any],
) -> _NativeAsyncNode:
    _native_reader_options.__set__(node, _reader_options(options))
    return node


def reset_reader(
    node: _NativeAsyncNode,
    options: ChunkStoreReaderOptions | dict[str, Any] | None = None,
) -> _NativeAsyncNode:
    converted = None if options is None else _reader_options(options)
    _native_reset_reader(node, converted)
    return node


def get_writer_options(node: _NativeAsyncNode) -> ChunkStoreWriterOptions:
    return _native_writer_options.__get__(node, type(node)).model_copy(
        deep=True
    )


def set_writer_options(
    node: _NativeAsyncNode,
    options: ChunkStoreWriterOptions | dict[str, Any],
) -> _NativeAsyncNode:
    _native_writer_options.__set__(node, _writer_options(options))
    return node


async def put_chunk(
    node: _NativeAsyncNode,
    chunk: types.Chunk,
    seq: int | None = None,
    final: bool = False,
) -> asyncio.Future[int]:
    """Enqueue a native chunk and return its durable confirmation future."""

    confirmation, admission = _native_writer_enqueue(
        _writer(node), chunk, seq=seq, final=final
    )
    if admission is not None:
        try:
            await admission
        except BaseException:
            confirmation.cancel()
            raise
    return cast(asyncio.Future[int], confirmation)


async def put_fragment(
    node: _NativeAsyncNode, fragment: types.NodeFragment
) -> asyncio.Future[int]:
    if not isinstance(fragment, types.NodeFragment):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="fragment must be a NodeFragment.",
        ).to_exception()
    if not isinstance(fragment.data, types.Chunk):
        raise Status(
            code=StatusCode.UNIMPLEMENTED,
            message="AsyncNode writers do not resolve NodeRef payloads.",
        ).to_exception()
    return await put_chunk(
        node,
        fragment.data,
        seq=fragment.seq,
        final=not fragment.continued,
    )


async def put(
    node: _NativeAsyncNode,
    value: Any,
    seq: int | None = None,
    final: bool = False,
    mimetype: str = "",
) -> asyncio.Future[int]:
    if isinstance(value, types.NodeFragment):
        if seq is not None or final or mimetype:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "seq, final, and mimetype are carried by a NodeFragment "
                    "and cannot be supplied separately."
                ),
            ).to_exception()
        return await put_fragment(node, value)
    if isinstance(value, types.Chunk):
        if mimetype:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="mimetype cannot be supplied with a raw Chunk.",
            ).to_exception()
        return await put_chunk(node, value, seq=seq, final=final)

    chunk = await asyncio.to_thread(
        _serialization_registry(node).to_chunk, value, mimetype
    )
    return await put_chunk(node, chunk, seq=seq, final=final)


async def put_final(
    node: _NativeAsyncNode,
    value: Any,
    seq: int | None = None,
    mimetype: str = "",
) -> asyncio.Future[int]:
    return await put(node, value, seq=seq, final=True, mimetype=mimetype)


async def put_null_final(
    node: _NativeAsyncNode, seq: int | None = None
) -> asyncio.Future[int]:
    return await put_chunk(
        node,
        types.Chunk(
            metadata=types.ChunkMetadata(mimetype="application/octet-stream")
        ),
        seq=seq,
        final=True,
    )


async def next_fragment(
    node: _NativeAsyncNode, timeout: timing.Duration | None = None
) -> types.NodeFragment | None:
    return await _reader(node).next(_read_timeout(timeout))


async def next_chunk(
    node: _NativeAsyncNode, timeout: timing.Duration | None = None
) -> types.Chunk | None:
    fragment = await next_fragment(node, timeout)
    return None if fragment is None else fragment.get_chunk()


async def _deserialize_fragment(
    node: _NativeAsyncNode,
    fragment: types.NodeFragment,
    mimetype_patterns: str | Sequence[str],
    obj_type: type[T] | None,
) -> T | Any:
    return await asyncio.to_thread(
        _serialization_registry(node).from_chunk,
        fragment.get_chunk(),
        mimetype_patterns,
        obj_type,
    )


async def next_object(
    node: _NativeAsyncNode,
    obj_type: type[T] | None = None,
    timeout: timing.Duration | None = None,
    mimetype_patterns: str | Sequence[str] = "",
) -> T | Any | None:
    fragment = await next_fragment(node, timeout)
    if fragment is None:
        return None
    chunk = fragment.get_chunk()
    if chunk.is_null():
        if fragment.continued:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="A null stream marker must be final.",
            ).to_exception()
        return None
    mimetype_patterns, obj_type = _resolve_expected_types(
        node, mimetype_patterns, obj_type
    )
    return await _deserialize_fragment(
        node, fragment, mimetype_patterns, obj_type
    )


async def next_value(
    node: _NativeAsyncNode,
    obj_type: type[T] | None = None,
    timeout: timing.Duration | None = None,
    mimetype_patterns: str | Sequence[str] = "",
) -> T | Any | None:
    return await next_object(node, obj_type, timeout, mimetype_patterns)


def _remaining_timeout(
    timeout: timing.Duration, started_at: timing.Time
) -> timing.Duration:
    if timeout.is_infinite():
        return timeout
    remaining = timeout - (timing.now() - started_at)
    if remaining <= timing.zero_duration():
        raise Status(
            code=StatusCode.DEADLINE_EXCEEDED,
            message=(
                "AsyncNode.consume() timed out before reaching the stream "
                "terminator."
            ),
        ).to_exception()
    return remaining


async def consume_fragment(
    node: _NativeAsyncNode,
    timeout: timing.Duration | None = None,
    allow_none: bool = False,
) -> types.NodeFragment | None:
    if not get_reader_options(node).ordered:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="consume() requires an ordered reader.",
        ).to_exception()

    converted_timeout = _read_timeout(timeout)
    started_at = timing.now()
    fragment = await next_fragment(node, converted_timeout)
    if fragment is None:
        if not allow_none:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="AsyncNode is empty at the current reader offset.",
            ).to_exception()
        return None

    chunk = fragment.get_chunk()
    if chunk.is_null():
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="AsyncNode cannot consume a null chunk as its value.",
        ).to_exception()
    if not fragment.continued:
        return fragment

    terminator = await next_fragment(
        node, _remaining_timeout(converted_timeout, started_at)
    )
    if terminator is None:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                "A continued consumed value must be followed by a null final "
                "chunk."
            ),
        ).to_exception()
    terminator_chunk = terminator.get_chunk()
    if terminator.continued or not terminator_chunk.is_null():
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                "The only fragment allowed after a consumed value is a null "
                "final chunk."
            ),
        ).to_exception()
    return fragment


async def consume_chunk(
    node: _NativeAsyncNode,
    timeout: timing.Duration | None = None,
    allow_none: bool = False,
) -> types.Chunk | None:
    fragment = await consume_fragment(node, timeout, allow_none=allow_none)
    if fragment is None:
        return None
    return fragment.get_chunk()


async def consume(
    node: _NativeAsyncNode,
    obj_type: type[T] | None = None,
    timeout: timing.Duration | None = None,
    mimetype_patterns: str | Sequence[str] = "",
    allow_none: bool = False,
) -> T | Any | None:
    fragment = await consume_fragment(node, timeout, allow_none=allow_none)
    if fragment is None:
        return None

    mimetype_patterns, obj_type = _resolve_expected_types(
        node, mimetype_patterns, obj_type
    )
    if obj_type is types.NodeFragment:
        return fragment
    if obj_type is types.Chunk:
        return fragment.get_chunk()
    return await _deserialize_fragment(
        node, fragment, mimetype_patterns, obj_type
    )


async def iter_fragments(
    node: _NativeAsyncNode, timeout: timing.Duration | None = None
) -> AsyncIterator[types.NodeFragment]:
    while (fragment := await next_fragment(node, timeout)) is not None:
        yield fragment


async def iter_chunks(
    node: _NativeAsyncNode, timeout: timing.Duration | None = None
) -> AsyncIterator[types.Chunk]:
    async for fragment in iter_fragments(node, timeout):
        yield fragment.get_chunk()


async def _aenter(node: _NativeAsyncNode) -> _NativeAsyncNode:
    return node


async def _aexit(
    node: _NativeAsyncNode,
    exc_type: type[BaseException] | None,
    exc: BaseException | None,
    traceback: Any,
) -> None:
    del exc_type, traceback
    if exc is None:
        await node.drain_and_close()
    else:
        await node.abort_with_status(Status.from_exception(exc))


def _aiter(node: _NativeAsyncNode) -> _NativeAsyncNode:
    return node


async def _anext(
    node: _NativeAsyncNode, timeout: timing.Duration | None = None
) -> Any:
    fragment = await next_fragment(node, timeout)
    if fragment is None:
        raise StopAsyncIteration
    chunk = fragment.get_chunk()
    if chunk.is_null():
        if fragment.continued:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="A null stream marker must be final.",
            ).to_exception()
        raise StopAsyncIteration
    _ensure_python_state(node)
    return await _deserialize_fragment(
        node,
        fragment,
        node.__dict__["_expected_mimetype_patterns"],
        node.__dict__["_expected_obj_type"],
    )


async def _iter_with_deadline(node: _NativeAsyncNode, deadline: timing.Time):
    node.set_reader_options(ChunkStoreReaderOptions(ordered=True))
    node = node.__aiter__()
    while True:
        try:
            yield await _anext(node, deadline - timing.now())
        except StopAsyncIteration:
            return


_NativeAsyncNode.serialization_registry = property(
    _serialization_registry, _set_serialization_registry
)
_NativeAsyncNode.set_serialization_registry = set_serialization_registry
_NativeAsyncNode.set_expected_types = set_expected_types
_NativeAsyncNode.expect_types = expect_types
_NativeAsyncNode._validate_expected_types = staticmethod(
    _validate_expected_types
)
_NativeAsyncNode.reader = property(_reader)
_NativeAsyncNode.writer = property(_writer)
_NativeAsyncNode.get_reader_options = get_reader_options
_NativeAsyncNode.set_reader_options = set_reader_options
_NativeAsyncNode.reset_reader = reset_reader
_NativeAsyncNode.get_writer_options = get_writer_options
_NativeAsyncNode.set_writer_options = set_writer_options
_NativeAsyncNode.put_chunk = put_chunk
_NativeAsyncNode.put_fragment = put_fragment
_NativeAsyncNode.put = put
_NativeAsyncNode.put_final = put_final
_NativeAsyncNode.put_null_final = put_null_final
_NativeAsyncNode.next_fragment = next_fragment
_NativeAsyncNode.next_chunk = next_chunk
_NativeAsyncNode.next_object = next_object
_NativeAsyncNode.next = next_value
_NativeAsyncNode.consume_fragment = consume_fragment
_NativeAsyncNode.consume_chunk = consume_chunk
_NativeAsyncNode.consume = consume
_NativeAsyncNode.iter_fragments = iter_fragments
_NativeAsyncNode.iter_chunks = iter_chunks
_NativeAsyncNode.iter_with_deadline = _iter_with_deadline
_NativeAsyncNode.__aenter__ = _aenter
_NativeAsyncNode.__aexit__ = _aexit
_NativeAsyncNode.__aiter__ = _aiter
_NativeAsyncNode.__anext__ = _anext


def _node_init(
    node: _NativeAsyncNode,
    chunk_store: ChunkStore,
    node_map: _NativeNodeMap | None = None,
    *,
    serialization_registry: SerializationRegistry | None = None,
    reader_options: ChunkStoreReaderOptions | dict[str, Any] | None = None,
    writer_options: ChunkStoreWriterOptions | dict[str, Any] | None = None,
) -> None:
    registry = (
        get_global_serialization_registry()
        if serialization_registry is None
        else serialization_registry
    )
    if not isinstance(registry, SerializationRegistry):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="serialization_registry must be a SerializationRegistry.",
        ).to_exception()
    _native_node_init(
        node,
        chunk_store,
        None,
        _reader_options(reader_options),
        _writer_options(writer_options),
    )
    node._node_map = node_map
    node._serialization_registry = registry
    node._expected_mimetype_patterns = ""
    node._expected_obj_type = None


def _create_node(
    cls: type[_NativeAsyncNode],
    node_id: str,
    node_map: _NativeNodeMap | None = None,
    *,
    serialization_registry: SerializationRegistry | None = None,
    reader_options: ChunkStoreReaderOptions | dict[str, Any] | None = None,
    writer_options: ChunkStoreWriterOptions | dict[str, Any] | None = None,
    chunk_store_factory: ChunkStoreFactory = LocalChunkStore,
) -> _NativeAsyncNode:
    return cls(
        chunk_store_factory(node_id),
        node_map,
        serialization_registry=serialization_registry,
        reader_options=reader_options,
        writer_options=writer_options,
    )


_NativeAsyncNode.__init__ = _node_init
_NativeAsyncNode.create = classmethod(_create_node)
_NativeAsyncNode.__module__ = "a11.nodes.async_node"
AsyncNode = _NativeAsyncNode

NodeMap = _NativeNodeMap
NodeMap.__module__ = "a11.nodes.async_node"
NodeMap.__getitem__ = NodeMap.get

__all__ = ["AsyncNode", "NodeMap"]
