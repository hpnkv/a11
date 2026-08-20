# Stores

A [`ChunkStore`][a11.stores.chunk_store.ChunkStore] is the ordered log behind a
node, and a deliberate
[extension point](../principles.md#chunkstore-where-stream-data-lives) for custom
storage. Readers and writers are its cursors.

## ChunkStore

The main store operations are intentionally low-level:
[`put`][a11.stores.chunk_store.ChunkStore.put] appends a fragment,
[`get`][a11.stores.chunk_store.ChunkStore.get] waits at a sequence position,
and
[`close_writes_with_status`][a11.stores.chunk_store.ChunkStore.close_writes_with_status]
publishes the terminal state to every reader.

::: a11.stores.chunk_store.ChunkStore

::: a11.stores.chunk_store.ChunkStoreFactory

::: a11.stores.chunk_store.native_chunk_store

## LocalChunkStore

::: a11.stores.local_chunk_store.LocalChunkStore

## RedisChunkStore

::: a11.stores.redis_chunk_store.RedisChunkStore

::: a11.stores.redis_chunk_store.RedisChunkStoreOptions

::: a11.stores.redis_chunk_store.RedisChunkStoreMetadata

::: a11.stores.redis_chunk_store.RedisChunkStoreKeys

## SQLiteChunkStore

Durable storage without a server: fragments live in one SQLite database per
storage root, with payloads above 128 KiB moved into adjacent blob files. It is
also the only backend that accepts `NodeRef` payloads, storing the reference
target as indexed columns so traversal between nodes is a query.

The [SQLite page](sqlite.md) covers the on-disk layout, durability, and
configuration, and is where
[`SQLiteChunkStore`][a11.stores.sqlite_chunk_store.SQLiteChunkStore],
[`SQLiteChunkStoreFactory`][a11.stores.sqlite_chunk_store.SQLiteChunkStoreFactory],
[`SQLiteChunkStoreOptions`][a11.stores.sqlite_chunk_store.SQLiteChunkStoreOptions],
and
[`SQLiteChunkStoreMetadata`][a11.stores.sqlite_chunk_store.SQLiteChunkStoreMetadata]
are documented.

## ChunkStoreReader

Create a reader at an offset and use
[`next`][a11.stores.chunk_store_reader.ChunkStoreReader.next] for one record or
async iteration to drain its configured range.

::: a11.stores.chunk_store_reader.ChunkStoreReader

::: a11.stores.chunk_store_reader.ChunkStoreReaderOptions

## ChunkStoreWriter

[`put_chunk`][a11.stores.chunk_store_writer.ChunkStoreWriter.put_chunk] returns
a confirmation future after bounded admission. Await that future when the
application must checkpoint only after storage accepts the chunk, then call
[`drain_and_close`][a11.stores.chunk_store_writer.ChunkStoreWriter.drain_and_close]
at producer shutdown. This is the storage-level cursor; application code reaches
it through a node, where [`finalize`][a11.nodes.async_node.AsyncNode.finalize]
marks the logical end of the data and closes in one call. Closing also tees a
closure marker to every attached stream, so a peer mirroring this node closes
its own write half — see
[the node lifecycle](../lifecycles/async-node.md#4-close-writes).

::: a11.stores.chunk_store_writer.ChunkStoreWriter

::: a11.stores.chunk_store_writer.ChunkStoreWriterOptions
