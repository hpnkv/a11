"""The Python-facing protocol for the native `ChunkStoreWriter`.

A `ChunkStoreWriter` is the write cursor over a
[ChunkStore][a11.stores.chunk_store.ChunkStore]: it admits
[Chunk][a11.data.types.Chunk] values into the store in sequence, applying
backpressure through its `ChunkStoreWriterOptions` buffer. Each write
returns a `asyncio.Future` that completes once the chunk is durably
stored, so a producer can pace itself by awaiting it. Most code reaches a writer
through an [AsyncNode][a11.nodes.async_node.AsyncNode], but it is usable
directly.

The class exported here is the native ``a11._native.ChunkStoreWriter``; this
module attaches its validating constructor and async ``put`` protocol via
[attach_protocol][a11._native_protocol.attach_protocol].
"""

from __future__ import annotations

import asyncio
from typing import Any

from a11 import _native
from a11._native_options import install_native_options
from a11._native_protocol import attach_protocol
from a11.data import types
from a11.status import Status, StatusCode
from a11.stores.chunk_store import ChunkStore

from a11._native import ChunkStoreWriterOptions

install_native_options(
    ChunkStoreWriterOptions,
    {
        "offset": (int, 0),
        "max_chunks_to_write_at_once": (int, 8),
        "num_chunks_to_buffer": (int | None, None),
        "sticky_mimetype": (bool, False),
    },
)
ChunkStoreWriterOptions.__module__ = __name__

from a11._native import ChunkStoreWriter

# Native descriptors captured before ``attach_protocol`` overwrites them.
_native_init = ChunkStoreWriter.__init__
_native_enqueue_chunk = ChunkStoreWriter.enqueue_chunk


class _ChunkStoreWriterProtocol:
    """A buffered, backpressured write cursor over a ChunkStore."""

    def __init__(
        self,
        chunk_store: ChunkStore,
        options: ChunkStoreWriterOptions | dict[str, Any] | None = None,
    ) -> None:
        """Open a writer over ``chunk_store``.

        ``options`` (a `ChunkStoreWriterOptions` or plain dict) tunes the
        starting offset, sticky mimetypes, and how much is buffered/flushed at
        once.
        """
        if options is None:
            options = ChunkStoreWriterOptions()
        elif not isinstance(options, ChunkStoreWriterOptions):
            options = ChunkStoreWriterOptions.model_validate(options)
        _native_init(self, chunk_store, options)

    async def put(
        self,
        obj: Any,
        seq: int | None = None,
        final: bool = False,
    ) -> asyncio.Future[int]:
        """Write a [Chunk][a11.data.types.Chunk] and confirm it durably.

        The writer operates at the chunk level; pass an already-serialized
        [Chunk][a11.data.types.Chunk] (use
        [AsyncNode][a11.nodes.async_node.AsyncNode]
        to write arbitrary Python objects). Returns a `asyncio.Future`
        resolving to the stored sequence number.
        """
        if not isinstance(obj, types.Chunk):
            raise Status(
                code=StatusCode.UNIMPLEMENTED,
                message=(
                    "ChunkStoreWriter.put is not implemented for generic"
                    " objects."
                ),
            ).to_exception()
        return await self.put_chunk(obj, seq=seq, final=final)

    async def put_chunk(
        self,
        chunk: types.Chunk,
        seq: int | None = None,
        final: bool = False,
    ) -> asyncio.Future[int]:
        """Enqueue a native chunk and return its durable confirmation future."""
        confirmation, admission = _native_enqueue_chunk(
            self, chunk, seq=seq, final=final
        )
        if admission is not None:
            try:
                await admission
            except BaseException:
                confirmation.cancel()
                raise
        return confirmation


attach_protocol(ChunkStoreWriter, _ChunkStoreWriterProtocol)
ChunkStoreWriter.__module__ = __name__

__all__ = ["ChunkStoreWriter", "ChunkStoreWriterOptions"]
