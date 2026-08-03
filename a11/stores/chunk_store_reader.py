"""The Python-facing protocol for the native `ChunkStoreReader`.

A `ChunkStoreReader` is the read cursor over a
[ChunkStore][a11.stores.chunk_store.ChunkStore]: it pulls
[NodeFragment][a11.data.types.NodeFragment] values out in order (or by arrival),
buffering ahead per its `ChunkStoreReaderOptions`. Most code reaches a
reader through an [AsyncNode][a11.nodes.async_node.AsyncNode], but it is a
first-class object you can drive directly and ``async for`` over.

The class exported here is the native ``a11._native.ChunkStoreReader``; this
module attaches its validating constructor and async-iteration protocol via
[attach_protocol][a11._native_protocol.attach_protocol].
"""

from __future__ import annotations

from typing import Any

from a11 import _native, timing
from a11._native_options import install_native_options
from a11._native_protocol import attach_protocol
from a11.data import types
from a11.status import Status, StatusCode
from a11.stores.chunk_store import ChunkStore

from a11._native import ChunkStoreReaderOptions

install_native_options(
    ChunkStoreReaderOptions,
    {
        "ordered": (bool, True),
        "pop_chunks": (bool, False),
        "num_chunks_to_buffer": (int, 32),
        "offset": (int, 0),
        "max_chunks_to_read": (int | None, None),
        "sticky_mimetype": (bool, False),
    },
)
ChunkStoreReaderOptions.__module__ = __name__

from a11._native import ChunkStoreReader

# Native descriptors captured before ``attach_protocol`` overwrites them.
_native_init = ChunkStoreReader.__init__
_native_next = ChunkStoreReader.next


class _ChunkStoreReaderProtocol:
    """An ordered, buffered read cursor over a ChunkStore."""

    def __init__(
        self,
        store: ChunkStore,
        options: ChunkStoreReaderOptions | dict[str, Any] | None = None,
    ) -> None:
        """Open a reader over ``store``.

        ``options`` (a `ChunkStoreReaderOptions` or plain dict) tunes
        ordering, buffering, starting offset, sticky mimetypes, and whether
        chunks are popped as they are read.
        """
        if options is None:
            options = ChunkStoreReaderOptions()
        elif not isinstance(options, ChunkStoreReaderOptions):
            options = ChunkStoreReaderOptions.model_validate(options)
        _native_init(self, store, options)

    def next(self, timeout: timing.Duration = timing.infinite_duration()):
        """Await the next fragment (``None`` at end of stream), up to ``
        timeout``."""
        if not isinstance(timeout, timing.Duration):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="timeout must be a timing.Duration instance.",
            ).to_exception()
        if timeout < timing.zero_duration():
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="timeout must be non-negative or infinite.",
            ).to_exception()
        return _native_next(self, timeout)

    def __aiter__(self) -> ChunkStoreReader:
        return self

    async def __anext__(self) -> types.NodeFragment:
        fragment = await self.next()
        if fragment is None:
            raise StopAsyncIteration
        return fragment


attach_protocol(ChunkStoreReader, _ChunkStoreReaderProtocol)
ChunkStoreReader.__module__ = __name__

__all__ = ["ChunkStoreReader", "ChunkStoreReaderOptions"]
