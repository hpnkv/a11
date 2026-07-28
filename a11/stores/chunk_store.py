"""Abstract chunk-store interface implemented by the native runtime."""

from collections.abc import Callable

from a11 import _native
from a11.data.types import NameString

ChunkStore = _native.ChunkStore
ChunkStore.__module__ = __name__

ChunkStoreFactory = Callable[[NameString], ChunkStore]

__all__ = ["ChunkStore", "ChunkStoreFactory"]
