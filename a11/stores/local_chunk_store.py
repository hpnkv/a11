"""Subclass-friendly facade over the native in-memory chunk store."""

from collections.abc import Sequence

from a11 import _native, timing
from a11.data import types
from a11.status import Status


class LocalChunkStore(_native.ChunkStore):
    """An in-memory store whose state and synchronization live in C++.

    The small forwarding layer keeps Python overrides virtual when a subclass is
    passed back into native readers, writers, nodes, or sessions.
    """

    def __init__(self, node_id: types.NameString):
        super().__init__()
        self._impl = _native.LocalChunkStore(node_id)
        self._status: Status | None = None

    async def get(
        self,
        seq: int,
        deadline: timing.Time | None = timing.infinite_future(),
    ) -> types.NodeFragment:
        """Wait for and return the fragment at sequence number ``seq``.

        Use a [ChunkStoreReader][a11.stores.chunk_store_reader.ChunkStoreReader]
        for ordinary sequential consumption; direct lookup is useful for
        replay, inspection, and custom retention logic.
        """
        return await self._impl.get(seq, deadline)

    async def get_by_arrival_order(
        self,
        arrival_order: int,
        deadline: timing.Time | None = timing.infinite_future(),
    ) -> types.NodeFragment:
        """Wait for a fragment by its zero-based ingestion order.

        Arrival order can differ from sequence order when fragments reach a
        store out of order.
        """
        return await self._impl.get_by_arrival_order(arrival_order, deadline)

    async def next(
        self,
        deadline: timing.Time | None = timing.infinite_future(),
        limit: int = 1,
    ) -> list[types.NodeFragment | None]:
        """Read from the store's shared logical-sequence cursor.

        The cursor advances through sequence numbers 0, 1, 2, and so on,
        waiting at gaps. ``None`` is the clean end sentinel, not a placeholder
        for a missing fragment. Use `get_by_arrival_order` for ingestion order;
        most agent code should let ``ChunkStoreReader`` manage this cursor,
        buffering, ordering, and end-of-stream handling.
        """
        return await self._impl.next(deadline, limit)

    async def put(self, fragment: types.NodeFragment) -> int:
        """Append one fragment and return its accepted sequence number."""
        return await self._impl.put(fragment)

    async def put_many(
        self, fragments: Sequence[types.NodeFragment]
    ) -> list[int]:
        """Append a batch and return sequence numbers in matching order."""
        return await self._impl.put_many(list(fragments))

    async def clear_data(self, seq: int) -> types.NodeFragment:
        """Discard one stored payload while retaining its sequence slot.

        This supports ``pop_chunks`` readers and retention policies without
        changing the ordering metadata seen by other readers.
        """
        return await self._impl.clear_data(seq)

    async def get_seq_for_arrival_order(self, arrival_order: int) -> int:
        """Translate a zero-based ingestion position to its sequence number."""
        return await self._impl.get_seq_for_arrival_order(arrival_order)

    async def get_final_seq(self) -> int | None:
        """Return the logical final sequence, if a final fragment was written.

        Finality and closure are independent: closing writes does not create a
        final sequence, and writing a final fragment does not close the store.
        """
        return await self._impl.get_final_seq()

    async def close_writes_with_status(
        self,
        status: Status,
        return_status_if_already_closed: bool = False,
    ) -> Status:
        """Seal writes with ``status`` and wake blocked readers.

        This records whether production completed or failed, but it does not
        mark any fragment as the final data value. Producers that need semantic
        finality should write a fragment with ``continued=False`` first (most
        commonly through ``AsyncNode.put_final`` or ``put_null_final``).
        """
        returned = await self._impl.close_writes_with_status(
            status, return_status_if_already_closed
        )
        self._status = returned
        return returned

    async def size(self) -> int:
        """Return the number of fragment slots currently held in memory."""
        return await self._impl.size()

    def get_id(self) -> types.NameString:
        """Return the node id whose fragment log this store backs."""
        return self._impl.get_id()

    def get_impl(self) -> _native.LocalChunkStore:
        """Return the owned native implementation."""

        return self._impl


__all__ = ["LocalChunkStore"]
