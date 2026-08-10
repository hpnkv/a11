import asyncio
import contextlib
import os
import pathlib
import sqlite3
from typing import Any, Iterator, Sequence

from absl import logging

from a11 import timing
from a11.data.serialization import get_global_serialization_registry
from a11.data.types import Chunk
from a11.nodes.async_node import AsyncNode
from a11.sdk import llm
from a11.status import Status, StatusCode
from a11.stores import chunk_store
from a11.stores import chunk_store_reader
from a11.stores import sqlite_chunk_store

TITLE_LIMIT = 60
_UNTITLED = "Untitled"


def _now_millis() -> int:
    return timing.now().nanoseconds_since_epoch // 1_000_000


def _from_chunk(
    chunk: Chunk,
    mimetype_patterns: str | Sequence[str] = "",
    obj_type: type | None = None,
) -> Any:
    return get_global_serialization_registry().from_chunk(
        chunk, mimetype_patterns, obj_type
    )


def _chunk_text(value: Any) -> str:
    """Best-effort text of one decoded content chunk.

    Every backend wraps its own provider payload in here, so this reads the
    shapes rather than the backend: a bare string, ``{"text": ...}``, or the
    ``{"role": ..., "content": [{"type": "text", "text": ...}]}`` envelope that
    the client and the Claude/Gemini backends both produce. Anything else --
    tool-use blocks, images -- contributes nothing, which is what we want for a
    title. ``llm.normalize_interaction`` is the principled route but needs a
    backend tag, and the client's own user interaction does not carry one.
    """
    if isinstance(value, str):
        return value
    if not isinstance(value, dict):
        return ""
    blocks = value.get("content")
    if isinstance(blocks, str):
        return blocks
    if isinstance(blocks, list):
        return "".join(
            block["text"]
            for block in blocks
            if isinstance(block, dict) and isinstance(block.get("text"), str)
        )
    text = value.get("text")
    return text if isinstance(text, str) else ""


def default_root() -> pathlib.Path:
    if override := os.environ.get("A11_CHUNK_STORE_DIR", ""):
        return pathlib.Path(override).expanduser()
    cache = os.environ.get("XDG_CACHE_HOME", "") or "~/.cache"
    return (
        pathlib.Path(cache).expanduser() / "a11" / "gateway" / "conversations"
    )


def interaction_text(interaction: llm.Interaction) -> str:
    """Best-effort human-readable text of an interaction's content."""
    parts: list[str] = []
    for chunk in interaction.content:
        try:
            parts.append(_chunk_text(_from_chunk(chunk)))
        except Exception:
            logging.debug("undecodable content chunk", exc_info=True)
    return "".join(parts)


def make_title(interaction: llm.Interaction) -> str:
    """A one-line title for the conversation this interaction opens."""
    text = " ".join(interaction_text(interaction).split())
    if not text:
        return _UNTITLED
    if len(text) <= TITLE_LIMIT:
        return text
    return text[: TITLE_LIMIT - 1].rstrip() + "…"


class ConversationIndex:
    """The conversation list: a derived ``sqlite3`` table beside the nodes.

    Connections are opened per call. The queries are single-row and the write
    rate is one upsert per chat turn, so there is nothing to pool, and a
    short-lived connection is the simplest thing that stays correct when the
    backend is restarted underneath it.
    """

    def __init__(self, path: pathlib.Path) -> None:
        self._path = path
        self._path.parent.mkdir(parents=True, exist_ok=True)
        with self._open() as db:
            # WAL so a second IDE instance reading the list never blocks the one
            # recording a turn.
            db.execute("PRAGMA journal_mode=WAL")
            # `project` is unused for now: the conversation list is global.
            # It is here so scoping the list to a project later is a query
            # change rather than a migration.
            db.execute("""
                CREATE TABLE IF NOT EXISTS conversations (
                    id         TEXT PRIMARY KEY,
                    title      TEXT NOT NULL,
                    started_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL,
                    project    TEXT NOT NULL DEFAULT ''
                )
                """)

    @contextlib.contextmanager
    def _open(self) -> Iterator[sqlite3.Connection]:
        db = sqlite3.connect(self._path, timeout=5.0)
        db.row_factory = sqlite3.Row
        try:
            with db:  # commits on clean exit, rolls back on exception
                yield db
        finally:
            db.close()

    def upsert(
        self, conversation_id: str, title: str, when_millis: int
    ) -> None:
        """Record or refresh one conversation, keeping its start time."""
        with self._open() as db:
            db.execute(
                """
                INSERT INTO conversations (id, title, started_at, updated_at)
                VALUES (?, ?, ?, ?)
                    ON CONFLICT(id) DO UPDATE SET
                    title = excluded.title,
                                           updated_at = excluded.updated_at
                """,
                (conversation_id, title, when_millis, when_millis),
            )

    def list(self) -> list[dict[str, Any]]:
        """Every conversation, most recently active first."""
        with self._open() as db:
            rows = db.execute(
                "SELECT id, title, started_at FROM conversations"
                " ORDER BY updated_at DESC"
            ).fetchall()
        return [
            {
                "id": row["id"],
                "title": row["title"],
                "started_at": row["started_at"],
            }
            for row in rows
        ]


