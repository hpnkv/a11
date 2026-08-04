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

## LocalChunkStore

::: a11.stores.local_chunk_store.LocalChunkStore

## RedisChunkStore

::: a11.stores.redis_chunk_store.RedisChunkStore

::: a11.stores.redis_chunk_store.RedisChunkStoreOptions

::: a11.stores.redis_chunk_store.RedisChunkStoreMetadata

::: a11.stores.redis_chunk_store.RedisChunkStoreKeys

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
at producer shutdown.

::: a11.stores.chunk_store_writer.ChunkStoreWriter

::: a11.stores.chunk_store_writer.ChunkStoreWriterOptions
