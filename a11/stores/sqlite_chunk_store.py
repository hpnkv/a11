"""Durable, embedded chunk streams backed by SQLite and blob files.

`SQLiteChunkStore` implements the same asynchronous contract as
[LocalChunkStore][a11.stores.local_chunk_store.LocalChunkStore], but the
fragments survive the process. It needs no server, unlike
[RedisChunkStore][a11.stores.redis_chunk_store.RedisChunkStore], which makes it
the natural choice for a single machine that wants its node streams to outlive a
restart.

The layout under a storage root is straightforward::

    ./store.sqlite
    ./blobs/939f2184-db19-4dd0-b949-bb31c5eadcf8
    ./blobs/7ee4a05e-f439-4e5f-bb97-8d1388960f29

Every mutation is one immediate transaction, so a batch either lands whole or
not at all, and readers never poll: they park on a per-node event that a
committing writer fires.

Because the store is relational, it is the only backend that accepts
`NodeRef` payloads. Their target, offset, and length become indexed columns, so
finding what refers to a node is a query rather than a walk -- see
`find_referrers`.

Stores sharing a root share one database, one set of connections, and one set of
worker threads, so opening a thousand nodes under one root is cheap. Use
`SQLiteChunkStoreFactory` to make that sharing explicit and to pass SQLite
storage to a `NodeMap` or `AsyncNode` as a ``chunk_store_factory``.
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

from a11 import timing
from a11._native_options import install_native_options
from a11._native_protocol import attach_protocol
from a11.data import types
from a11.status import Status, StatusCode

from a11._native import SQLiteChunkStore
from a11._native import SQLiteChunkStoreFactory
from a11._native import SQLiteChunkStoreMetadata
from a11._native import SQLiteChunkStoreOptions
from a11._native import SQLiteSynchronous

install_native_options(
    SQLiteChunkStoreOptions,
    {
        "inline_data_threshold": (int, 128 * 1024),
        "owner_id": (str, ""),
    },
)

_MAX_UINT32 = (1 << 32) - 1
_MAX_UINT64 = (1 << 64) - 1

_native_init = SQLiteChunkStore.__init__
_native_get = SQLiteChunkStore.get
_native_get_by_arrival_order = SQLiteChunkStore.get_by_arrival_order
_native_next = SQLiteChunkStore.next
_native_put = SQLiteChunkStore.put
_native_put_many = SQLiteChunkStore.put_many
_native_clear_data = SQLiteChunkStore.clear_data
_native_get_seq_for_arrival_order = SQLiteChunkStore.get_seq_for_arrival_order
_native_get_final_seq = SQLiteChunkStore.get_final_seq
_native_close_writes_with_status = SQLiteChunkStore.close_writes_with_status
_native_size = SQLiteChunkStore.size
_native_get_metadata = SQLiteChunkStore.get_metadata
_native_find_referrers = SQLiteChunkStore.find_referrers
_native_sweep_orphan_blobs = SQLiteChunkStore.sweep_orphan_blobs
_native_metadata_status = SQLiteChunkStoreMetadata.status

_native_factory_init = SQLiteChunkStoreFactory.__init__
_native_factory_open = SQLiteChunkStoreFactory.open
_native_factory_sweep = SQLiteChunkStoreFactory.sweep_orphan_blobs


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


def _options(
    value: SQLiteChunkStoreOptions | dict[str, Any] | None,
) -> SQLiteChunkStoreOptions | None:
    if value is None or isinstance(value, SQLiteChunkStoreOptions):
        return value
    return SQLiteChunkStoreOptions.model_validate(value)


def _root(value: object) -> str | None:
    if value is None:
        return None
    # Accept os.PathLike so callers can pass a pathlib.Path directly.
    if hasattr(value, "__fspath__"):
        value = value.__fspath__()
    if not isinstance(value, str):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="root must be a string, a path, or None.",
        ).to_exception()
    return value


class _SQLiteChunkStoreProtocol:
    """Typed asyncio protocol over the native SQLite store operations."""

    def __init__(
        self,
        id: str,
        root: str | Any | None = None,
        options: SQLiteChunkStoreOptions | dict[str, Any] | None = None,
    ) -> None:
        """Open one node's durable stream under a storage root.

        Stores for the same id and root address the same rows, so reopening
        after a restart resumes the same fragment log. Omit `root` to use
        `SQLiteChunkStoreFactory.default_root()`. Opening many nodes under one
        root is cheap; they share a single database.
        """
        if not isinstance(id, str):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="id must be a string.",
            ).to_exception()
        _native_init(self, id, _root(root), _options(options))

    @staticmethod
    def create(
        id: str,
        root: str | Any | None = None,
        options: SQLiteChunkStoreOptions | dict[str, Any] | None = None,
    ) -> SQLiteChunkStore:
        """Create a SQLite-backed fragment log for one node id."""
        return SQLiteChunkStore(id, root, options)

    async def get(
        self,
        seq: int,
        deadline: timing.Time | None = None,
    ) -> types.NodeFragment:
        """Wait for and return a fragment by sequence number.

        The wait parks on a per-node event rather than polling the database,
        and resolves early with an error once the fragment can no longer
        arrive.
        """
        return await _native_get(
            self, _unsigned(seq, "seq", _MAX_UINT32), deadline
        )

    async def get_by_arrival_order(
        self,
        arrival_order: int,
        deadline: timing.Time | None = None,
    ) -> types.NodeFragment:
        """Wait for a fragment by its zero-based ingestion order."""
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
        """Read from the persistent shared logical-sequence cursor.

        The cursor lives in the database, so it survives a restart and is
        shared by every store open on this node. It advances through sequence
        numbers and waits at gaps; ``None`` marks clean end-of-stream. Prefer
        `ChunkStoreReader` for ordinary consumption.
        """
        converted_limit = _unsigned(limit, "limit", _MAX_UINT64)
        if converted_limit == 0:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="limit must be positive.",
            ).to_exception()
        return await _native_next(self, deadline, converted_limit)

    async def put(self, fragment: types.NodeFragment) -> int:
        """Atomically append one fragment and return its sequence number."""
        return await _native_put(self, _fragment(fragment))

    async def put_many(
        self, fragments: Sequence[types.NodeFragment]
    ) -> list[int]:
        """Atomically append a batch and return its assigned sequences.

        The batch commits in one transaction: either every fragment is stored,
        or none is and no blob file is left behind.
        """
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
        """Tombstone one payload while retaining ordering metadata.

        Returns the fragment as it was. Any blob file backing it is unlinked
        once the transaction commits. Node-reference fragments cannot be
        cleared, because a tombstone is chunk-shaped.
        """
        return await _native_clear_data(
            self, _unsigned(seq, "seq", _MAX_UINT32)
        )

    async def get_seq_for_arrival_order(self, arrival_order: int) -> int:
        """Translate a zero-based ingestion position to its sequence number."""
        return await _native_get_seq_for_arrival_order(
            self,
            _unsigned(arrival_order, "arrival_order", _MAX_UINT64),
        )

    async def get_final_seq(self) -> int | None:
        """Return the logical final sequence, if one has been written.

        The final marker is independent of write closure. Closing a store does
        not synthesize it, and a final fragment does not close the store.
        """
        return await _native_get_final_seq(self)

    async def close_writes_with_status(
        self,
        status: Status,
        return_status_if_already_closed: bool = False,
    ) -> Status:
        """Atomically seal writes with a terminal status and wake readers.

        This closes the producer side but does not mark data final. Write a
        final fragment first when consumers use whole-value semantics such as
        `AsyncNode.consume`.
        """
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
        """Return the number of fragment slots, tombstones included."""
        return await _native_size(self)

    async def get_metadata(self) -> SQLiteChunkStoreMetadata:
        """Read cursors, finality, and closure state in one row read."""
        return await _native_get_metadata(self)

    async def find_referrers(
        self, limit: int = 100
    ) -> list[types.NodeFragment]:
        """Find fragments elsewhere whose `NodeRef` points at this node.

        This is the traversal the relational layout exists for: the answer
        comes from an index on the reference target, so the cost tracks the
        number of referrers rather than the size of the database.
        """
        return await _native_find_referrers(
            self, _unsigned(limit, "limit", _MAX_UINT64)
        )

    async def sweep_orphan_blobs(self) -> int:
        """Delete unreferenced blob files older than the grace period.

        A crash between writing a blob and committing its row leaves a file
        nothing points at. This reclaims those; the grace period keeps it from
        deleting a blob whose transaction is still in flight elsewhere.
        """
        return await _native_sweep_orphan_blobs(self)


class _SQLiteChunkStoreFactoryProtocol:
    """Typed protocol for the shared-per-root store factory."""

    def __init__(
        self,
        root: str | Any | None = None,
        options: SQLiteChunkStoreOptions | dict[str, Any] | None = None,
    ) -> None:
        """Create a factory rooted at a directory, created when absent.

        Pass the factory itself wherever a ``chunk_store_factory`` callable is
        expected to make SQLite the backing store for a `NodeMap`, `AsyncNode`,
        or `Session`.
        """
        _native_factory_init(self, _root(root), _options(options))

    def open(self, node_id: str) -> SQLiteChunkStore:
        """Open a store for `node_id` under this factory's root."""
        if not isinstance(node_id, str):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="node_id must be a string.",
            ).to_exception()
        return _native_factory_open(self, node_id)

    async def sweep_orphan_blobs(self) -> int:
        """Delete unreferenced blob files older than the grace period."""
        return await _native_factory_sweep(self)


class _SQLiteChunkStoreMetadataProtocol:
    """Typed convenience properties for node-level SQLite state."""

    @property
    def status(self) -> Status | None:
        """Return the terminal status when closed, otherwise ``None``."""
        return _native_metadata_status.__get__(self)


attach_protocol(SQLiteChunkStore, _SQLiteChunkStoreProtocol)
attach_protocol(SQLiteChunkStoreFactory, _SQLiteChunkStoreFactoryProtocol)
attach_protocol(SQLiteChunkStoreMetadata, _SQLiteChunkStoreMetadataProtocol)

for _class in (
    SQLiteChunkStore,
    SQLiteChunkStoreFactory,
    SQLiteChunkStoreMetadata,
    SQLiteChunkStoreOptions,
    SQLiteSynchronous,
):
    _class.__module__ = __name__

__all__ = [
    "SQLiteChunkStore",
    "SQLiteChunkStoreFactory",
    "SQLiteChunkStoreMetadata",
    "SQLiteChunkStoreOptions",
    "SQLiteSynchronous",
]