class ConversationStore:
    """Append-only conversation storage: SQLite-backed nodes plus the index."""

    def __init__(self, root: pathlib.Path | None = None) -> None:
        self.root = pathlib.Path(root or default_root()).expanduser()
        self.root.mkdir(parents=True, exist_ok=True)
        self.index = ConversationIndex(self.root / "conversations.sqlite")
        self._chunk_root = str(self.root / "conversations")
        self._factory = sqlite_chunk_store.SQLiteChunkStoreFactory(
            self._chunk_root
        )
        self._locks: dict[str, asyncio.Lock] = {}

    def _lock(self, conversation_id: str) -> asyncio.Lock:
        return self._locks.setdefault(conversation_id, asyncio.Lock())

    async def record(
        self, interactions: Sequence[llm.Interaction]
    ) -> str | None:
        """Append whatever of ``interactions`` this conversation does not have.

        The client replays its whole history every turn, so this is handed the
        conversation from the top each time and appends by interaction id. That
        also makes it idempotent under the client's retry-once-on-failure path:
        a turn recorded twice adds nothing the second time.

        Returns the conversation id, or None when there was nothing to record.
        """
        if not interactions:
            return None
        conversation_id = interactions[0].id
        if not conversation_id:
            logging.warning(
                "conversation's first interaction has no id; not recorded"
            )
            return None

        async with self._lock(conversation_id):
            stored = await self.read(conversation_id)
            known = {interaction.id for interaction in stored}
            fresh = [i for i in interactions if i.id not in known]
            if fresh:
                node = AsyncNode.create(
                    conversation_id, chunk_store_factory=self._factory
                )
                for interaction in fresh:
                    # Awaiting the returned future is what waits for the
                    # store to accept the fragment, rather than merely admit
                    # it -- so it is durable before `record` returns.
                    await (
                        await node.put(interaction, mimetype="application/json")
                    )
            first = stored[0] if stored else interactions[0]
            await asyncio.to_thread(
                self.index.upsert,
                conversation_id,
                make_title(first),
                _now_millis(),
            )
        return conversation_id

    async def read(self, conversation_id: str) -> list[llm.Interaction]:
        """Every interaction of a conversation, in order; empty if unknown.

        An unknown id is not an error: the client may hold the id of a
        conversation whose cache has been cleared.
        """
        store = sqlite_chunk_store.SQLiteChunkStore(
            conversation_id, self._chunk_root
        )
        size = await store.size()
        interactions: list[llm.Interaction] = []
        for seq in range(size):
            fragment = await store.get(seq)
            chunk = fragment.get_chunk()
            # A conversation node is never sealed with a final fragment, but be
            # tolerant of one: a null chunk is a stream terminator, not a value.
            if chunk is None or chunk.is_null():
                continue
            try:
                interactions.append(
                    _from_chunk(chunk, obj_type=llm.Interaction)
                )
            except Exception:
                logging.warning(
                    "conversation %s: undecodable fragment at seq %d",
                    conversation_id,
                    seq,
                    exc_info=True,
                )
        return interactions

    async def list(self) -> list[dict[str, Any]]:
        """The conversation list, newest first: ``{id, title, started_at}``."""
        return await asyncio.to_thread(self.index.list)


_STORE: ConversationStore | None = None


def get_conversation_store(path: str | pathlib.Path = "") -> ConversationStore:
    """The process-wide store, made on first use.

    One gateway process serves every client, and a conversation is identified
    the same way for all of them, so the store is global. Asking for a
    *different* root once it exists is an error rather than a second store: two
    stores over two roots would answer the same conversation id differently.
    """
    global _STORE
    root = pathlib.Path(path or default_root()).expanduser()

    if _STORE is not None:
        if path and _STORE.root != root:
            raise Status(
                code=StatusCode.ALREADY_EXISTS,
                message=(
                    "The global conversation store already exists at"
                    f" {_STORE.root}"
                ),
            ).to_exception()
        return _STORE

    _STORE = ConversationStore(root)
    return _STORE


async def read_interactions(
    store: chunk_store.ChunkStore, timeout: timing.Duration | None = None
) -> list[llm.Interaction]:
    """Every interaction on a *terminated* stream, without consuming it.

    An explicit ``offset`` gives the reader its own view, so this never touches
    the store's persistent cursor -- the one that ``AsyncNode.next()`` shares
    with every other reader of the node. Used to harvest an action's ports after
    the handler has closed them; the trailing null fragment that terminates such
    a port is a marker, not a value, so it is skipped.
    """
    reader = chunk_store_reader.ChunkStoreReader(
        store, {"offset": 0, "pop_chunks": False}
    )
    deadline = timeout or timing.infinite_duration()
    interactions: list[llm.Interaction] = []
    while (fragment := await reader.next(deadline)) is not None:
        chunk = fragment.get_chunk()
        if chunk is None or chunk.is_null():
            continue
        try:
            interactions.append(_from_chunk(chunk, obj_type=llm.Interaction))
        except Exception:
            logging.warning(
                "skipping undecodable interaction fragment", exc_info=True
            )
    return interactions
