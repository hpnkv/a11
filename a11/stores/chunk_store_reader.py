"""Async Python protocol for the native chunk-store reader."""

from typing import Any

from a11 import _native, timing
from a11._native_options import install_native_options
from a11.data import types
from a11.status import Status, StatusCode
from a11.stores.chunk_store import ChunkStore

ChunkStoreReaderOptions = install_native_options(
    _native.ChunkStoreReaderOptions,
    {
        "ordered": (bool, True),
        "pop_chunks": (bool, False),
        "num_chunks_to_buffer": (int, 32),
        "offset": (int, 0),
        "max_chunks_to_read": (int | None, None),
    },
)
ChunkStoreReaderOptions.__module__ = __name__

ChunkStoreReader = _native.ChunkStoreReader
_native_init = ChunkStoreReader.__init__
_native_next = ChunkStoreReader.next


def _init(
    reader: ChunkStoreReader,
    store: ChunkStore,
    options: ChunkStoreReaderOptions | dict[str, Any] | None = None,
) -> None:
    if options is None:
        options = ChunkStoreReaderOptions()
    elif not isinstance(options, ChunkStoreReaderOptions):
        options = ChunkStoreReaderOptions.model_validate(options)
    _native_init(reader, store, options)


def _next(
    reader: ChunkStoreReader,
    timeout: timing.Duration = timing.infinite_duration(),
):
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
    return _native_next(reader, timeout)


async def _anext(reader: ChunkStoreReader) -> types.NodeFragment:
    fragment = await _next(reader)
    if fragment is None:
        raise StopAsyncIteration
    return fragment


def _aiter(reader: ChunkStoreReader) -> ChunkStoreReader:
    return reader


ChunkStoreReader.__init__ = _init
ChunkStoreReader.__module__ = __name__
ChunkStoreReader.next = _next
ChunkStoreReader.__aiter__ = _aiter
ChunkStoreReader.__anext__ = _anext

__all__ = ["ChunkStoreReader", "ChunkStoreReaderOptions"]
