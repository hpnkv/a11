"""Async Python protocol for the native chunk-store writer."""

from __future__ import annotations

import asyncio
from typing import Any

from a11 import _native
from a11._native_options import install_native_options
from a11.data import types
from a11.status import Status, StatusCode
from a11.stores.chunk_store import ChunkStore

ChunkStoreWriterOptions = install_native_options(
    _native.ChunkStoreWriterOptions,
    {
        "offset": (int, 0),
        "max_chunks_to_write_at_once": (int, 8),
        "num_chunks_to_buffer": (int | None, None),
    },
)
ChunkStoreWriterOptions.__module__ = __name__


ChunkStoreWriter = _native.ChunkStoreWriter
_native_init = ChunkStoreWriter.__init__
_native_enqueue_chunk = ChunkStoreWriter.enqueue_chunk


def _init(
    writer: ChunkStoreWriter,
    chunk_store: ChunkStore,
    options: ChunkStoreWriterOptions | dict[str, Any] | None = None,
) -> None:
    if options is None:
        options = ChunkStoreWriterOptions()
    elif not isinstance(options, ChunkStoreWriterOptions):
        options = ChunkStoreWriterOptions.model_validate(options)
    _native_init(writer, chunk_store, options)


async def _put(
    writer: ChunkStoreWriter,
    obj: Any,
    seq: int | None = None,
    final: bool = False,
) -> asyncio.Future[int]:
    if not isinstance(obj, types.Chunk):
        raise Status(
            code=StatusCode.UNIMPLEMENTED,
            message=(
                "ChunkStoreWriter.put is not implemented for generic objects."
            ),
        ).to_exception()
    return await _put_chunk(writer, obj, seq=seq, final=final)


async def _put_chunk(
    writer: ChunkStoreWriter,
    chunk: types.Chunk,
    seq: int | None = None,
    final: bool = False,
) -> asyncio.Future[int]:
    confirmation, admission = _native_enqueue_chunk(
        writer, chunk, seq=seq, final=final
    )
    if admission is not None:
        try:
            await admission
        except BaseException:
            confirmation.cancel()
            raise
    return confirmation


ChunkStoreWriter.__init__ = _init
ChunkStoreWriter.__module__ = __name__
ChunkStoreWriter.put = _put
ChunkStoreWriter.put_chunk = _put_chunk

__all__ = ["ChunkStoreWriter", "ChunkStoreWriterOptions"]
