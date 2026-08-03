"""Persistent, multi-process chunk streams backed by Redis Streams.

`RedisChunkStore` implements the same asynchronous contract as
[LocalChunkStore][a11.stores.local_chunk_store.LocalChunkStore], while keeping
node metadata directly addressable and moving large encoded chunks into a
separate blob key. Each node's keys share one Redis Cluster hash tag, and every
write, close, and tombstone transition is committed by one Lua state-machine
invocation.
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

from a11 import timing
from a11._native_options import install_native_options
from a11._native_protocol import attach_protocol
from a11.data import types
from a11.redis.client import RedisClient
from a11.status import Status, StatusCode

from a11._native import RedisChunkStore
from a11._native import RedisChunkStoreKeys
from a11._native import RedisChunkStoreMetadata
from a11._native import RedisChunkStoreOptions

install_native_options(
    RedisChunkStoreOptions,
    {
        "key_prefix": (str, "a11:"),
        "inline_data_threshold": (int, 256 * 1024),
    },
)

_MAX_UINT32 = (1 << 32) - 1
_MAX_UINT64 = (1 << 64) - 1

_native_init = RedisChunkStore.__init__
_native_get = RedisChunkStore.get
_native_get_by_arrival_order = RedisChunkStore.get_by_arrival_order
_native_next = RedisChunkStore.next
_native_put = RedisChunkStore.put
_native_put_many = RedisChunkStore.put_many
_native_clear_data = RedisChunkStore.clear_data
_native_get_seq_for_arrival_order = RedisChunkStore.get_seq_for_arrival_order
_native_get_final_seq = RedisChunkStore.get_final_seq
_native_close_writes_with_status = RedisChunkStore.close_writes_with_status
_native_size = RedisChunkStore.size
_native_initialize = RedisChunkStore.initialize
_native_get_metadata = RedisChunkStore.get_metadata
_native_metadata_status = RedisChunkStoreMetadata.status


def _unsigned(value: object, name: str, maximum: int) -> int:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or value < 0
        or value > maximum
    ):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"{name} must be an integer between 0 and {maximum}.",
        ).to_exception()
    return value


def _fragment(value: object, name: str = "fragment") -> types.NodeFragment:
    if not isinstance(value, types.NodeFragment):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"{name} must be a NodeFragment.",
        ).to_exception()
    return value


class _RedisChunkStoreProtocol:
    """Typed asyncio protocol over the native Redis store operations."""

    def __init__(
        self,
        id: str,
        client: RedisClient | None = None,
        options: RedisChunkStoreOptions | dict[str, Any] | None = None,
    ) -> None:
        """Open one node's persistent stream with an injected Redis client."""
        if not isinstance(id, str):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="id must be a string.",
            ).to_exception()
        if client is not None and not isinstance(client, RedisClient):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="client must be a RedisClient or None.",
            ).to_exception()
        if options is not None and not isinstance(
            options, RedisChunkStoreOptions
        ):
            options = RedisChunkStoreOptions.model_validate(options)
        _native_init(self, id, client, options)

    @staticmethod
    def create(
        id: str,
        client: RedisClient | None = None,
        options: RedisChunkStoreOptions | dict[str, Any] | None = None,
    ) -> RedisChunkStore:
        """Create a Redis store with optional client and options."""
        return RedisChunkStore(id, client, options)

    async def get(
        self,
        seq: int,
        deadline: timing.Time | None = None,
    ) -> types.NodeFragment:
        return await _native_get(
            self, _unsigned(seq, "seq", _MAX_UINT32), deadline
        )

    async def get_by_arrival_order(
        self,
        arrival_order: int,
        deadline: timing.Time | None = None,
    ) -> types.NodeFragment:
        return await _native_get_by_arrival_order(
            self,
            _unsigned(arrival_order, "arrival_order", _MAX_UINT64),
            deadline,
        )

    async def next(
        self,
        deadline: timing.Time | None = None,
        limit: int = 1,
    ) -> list[types.NodeFragment | None]:
        converted_limit = _unsigned(limit, "limit", _MAX_UINT64)
        if converted_limit == 0:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="limit must be positive.",
            ).to_exception()
        return await _native_next(self, deadline, converted_limit)

    async def put(self, fragment: types.NodeFragment) -> int:
        return await _native_put(self, _fragment(fragment))

    async def put_many(
        self, fragments: Sequence[types.NodeFragment]
    ) -> list[int]:
        try:
            values = list(fragments)
        except Exception as error:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"fragments must be a sequence: {error}",
            ).to_exception() from None
        for index, value in enumerate(values):
            _fragment(value, f"fragments[{index}]")
        return await _native_put_many(self, values)

    async def clear_data(self, seq: int) -> types.NodeFragment:
        return await _native_clear_data(
            self, _unsigned(seq, "seq", _MAX_UINT32)
        )

    async def get_seq_for_arrival_order(self, arrival_order: int) -> int:
        return await _native_get_seq_for_arrival_order(
            self,
            _unsigned(arrival_order, "arrival_order", _MAX_UINT64),
        )

    async def get_final_seq(self) -> int | None:
        return await _native_get_final_seq(self)

    async def close_writes_with_status(
        self,
        status: Status,
        return_status_if_already_closed: bool = False,
    ) -> Status:
        if not isinstance(status, Status):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="status must be an a11.status.Status.",
            ).to_exception()
        if not isinstance(return_status_if_already_closed, bool):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="return_status_if_already_closed must be a boolean.",
            ).to_exception()
        return await _native_close_writes_with_status(
            self, status, return_status_if_already_closed
        )

    async def size(self) -> int:
        return await _native_size(self)

    async def initialize(self) -> None:
        """Ensure metadata exists without writing a chunk."""
        await _native_initialize(self)

    async def get_metadata(self) -> RedisChunkStoreMetadata:
        """Read node state without walking the chunk stream."""
        return await _native_get_metadata(self)


class _RedisChunkStoreMetadataProtocol:
    """Typed convenience properties for node-level Redis state."""

    @property
    def status(self) -> Status | None:
        """Return the terminal status when closed, otherwise ``None``."""
        return _native_metadata_status.__get__(self)


attach_protocol(RedisChunkStore, _RedisChunkStoreProtocol)
attach_protocol(RedisChunkStoreMetadata, _RedisChunkStoreMetadataProtocol)

for _class in (
    RedisChunkStore,
    RedisChunkStoreKeys,
    RedisChunkStoreMetadata,
    RedisChunkStoreOptions,
):
    _class.__module__ = __name__


__all__ = [
    "RedisChunkStore",
    "RedisChunkStoreKeys",
    "RedisChunkStoreMetadata",
    "RedisChunkStoreOptions",
    "RedisClient",
]
