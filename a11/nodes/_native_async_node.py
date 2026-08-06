"""The Python-facing async protocol for the native `AsyncNode`.

An `AsyncNode` is A11's unit of streaming state: a single, ordered
sequence of chunks that one side *writes* and another side *reads*, backed by a
[ChunkStore][a11.stores.chunk_store.ChunkStore] and optionally mirrored across a
[WireStream][a11.net.wire_stream.WireStream] to a remote peer. Nodes are how the
inputs and outputs of an [Action][a11.actions.action.Action] carry data, and how
an agent streams partial results (tokens, audio frames, tool calls) to its
caller before the work is finished.

The class exported here is the native ``a11._native.AsyncNode``; this module
attaches its Python protocol -- awaiting produced values, ``async for`` over the
stream, serializing arbitrary Python objects on the way in and deserializing
them on the way out -- via
[attach_protocol][a11._native_protocol.attach_protocol]. The
methods below run bound to native instances; the ``_AsyncNodeProtocol`` class is
only a readable description of them.
"""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import AsyncIterator, Iterator, Sequence
from typing import Any, TypeVar, cast

from a11 import _native, timing
from a11._native_protocol import attach_protocol
from a11.data import types
from a11.data.serialization import (
    SerializationRegistry,
    get_global_serialization_registry,
)
from a11.status import Status, StatusCode
from a11.stores.chunk_store import ChunkStore, ChunkStoreFactory
from a11.stores.chunk_store_reader import (
    ChunkStoreReader,
    ChunkStoreReaderOptions,
)
from a11.stores.chunk_store_writer import (
    ChunkStoreWriter,
    ChunkStoreWriterOptions,
)
from a11.stores.local_chunk_store import LocalChunkStore

T = TypeVar("T")

from a11._native import AsyncNode
from a11._native import NodeMap

# Native descriptors captured before ``attach_protocol`` overwrites them. C++
# always calls the native implementation directly; these handles let the Python
# conveniences reach it too, without maintaining duplicate stream state.
_native_node_init = AsyncNode.__init__
_native_node_reader = AsyncNode.reader
_native_node_writer = AsyncNode.writer
_native_reader_options = AsyncNode.__dict__["reader_options"]
_native_writer_options = AsyncNode.__dict__["writer_options"]
_native_reset_reader = AsyncNode.reset_reader
_native_writer_enqueue = _native.ChunkStoreWriter.enqueue_chunk


# --- Internal helpers (not part of the public class; never attached) ---------


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


async def _next_value_fragment(
    node: AsyncNode,
    converted_timeout: timing.Duration,
    started_at: timing.Time,
) -> types.NodeFragment | None:
    """The next fragment carrying a value, or ``None`` once the node ends.

    A null chunk is a marker, not a value: a final one says the node is
    finished, and a non-final one says nothing at all. Neither is something a
    reader asked for, so both are skipped here rather than surfaced as a value
    or rejected — which is what lets a node be closed with nothing in it.
    """
    while True:
        fragment = await node.next_fragment(
            _remaining_timeout(converted_timeout, started_at)
        )
        if fragment is None:
            return None
        if not fragment.get_chunk().is_null():
            return fragment
        if not fragment.continued:
            return None


def _ensure_python_state(node: AsyncNode) -> None:
    state = node.__dict__
    state.setdefault(
        "_serialization_registry", get_global_serialization_registry()
    )
    state.setdefault("_expected_mimetype_patterns", "")
    state.setdefault("_expected_obj_type", None)


def _get_serialization_registry(node: AsyncNode) -> SerializationRegistry:
    _ensure_python_state(node)
    return cast(SerializationRegistry, node.__dict__["_serialization_registry"])


def _set_serialization_registry(
    node: AsyncNode, registry: SerializationRegistry
) -> None:
    if not isinstance(registry, SerializationRegistry):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="registry must be a SerializationRegistry.",
        ).to_exception()
    _ensure_python_state(node)
    node.__dict__["_serialization_registry"] = registry


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


def _resolve_expected_types(
    node: AsyncNode,
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
                "AsyncNode timed out before reaching the stream terminator."
            ),
        ).to_exception()
    return remaining


async def _deserialize_fragment(
    node: AsyncNode,
    fragment: types.NodeFragment,
    mimetype_patterns: str | Sequence[str],
    obj_type: type[T] | None,
) -> T | Any:
    return await asyncio.to_thread(
        _get_serialization_registry(node).from_chunk,
        fragment.get_chunk(),
        mimetype_patterns,
        obj_type,
    )


# --- The Python protocol attached onto the native AsyncNode ------------------


