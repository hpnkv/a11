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

## ChunkStoreReader

::: a11.stores.chunk_store_reader.ChunkStoreReader

::: a11.stores.chunk_store_reader.ChunkStoreReaderOptions

## ChunkStoreWriter

::: a11.stores.chunk_store_writer.ChunkStoreWriter

::: a11.stores.chunk_store_writer.ChunkStoreWriterOptions
