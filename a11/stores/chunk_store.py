"""A11's pluggable storage interface for streamed data.

A `ChunkStore` is where the data of an
[AsyncNode][a11.nodes.async_node.AsyncNode] actually lives: an ordered,
appendable
log of [NodeFragment][a11.data.types.NodeFragment] values that writers append
to and
readers pull from, keyed by sequence number and by arrival order. Everything
above it -- nodes, readers, writers, actions -- is written against this
interface, so the *storage backend* is a pluggable detail.

`ChunkStore` is therefore a deliberate **extension point**. The default
[LocalChunkStore][a11.stores.local_chunk_store.LocalChunkStore] keeps data in
memory, but
you can implement the interface (or subclass ``LocalChunkStore``) to persist a
stream to disk, a database, or a blob store, to add fault injection in tests, or
to enforce a custom retention policy -- without changing any node, action, or
session code. A `ChunkStoreFactory` (``node_id -> ChunkStore``) is how you
tell a [NodeMap][a11.nodes.async_node.NodeMap] or
[AsyncNode.create][a11.nodes.async_node.AsyncNode.create] which
implementation to build.

The class exported here is the native ``a11._native.ChunkStore`` base; its
methods are documented on the C++ binding. Subclass overrides written in Python
stay virtual when the store is handed back into native readers, writers, nodes,
and sessions.
"""

from collections.abc import Callable

from a11 import _native
from a11.data.types import NameString

from a11._native import ChunkStore

ChunkStore.__module__ = __name__

#: A callable ``node_id -> ChunkStore`` used to build a store for a node.
ChunkStoreFactory = Callable[[NameString], ChunkStore]

#: Attribute a pure Python facade sets to name the native store it forwards to.
NATIVE_STORE_ATTRIBUTE = "_a11_native_chunk_store"


def native_chunk_store(store: ChunkStore) -> ChunkStore:
    """The native store behind ``store``, or ``store`` itself.

    A facade that only forwards -- an unsubclassed
    [LocalChunkStore][a11.stores.local_chunk_store.LocalChunkStore] -- is
    indistinguishable from the store it wraps except in cost, and the cost is
    large: a native reader or writer calling such a store re-enters Python for
    every fragment, so each read and each write becomes an event-loop turn and
    none of the inline fast paths in `ChunkStoreReader` or `ChunkStoreWriter`
    can fire. Handing native code the store underneath removes the round trip.

    A facade only advertises itself when nothing is overriding it, so a
    subclass with Python behaviour still goes through the trampoline and stays
    virtual.
    """
    return getattr(store, NATIVE_STORE_ATTRIBUTE, None) or store


__all__ = ["ChunkStore", "ChunkStoreFactory", "native_chunk_store"]