class _AsyncNodeProtocol:
    """An asynchronous, ordered stream of chunks read from and written to A11.

    A node has two halves. The **writer** end accepts values -- native
    [Chunk][a11.data.types.Chunk] objects,
    [NodeFragment][a11.data.types.NodeFragment]
    objects, or any Python object a serializer in the node's
    [SerializationRegistry][a11.data.serialization.SerializationRegistry] can
    encode -- and
    admits them into the backing
    [ChunkStore][a11.stores.chunk_store.ChunkStore] in
    sequence. The **reader** end yields them back, deserializing on the way out.
    Every ``put*`` coroutine admits the write and returns an `asyncio.Future`
    that completes once the backing store accepts the chunk. The writer also
    attempts or queues sends to attached
    [WireStreams][a11.net.wire_stream.WireStream] while processing the batch,
    but those sends are not a separate delivery-acknowledgement barrier and a
    later tee failure cannot revoke an already confirmed store write.

    Consume a node in whichever shape fits the work:

    - ``await node.next()`` for the next value (``None`` at end of stream);
    - ``async for value in node:`` to iterate to completion;
    - ``await node.consume()`` when exactly one whole value is expected;
    - the ``*_chunk`` / ``*_fragment`` variants to stay at the transport level.

    Use it as an async context manager to guarantee buffered writes are
    drained and the writer is closed::

        async with AsyncNode.create("output") as node:
            await node.put("hello")
            await node.put_final("world")

    A clean exit drains and closes the writer; it does not invent a final
    fragment, so write one with `put_final` or `put_null_final` before leaving
    the block. Leaving via an exception aborts the node with that error's
    [Status][a11.status.Status], so a reader on the far end observes the
    failure instead of a truncated stream.
    """

    def __init__(
        self,
        chunk_store: ChunkStore,
        node_map: NodeMap | None = None,
        *,
        serialization_registry: SerializationRegistry | None = None,
        reader_options: ChunkStoreReaderOptions | dict[str, Any] | None = None,
        writer_options: ChunkStoreWriterOptions | dict[str, Any] | None = None,
    ) -> None:
        """Build a node over ``chunk_store``.

        Prefer `create`, which constructs the store for you from a node
        id. Pass ``reader_options`` / ``writer_options`` (as
        `ChunkStoreReaderOptions` / `ChunkStoreWriterOptions` or plain dicts) to
        tune buffering and ordering, and a custom ``serialization_registry`` to
        control how Python objects map to chunks.
        """
        registry = (
            get_global_serialization_registry()
            if serialization_registry is None
            else serialization_registry
        )
        if not isinstance(registry, SerializationRegistry):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "serialization_registry must be a SerializationRegistry."
                ),
            ).to_exception()
        _native_node_init(
            self,
            chunk_store,
            None,
            _reader_options(reader_options),
            _writer_options(writer_options),
        )
        self._node_map = node_map
        self._serialization_registry = registry
        self._expected_mimetype_patterns = ""
        self._expected_obj_type = None

    @classmethod
    def create(
        cls: type[AsyncNode],
        node_id: str,
        node_map: NodeMap | None = None,
        *,
        serialization_registry: SerializationRegistry | None = None,
        reader_options: ChunkStoreReaderOptions | dict[str, Any] | None = None,
        writer_options: ChunkStoreWriterOptions | dict[str, Any] | None = None,
        chunk_store_factory: ChunkStoreFactory = LocalChunkStore,
    ) -> AsyncNode:
        """Create a standalone node identified by ``node_id``.

        ``chunk_store_factory`` builds the backing store from the id; it
        defaults to an in-memory
        [LocalChunkStore][a11.stores.local_chunk_store.LocalChunkStore],
        so overriding it is how you place a node's data in a different backend.

        Examples:
            Create a stream used to deliver answer fragments:

            ```python
            answer = AsyncNode.create("answer-tokens")
            ```
        """
        return cls(
            chunk_store_factory(node_id),
            node_map,
            serialization_registry=serialization_registry,
            reader_options=reader_options,
            writer_options=writer_options,
        )

    @staticmethod
    def _validate_expected_types(
        mimetype_patterns: str | Sequence[str], obj_type: type | None
    ) -> str | tuple[str, ...]:
        return _validate_expected_types(mimetype_patterns, obj_type)

    # --- Configuration -------------------------------------------------------

    @property
    def serialization_registry(self) -> SerializationRegistry:
        """The registry used to (de)serialize Python objects for this node."""
        return _get_serialization_registry(self)

    @serialization_registry.setter
    def serialization_registry(self, registry: SerializationRegistry) -> None:
        _set_serialization_registry(self, registry)

    def set_serialization_registry(
        self, registry: SerializationRegistry
    ) -> AsyncNode:
        """Set the serialization registry and return ``self`` for chaining."""
        _set_serialization_registry(self, registry)
        return self

    def set_expected_types(
        self,
        mimetype_patterns: str | Sequence[str],
        obj_type: type | None,
    ) -> AsyncNode:
        """Set the default MIME patterns and object type for reads.

        Once set, ``next()``/``consume()`` and ``async for`` deserialize to
        ``obj_type`` (matching ``mimetype_patterns``) without repeating those
        arguments on every call. Returns ``self`` for chaining.
        """
        patterns = _validate_expected_types(mimetype_patterns, obj_type)
        _ensure_python_state(self)
        self.__dict__["_expected_mimetype_patterns"] = patterns
        self.__dict__["_expected_obj_type"] = obj_type
        return self

    @contextlib.contextmanager
    def expect_types(
        self,
        mimetype_patterns: str | Sequence[str],
        obj_type: type | None,
    ) -> Iterator[AsyncNode]:
        """Temporarily set the expected read types for the ``with`` block."""
        _ensure_python_state(self)
        previous_patterns = self.__dict__["_expected_mimetype_patterns"]
        previous_obj_type = self.__dict__["_expected_obj_type"]
        self.set_expected_types(mimetype_patterns, obj_type)
        try:
            yield self
        finally:
            self.__dict__["_expected_mimetype_patterns"] = previous_patterns
            self.__dict__["_expected_obj_type"] = previous_obj_type

    @property
    def reader(self) -> ChunkStoreReader:
        """The node's
        [ChunkStoreReader][a11.stores.chunk_store_reader.ChunkStoreReader]."""
        return _native_node_reader(self)

    @property
    def writer(self) -> ChunkStoreWriter:
        """The node's
        [ChunkStoreWriter][a11.stores.chunk_store_writer.ChunkStoreWriter]."""
        return _native_node_writer(self)

    def get_reader_options(self) -> ChunkStoreReaderOptions:
        """Return a copy of the reader's current options."""
        return _native_reader_options.__get__(self, type(self)).model_copy(
            deep=True
        )

    def set_reader_options(
        self, options: ChunkStoreReaderOptions | dict[str, Any]
    ) -> AsyncNode:
        """Replace the reader options and return ``self`` for chaining."""
        _native_reader_options.__set__(self, _reader_options(options))
        return self

    def reset_reader(
        self,
        options: ChunkStoreReaderOptions | dict[str, Any] | None = None,
    ) -> AsyncNode:
        """Rewind/reconfigure the reader (e.g. to re-read from an offset)."""
        converted = None if options is None else _reader_options(options)
        _native_reset_reader(self, converted)
        return self

    def get_writer_options(self) -> ChunkStoreWriterOptions:
        """Return a copy of the writer's current options."""
        return _native_writer_options.__get__(self, type(self)).model_copy(
            deep=True
        )

    def set_writer_options(
        self, options: ChunkStoreWriterOptions | dict[str, Any]
    ) -> AsyncNode:
        """Replace the writer options and return ``self`` for chaining."""
        _native_writer_options.__set__(self, _writer_options(options))
        return self

    # --- Writing -------------------------------------------------------------

    async def put_chunk(
        self,
        chunk: types.Chunk,
        seq: int | None = None,
        final: bool = False,
    ) -> asyncio.Future[int]:
        """Admit a native chunk and return its store-confirmation future.

        Await this coroutine to respect the writer's bounded admission buffer,
        then await the returned future when the backing store must have
        accepted the fragment. Attached stream sends are attempted or queued
        as the writer processes the batch, but do not add a second delivery
        confirmation.
        """
        confirmation, admission = _native_writer_enqueue(
            self.writer, chunk, seq=seq, final=final
        )
        if admission is not None:
            try:
                await admission
            except BaseException:
                confirmation.cancel()
                raise
        return cast(asyncio.Future[int], confirmation)

    async def put_fragment(
        self, fragment: types.NodeFragment
    ) -> asyncio.Future[int]:
        """Enqueue a [NodeFragment][a11.data.types.NodeFragment] (carrying its
        seq/final)."""
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
        return await self.put_chunk(
            fragment.data,
            seq=fragment.seq,
            final=not fragment.continued,
        )

    async def put(
        self,
        value: Any,
        seq: int | None = None,
        final: bool = False,
        mimetype: str = "",
    ) -> asyncio.Future[int]:
        """Write ``value`` and return its store-confirmation future.

        ``value`` may be a [NodeFragment][a11.data.types.NodeFragment], a
        [Chunk][a11.data.types.Chunk], or any Python object the node's
        serialization registry can encode (``mimetype`` selects the encoding).
        Set ``final=True`` on the last data fragment so readers know where the
        logical value ends. Finality does not close the writer: call
        `drain_and_close` after the confirmation future resolves. The returned
        `asyncio.Future` resolves to the stored sequence number after the
        backing store accepts the fragment. Attached WireStream sends are
        attempted or queued by the writer but are not separately acknowledged.

        Examples:
            Add an intermediate token while a model response is produced:

            ```python
            await answer.put("The shipment ")
            ```
        """
        if isinstance(value, types.NodeFragment):
            if seq is not None or final or mimetype:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        "seq, final, and mimetype are carried by a "
                        "NodeFragment and cannot be supplied separately."
                    ),
                ).to_exception()
            return await self.put_fragment(value)
        if isinstance(value, types.Chunk):
            if mimetype:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message="mimetype cannot be supplied with a raw Chunk.",
                ).to_exception()
            return await self.put_chunk(value, seq=seq, final=final)

        chunk = await asyncio.to_thread(
            _get_serialization_registry(self).to_chunk, value, mimetype
        )
        return await self.put_chunk(chunk, seq=seq, final=final)

    async def put_final(
        self,
        value: Any,
        seq: int | None = None,
        mimetype: str = "",
    ) -> asyncio.Future[int]:
        """Write ``value`` as the logical final element.

        This marks the final sequence but leaves the writer open. The returned
        confirmation can be awaited when immediate store acceptance matters;
        otherwise a later `drain_and_close` flushes queued work.

        Examples:
            Mark the last visible fragment and close the producer:

            ```python
            await answer.put_final("arrives Friday.")
            ```
        """
        return await self.put(value, seq=seq, final=True, mimetype=mimetype)

    async def put_null_final(
        self, seq: int | None = None
    ) -> asyncio.Future[int]:
        """Write an explicit null fragment as the logical terminator.

        Use this after a non-final value when `consume` should treat that value
        as one complete unary result. It does not close the writer; finish with
        `drain_and_close` after the confirmation resolves.
        """
        return await self.put_chunk(
            types.Chunk(
                metadata=types.ChunkMetadata(
                    mimetype="application/octet-stream"
                )
            ),
            seq=seq,
            final=True,
        )

    # --- Reading -------------------------------------------------------------

    async def next_fragment(
        self, timeout: timing.Duration | None = None
    ) -> types.NodeFragment | None:
        """Read the next raw fragment, or ``None`` at end of stream."""
        return await self.reader.next(_read_timeout(timeout))

    async def next_chunk(
        self, timeout: timing.Duration | None = None
    ) -> types.Chunk | None:
        """Read the next raw chunk, or ``None`` at end of stream."""
        fragment = await self.next_fragment(timeout)
        return None if fragment is None else fragment.get_chunk()

    async def next_object(
        self,
        obj_type: type[T] | None = None,
        timeout: timing.Duration | None = None,
        mimetype_patterns: str | Sequence[str] = "",
    ) -> T | Any | None:
        """Read and deserialize the next value, or ``None`` at end of stream."""
        fragment = await _next_value_fragment(
            self, _read_timeout(timeout), timing.now()
        )
        if fragment is None:
            return None
        mimetype_patterns, obj_type = _resolve_expected_types(
            self, mimetype_patterns, obj_type
        )
        return await _deserialize_fragment(
            self, fragment, mimetype_patterns, obj_type
        )

    async def next(
        self,
        obj_type: type[T] | None = None,
        timeout: timing.Duration | None = None,
        mimetype_patterns: str | Sequence[str] = "",
    ) -> T | Any | None:
        """Alias for `next_object`: the next deserialized value or ``None``.

        Examples:
            Process a live audit stream one event at a time:

            ```python
            events.set_expected_types("application/json", AuditEvent)
            while (event := await events.next()) is not None:
                await audit_index.store(event)
            ```
        """
        return await self.next_object(obj_type, timeout, mimetype_patterns)

    async def consume_fragment(
        self,
        timeout: timing.Duration | None = None,
        allow_none: bool = False,
    ) -> types.NodeFragment | None:
        """Read exactly one whole value's fragment, enforcing the terminator.

        Unlike `next_fragment`, this expects the node to hold exactly one
        value, and raises if that shape is violated. Two spellings are
        accepted: the value written as final, or the value followed by a null
        final chunk. With ``allow_none`` a node that holds no value — closed
        empty, or holding nothing but a null final — yields ``None`` instead of
        raising. Requires an ordered reader.
        """
        if not self.get_reader_options().ordered:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="consume() requires an ordered reader.",
            ).to_exception()

        converted_timeout = _read_timeout(timeout)
        started_at = timing.now()
        fragment = await _next_value_fragment(
            self, converted_timeout, started_at
        )
        if fragment is None:
            if not allow_none:
                raise Status(
                    code=StatusCode.FAILED_PRECONDITION,
                    message="AsyncNode is empty at the current reader offset.",
                ).to_exception()
            return None

        if not fragment.continued:
            return fragment

        terminator = await self.next_fragment(
            _remaining_timeout(converted_timeout, started_at)
        )
        if terminator is None:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message=(
                    "A continued consumed value must be followed by a null "
                    "final chunk."
                ),
            ).to_exception()
        terminator_chunk = terminator.get_chunk()
        if terminator.continued or not terminator_chunk.is_null():
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message=(
                    "The only fragment allowed after a consumed value is a "
                    "null final chunk."
                ),
            ).to_exception()
        return fragment

    async def consume_chunk(
        self,
        timeout: timing.Duration | None = None,
        allow_none: bool = False,
    ) -> types.Chunk | None:
        """Consume exactly one whole value and return its raw chunk."""
        fragment = await self.consume_fragment(timeout, allow_none=allow_none)
        if fragment is None:
            return None
        return fragment.get_chunk()

    async def consume(
        self,
        obj_type: type[T] | None = None,
        timeout: timing.Duration | None = None,
        mimetype_patterns: str | Sequence[str] = "",
        allow_none: bool = False,
    ) -> T | Any | None:
        """Consume exactly one whole value and return it deserialized.

        Use this for a node that carries a single result (the common case for a
        unary action output). Pass ``obj_type`` to deserialize to a specific
        type, or request ``NodeFragment``/``Chunk`` to get the raw form.

        Examples:
            Read the unary customer input of an action handler:

            ```python
            customer = await action["customer"].consume(obj_type=Customer)
            ```
        """
        fragment = await self.consume_fragment(timeout, allow_none=allow_none)
        if fragment is None:
            return None

        mimetype_patterns, obj_type = _resolve_expected_types(
            self, mimetype_patterns, obj_type
        )
        if obj_type is types.NodeFragment:
            return fragment
        if obj_type is types.Chunk:
            return fragment.get_chunk()
        return await _deserialize_fragment(
            self, fragment, mimetype_patterns, obj_type
        )

    async def iter_fragments(
        self, timeout: timing.Duration | None = None
    ) -> AsyncIterator[types.NodeFragment]:
        """Async-iterate raw fragments until the stream ends."""
        while (fragment := await self.next_fragment(timeout)) is not None:
            yield fragment

    async def iter_chunks(
        self, timeout: timing.Duration | None = None
    ) -> AsyncIterator[types.Chunk]:
        """Async-iterate raw chunks until the stream ends."""
        async for fragment in self.iter_fragments(timeout):
            yield fragment.get_chunk()

    async def iter_with_deadline(self, deadline: timing.Time):
        """Async-iterate deserialized values until ``deadline`` or end of
        stream."""
        self.set_reader_options(ChunkStoreReaderOptions(ordered=True))
        node = self.__aiter__()
        while True:
            try:
                yield await node.__anext__(deadline - timing.now())
            except StopAsyncIteration:
                return

    # --- Lifecycle and async iteration --------------------------------------

    async def __aenter__(self) -> AsyncNode:
        return self

    async def __aexit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        traceback: Any,
    ) -> None:
        del exc_type, traceback
        if exc is None:
            await self.drain_and_close()
        else:
            await self.abort_with_status(Status.from_exception(exc))

    def __aiter__(self) -> AsyncNode:
        return self

    async def __anext__(self, timeout: timing.Duration | None = None) -> Any:
        fragment = await _next_value_fragment(
            self, _read_timeout(timeout), timing.now()
        )
        if fragment is None:
            raise StopAsyncIteration
        _ensure_python_state(self)
        return await _deserialize_fragment(
            self,
            fragment,
            self.__dict__["_expected_mimetype_patterns"],
            self.__dict__["_expected_obj_type"],
        )


attach_protocol(AsyncNode, _AsyncNodeProtocol)
AsyncNode.__module__ = "a11.nodes.async_node"

NodeMap.__module__ = "a11.nodes.async_node"
NodeMap.__getitem__ = NodeMap.get

__all__ = ["AsyncNode", "NodeMap"]
