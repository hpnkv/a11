# Stores

A [`ChunkStore`][a11.stores.chunk_store.ChunkStore] is the ordered log behind a
node, and a deliberate
[extension point](../principles.md#chunkstore-where-stream-data-lives) for custom
storage. Readers and writers are its cursors.

## ChunkStore

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

::: a11.stores.chunk_store_reader.ChunkStoreReader

::: a11.stores.chunk_store_reader.ChunkStoreReaderOptions

## ChunkStoreWriter

::: a11.stores.chunk_store_writer.ChunkStoreWriter

::: a11.stores.chunk_store_writer.ChunkStoreWriterOptions
