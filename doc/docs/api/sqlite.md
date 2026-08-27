# SQLite

[`SQLiteChunkStore`][a11.stores.sqlite_chunk_store.SQLiteChunkStore] is a
durable [`ChunkStore`][a11.stores.chunk_store.ChunkStore] that needs no server.
It sits between the two existing backends: `LocalChunkStore` is fast but loses
everything at exit, and `RedisChunkStore` persists but requires Redis. When a
single machine wants its node streams to survive a restart, this is the one to
reach for.

```python
from a11.stores.sqlite_chunk_store import SQLiteChunkStoreFactory

factory = SQLiteChunkStoreFactory("/var/lib/agent/chunks")
store = factory.open("agent-output")
await store.put_many(fragments)
```

Pass the factory itself wherever a `chunk_store_factory` is expected — it is
callable — to make SQLite the backing store for a whole node map or session:

```python
node = a11.AsyncNode.create("stream", chunk_store_factory=factory)
```

Omitting the root uses
[`default_root`][a11.stores.sqlite_chunk_store.SQLiteChunkStoreFactory.default_root]:
`$A11_SQLITE_CHUNK_STORE_ROOT` when set, otherwise `$XDG_CACHE_HOME/a11/chunks`,
otherwise `~/.cache/a11/chunks`.

## Storage layout

A root holds one database and one blob directory:

```
./store.sqlite
./blobs/939f2184-db19-4dd0-b949-bb31c5eadcf8
./blobs/7ee4a05e-f439-4e5f-bb97-8d1388960f29
```

Every store opened under the same root shares one database, connection set, and
worker pool. Use one factory per root to share those resources across nodes.

The `nodes` table holds a row per represented node: the shared producer and
consumer cursors, closure state and terminal status, the declared final
sequence, `owner_id`, `created_at`/`updated_at`, and cached counters such as
`size` so
[`size`][a11.stores.sqlite_chunk_store.SQLiteChunkStore.size] and
[`get_final_seq`][a11.stores.sqlite_chunk_store.SQLiteChunkStore.get_final_seq]
are single-row reads. A row is created implicitly by the first accepted write.

The `fragments` table stores fragment fields in separate columns, allowing SQL
filters by owner, timestamp, or reference target. Indexes cover
`(node_id, seq)` for sequenced reading,
`(node_id, arrival_order)` for ingestion order, `owner_id` on the node table,
and partial indexes on `node_ref_id` and on the blob reference.

A fragment's `timestamp` column is always populated — from
`ChunkMetadata.timestamp` when present, otherwise the current UTC time — so it
is a usable index key; a companion flag records whether the original metadata
actually carried one, so round-tripping stays exact.

!!! note "`continued` is derived, never stored as truth"

    The flag returned to callers is recomputed from the node's current final
    sequence on every read. A later batch can declare finality, and a stored
    flag would then disagree with the store.

## Payloads and blob files

`Chunk.data` at or below `inline_data_threshold` (128 KiB by default) stays in
the row. Anything larger is written to `blobs/` under a UUID name recorded in
the row, which keeps the page cache useful and the write-ahead log small.
Reading is transparent either way.

A blob is written to a temporary name, fsynced, renamed into place, and then
its directory is fsynced — all *before* the transaction referencing it commits,
so a committed row can never point at a payload that never reached disk.
Removals run in the other order, after the commit, so a rollback cannot take
the data with it. A crash in either window leaves an unreferenced file, which
[`sweep_orphan_blobs`][a11.stores.sqlite_chunk_store.SQLiteChunkStore.sweep_orphan_blobs]
reclaims; its grace period keeps it from deleting a blob whose transaction is
still in flight.

## Node references

This is the only backend that accepts `NodeRef` payloads — `LocalChunkStore`
and `RedisChunkStore` reject them as `UNIMPLEMENTED`. The target, offset, and
length become indexed columns, making referrer lookup a query:

```python
referrers = await store.find_referrers()
```

Because a tombstone is chunk-shaped,
[`clear_data`][a11.stores.sqlite_chunk_store.SQLiteChunkStore.clear_data]
rejects node-reference fragments to avoid returning a different payload type.

## Transactions and waiting

Every mutation runs as one `BEGIN IMMEDIATE` transaction, so a batch either
lands whole or not at all, leaving no partial rows and no stray blob files.
Contention uses bounded retries because `sqlite3_busy_timeout` would block the
calling thread inside SQLite.

Readers never poll. A getter snapshots a per-node change event, runs an
optimistic read, and parks on that event only if the fragment it wants has not
arrived; a committing writer fires the event once `COMMIT` has returned. Taking
the snapshot before the read, and firing strictly after the commit, is what
closes the lost-wakeup window in both directions.

SQLite calls run on a small dedicated thread pool. Running them on A11's fiber
workers could block deadline timers while `sqlite3_step` waits.

!!! warning "One writing process per root, by default"

    SQLite's change hooks are per-connection, so a writer in another process
    cannot wake a reader parked in this one. Multi-process readers would wait
    out their deadlines. Set `cross_process_poll_interval` to enable a
    `PRAGMA data_version` watcher when more than one process writes a root; it
    is disabled by default so the common case never polls.

## Configuration

An explicit `SQLiteChunkStoreOptions` takes the place of the environment.
Per-root settings — durability, the poll interval, the grace period — apply on
the first open of that root, since the database behind it is shared.

| Variable | Default | Meaning |
| --- | --- | --- |
| `A11_SQLITE_CHUNK_STORE_ROOT` | unset | Default storage root, overriding the cache-directory convention. |
| `A11_SQLITE_CHUNK_STORE_INLINE_DATA_THRESHOLD_BYTES` | `131072` | `Chunk.data` size above which the payload moves to a blob file. |
| `A11_SQLITE_CHUNK_STORE_OWNER_ID` | empty | Owner recorded on node rows. |
| `A11_SQLITE_CHUNK_STORE_SYNCHRONOUS` | `normal` | `off`, `normal`, or `full`; applied as `PRAGMA synchronous`. |
| `A11_SQLITE_CHUNK_STORE_CROSS_PROCESS_POLL_MS` | `0` | Interval for noticing other processes' commits; `0` disables it. |
| `A11_SQLITE_CHUNK_STORE_BLOB_GRACE_MS` | `3600000` | How long an unreferenced blob survives before a sweep may remove it. |

`normal` in WAL mode survives an application crash but may lose the newest
commits on power loss. Use `full` when that matters.

Ownership has no enforcement semantics yet; `owner_id` exists so nodes can be
attributed and filtered.

SQLite is compiled into the extension from the upstream amalgamation with
hidden visibility, so it cannot collide with the system `libsqlite3` that
CPython's own `_sqlite3` module loads into the same process. The build option
is `A11_BUILD_SQLITE`.

::: a11.stores.sqlite_chunk_store.SQLiteChunkStore

::: a11.stores.sqlite_chunk_store.SQLiteChunkStoreFactory

::: a11.stores.sqlite_chunk_store.SQLiteChunkStoreOptions

::: a11.stores.sqlite_chunk_store.SQLiteChunkStoreMetadata
