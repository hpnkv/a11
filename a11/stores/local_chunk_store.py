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
        return await self._impl.get(seq, deadline)

    async def get_by_arrival_order(
        self,
        arrival_order: int,
        deadline: timing.Time | None = timing.infinite_future(),
    ) -> types.NodeFragment:
        return await self._impl.get_by_arrival_order(arrival_order, deadline)

    async def next(
        self,
        deadline: timing.Time | None = timing.infinite_future(),
        limit: int = 1,
    ) -> list[types.NodeFragment | None]:
        return await self._impl.next(deadline, limit)

    async def put(self, fragment: types.NodeFragment) -> int:
        return await self._impl.put(fragment)

    async def put_many(
        self, fragments: Sequence[types.NodeFragment]
    ) -> list[int]:
        return await self._impl.put_many(list(fragments))

    async def clear_data(self, seq: int) -> types.NodeFragment:
        return await self._impl.clear_data(seq)

    async def get_seq_for_arrival_order(self, arrival_order: int) -> int:
        return await self._impl.get_seq_for_arrival_order(arrival_order)

    async def get_final_seq(self) -> int | None:
        return await self._impl.get_final_seq()

    async def close_writes_with_status(
        self,
        status: Status,
        return_status_if_already_closed: bool = False,
    ) -> Status:
        returned = await self._impl.close_writes_with_status(
            status, return_status_if_already_closed
        )
        self._status = returned
        return returned

    async def size(self) -> int:
        return await self._impl.size()

    def get_id(self) -> types.NameString:
        return self._impl.get_id()

    def get_impl(self) -> _native.LocalChunkStore:
        """Return the owned native implementation."""

        return self._impl


__all__ = ["LocalChunkStore"]
