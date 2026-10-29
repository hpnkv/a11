/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package a11

import a11.internal.NativeBridge
import a11.internal.NativeData
import a11.internal.nativeBoolean
import a11.internal.nativeFragment
import a11.internal.nativeFragments
import a11.internal.nativeText
import a11.internal.wireFields

/** Native store contract shared by in-memory, SQLite, and Redis storage. */
open class ChunkStore internal constructor(id: Long) : NativeObject(id) {
    fun getId(): StatusOr<String> = NativeBridge.call("store.id", this).map(::nativeText)

    val holdsObjects: Boolean
        get() = NativeBridge.call("store.holdsObjects", this).map(::nativeBoolean).value()

    suspend fun get(
        seq: Long,
        timeout: kotlin.time.Duration = kotlin.time.Duration.INFINITE,
    ): StatusOr<NodeFragment> =
        NativeBridge.async("store.get", this, mapOf("seq" to seq, "timeout" to timeout))
            .map(::nativeFragment)

    suspend fun get(seq: Long, timeout: Duration): StatusOr<NodeFragment> =
        NativeBridge.async("store.get", this, mapOf("seq" to seq, "timeout" to timeout))
            .map(::nativeFragment)

    suspend fun get(seq: Long, timeoutMs: Long?): StatusOr<NodeFragment> =
        NativeBridge.async("store.get", this, mapOf("seq" to seq, "timeoutMs" to timeoutMs))
            .map(::nativeFragment)

    suspend fun getByArrivalOrder(
        offset: Long,
        timeout: kotlin.time.Duration = kotlin.time.Duration.INFINITE,
    ): StatusOr<NodeFragment> =
        NativeBridge.async(
                "store.getByArrivalOrder",
                this,
                mapOf("offset" to offset, "timeout" to timeout),
            )
            .map(::nativeFragment)

    suspend fun getByArrivalOrder(offset: Long, timeout: Duration): StatusOr<NodeFragment> =
        NativeBridge.async(
                "store.getByArrivalOrder",
                this,
                mapOf("offset" to offset, "timeout" to timeout),
            )
            .map(::nativeFragment)

    suspend fun getByArrivalOrder(offset: Long, timeoutMs: Long?): StatusOr<NodeFragment> =
        NativeBridge.async(
                "store.getByArrivalOrder",
                this,
                mapOf("offset" to offset, "timeoutMs" to timeoutMs),
            )
            .map(::nativeFragment)

    suspend fun next(
        limit: Int = 1,
        timeout: kotlin.time.Duration = kotlin.time.Duration.INFINITE,
    ): StatusOr<List<NodeFragment?>> =
        NativeBridge.async("store.next", this, mapOf("limit" to limit, "timeout" to timeout))
            .map(::nativeFragments)

    suspend fun next(limit: Int = 1, timeout: Duration): StatusOr<List<NodeFragment?>> =
        NativeBridge.async("store.next", this, mapOf("limit" to limit, "timeout" to timeout))
            .map(::nativeFragments)

    suspend fun next(limit: Int = 1, timeoutMs: Long?): StatusOr<List<NodeFragment?>> =
        NativeBridge.async("store.next", this, mapOf("limit" to limit, "timeoutMs" to timeoutMs))
            .map(::nativeFragments)

    suspend fun put(fragment: NodeFragment): StatusOr<Long> {
        val pending =
            NativeData.storePut(this, fragment).orElse { status ->
                return status
            }
        return NativeBridge.awaitOwned(pending).map(NativeBridge::number)
    }

    /** Store native contents by move; [fragment] retains its C++ moved-from state. */
    suspend fun putMoved(fragment: NodeFragment): StatusOr<Long> {
        val pending =
            NativeData.storePutMoved(this, fragment).orElse { status ->
                return status
            }
        return NativeBridge.awaitOwned(pending).map(NativeBridge::number)
    }

    suspend fun putMany(fragments: List<NodeFragment>): StatusOr<List<Long>> =
        NativeBridge.async("store.putMany", this, mapOf("fragments" to fragments.map(::wireFields)))
            .map { value ->
                (value as? List<*> ?: fatal("Expected native sequence list")).map(
                    NativeBridge::number
                )
            }

    suspend fun clearData(seq: Long): StatusOr<NodeFragment> =
        NativeBridge.async("store.clearData", this, mapOf("seq" to seq)).map(::nativeFragment)

    suspend fun getSeqForArrivalOrder(offset: Long): StatusOr<Long> =
        NativeBridge.async("store.seqForArrivalOrder", this, mapOf("offset" to offset))
            .map(NativeBridge::number)

    suspend fun getFinalSeq(): StatusOr<Long?> =
        NativeBridge.async("store.finalSeq", this).map { value -> value?.let(NativeBridge::number) }

    suspend fun size(): StatusOr<Long> =
        NativeBridge.async("store.size", this).map(NativeBridge::number)

    suspend fun closeWritesWithStatus(
        status: Status,
        returnStatusIfAlreadyClosed: Boolean = false,
    ): StatusOr<Status> =
        NativeBridge.async(
                "store.closeWritesWithStatus",
                this,
                mapOf(
                    "status" to statusFields(status),
                    "returnStatusIfAlreadyClosed" to returnStatusIfAlreadyClosed,
                ),
            )
            .map(NativeBridge::statusValue)
}

/** In-memory C++ ChunkStore; reader/writer pumps and state remain native. */
class LocalChunkStore private constructor(id: Long) : ChunkStore(id) {
    companion object {
        fun create(id: String): StatusOr<LocalChunkStore> =
            NativeBridge.call("store.local", args = mapOf("id" to id)).map { value ->
                LocalChunkStore(NativeBridge.handle(value))
            }
    }
}

/** Persistent C++ store backed by SQLite and blob files. */
class SQLiteChunkStore private constructor(id: Long) : ChunkStore(id) {
    companion object {
        fun create(id: String, root: String): StatusOr<SQLiteChunkStore> =
            NativeBridge.call("store.sqlite", args = mapOf("id" to id, "root" to root)).map { value
                ->
                SQLiteChunkStore(NativeBridge.handle(value))
            }
    }
}

internal fun statusFields(status: Status): Map<String, Any?> =
    mapOf("code" to status.code.value, "message" to status.message, "details" to status.details)

/** Reader options obtain their defaults from the C++ option struct. */
data class ChunkStoreReaderOptions(
    val ordered: Boolean = readerDefaults.boolean("ordered"),
    val popChunks: Boolean = readerDefaults.boolean("pop_chunks"),
    val numChunksToBuffer: Long = readerDefaults.long("num_chunks_to_buffer"),
    val offset: Long = readerDefaults.long("offset"),
    val maxChunksToRead: Long? = null,
    val stickyMimetype: Boolean = readerDefaults.boolean("sticky_mimetype"),
) {
    internal fun fields(): Map<String, Any?> =
        mapOf(
            "ordered" to ordered,
            "pop_chunks" to popChunks,
            "num_chunks_to_buffer" to numChunksToBuffer,
            "offset" to offset,
            "max_chunks_to_read" to maxChunksToRead,
            "sticky_mimetype" to stickyMimetype,
        )

    fun validate(): Status = NativeBridge.status("reader.validateOptions", args = fields())
}

/** Writer options use native admission bounds and batching. */
data class ChunkStoreWriterOptions(
    val offset: Long = writerDefaults.long("offset"),
    val maxChunksToWriteAtOnce: Long = writerDefaults.long("max_chunks_to_write_at_once"),
    val numChunksToBuffer: Long? = null,
    val stickyMimetype: Boolean = writerDefaults.boolean("sticky_mimetype"),
) {
    internal fun fields(): Map<String, Any?> =
        mapOf(
            "offset" to offset,
            "max_chunks_to_write_at_once" to maxChunksToWriteAtOnce,
            "num_chunks_to_buffer" to numChunksToBuffer,
            "sticky_mimetype" to stickyMimetype,
        )

    fun validate(): Status = NativeBridge.status("writer.validateOptions", args = fields())
}

private val readerDefaults by lazy {
    NativeBridge.fields(NativeBridge.call("reader.defaults").value())
}
private val writerDefaults by lazy {
    NativeBridge.fields(NativeBridge.call("writer.defaults").value())
}

private fun Map<String, Any?>.boolean(name: String): Boolean = nativeBoolean(getValue(name))

private fun Map<String, Any?>.long(name: String): Long = NativeBridge.number(getValue(name))

/** C++ reader with ordered or arrival-order cursors and bounded prefetch. */
class ChunkStoreReader internal constructor(id: Long) : NativeObject(id) {
    suspend fun next(
        timeout: kotlin.time.Duration = kotlin.time.Duration.INFINITE
    ): StatusOr<NodeFragment?> =
        NativeBridge.async("reader.next", this, mapOf("timeout" to timeout)).map { value ->
            value?.let(::nativeFragment)
        }

    suspend fun next(timeout: Duration): StatusOr<NodeFragment?> =
        NativeBridge.async("reader.next", this, mapOf("timeout" to timeout)).map { value ->
            value?.let(::nativeFragment)
        }

    suspend fun next(timeoutMs: Long?): StatusOr<NodeFragment?> =
        NativeBridge.async("reader.next", this, mapOf("timeoutMs" to timeoutMs)).map { value ->
            value?.let(::nativeFragment)
        }

    suspend fun nextMany(
        limit: Int,
        timeout: kotlin.time.Duration = kotlin.time.Duration.INFINITE,
    ): StatusOr<List<NodeFragment?>> =
        NativeBridge.async("reader.nextMany", this, mapOf("limit" to limit, "timeout" to timeout))
            .map(::nativeFragments)

    suspend fun nextMany(limit: Int, timeout: Duration): StatusOr<List<NodeFragment?>> =
        NativeBridge.async("reader.nextMany", this, mapOf("limit" to limit, "timeout" to timeout))
            .map(::nativeFragments)

    suspend fun nextMany(limit: Int, timeoutMs: Long?): StatusOr<List<NodeFragment?>> =
        NativeBridge.async(
                "reader.nextMany",
                this,
                mapOf("limit" to limit, "timeoutMs" to timeoutMs),
            )
            .map(::nativeFragments)

    fun ensureStarted(): Status = NativeBridge.status("reader.ensureStarted", this)

    fun cancel(): Status = NativeBridge.status("reader.cancel", this)

    val status: Status
        get() = NativeBridge.call("reader.status", this).map(NativeBridge::statusValue).value()

    suspend fun done(): Status = NativeBridge.async("reader.done", this).status

    companion object {
        fun create(
            store: ChunkStore,
            options: ChunkStoreReaderOptions = ChunkStoreReaderOptions(),
        ): StatusOr<ChunkStoreReader> =
            NativeBridge.call(
                    "reader.create",
                    args = mapOf("store" to store, "options" to options.fields()),
                )
                .map { value -> ChunkStoreReader(NativeBridge.handle(value)) }
    }
}

/** Admission and persistence are separate native completion barriers. */
data class ChunkStoreWrite(val admitted: NativeFuture<Unit>, val confirmation: NativeFuture<Long>)

/** C++ writer with bounded admission, batching, and native tee semantics. */
class ChunkStoreWriter internal constructor(id: Long) : NativeObject(id) {
    suspend fun putChunk(chunk: Chunk, seq: Long? = null, final: Boolean = false): StatusOr<Long> =
        NativeBridge.async(
                "writer.putChunk",
                this,
                mapOf("chunk" to wireFields(chunk), "seq" to seq, "final" to final),
            )
            .map(NativeBridge::number)

    fun enqueueChunk(
        chunk: Chunk,
        seq: Long? = null,
        final: Boolean = false,
    ): StatusOr<ChunkStoreWrite> =
        NativeBridge.call(
                "writer.enqueueChunk",
                this,
                mapOf("chunk" to wireFields(chunk), "seq" to seq, "final" to final),
            )
            .map { value ->
                val fields = NativeBridge.fields(value)
                ChunkStoreWrite(
                    NativeFuture(NativeBridge.handle(fields["admitted"])) { Unit },
                    NativeFuture(NativeBridge.handle(fields["confirmation"]), NativeBridge::number),
                )
            }

    fun ensureStarted(): Status = NativeBridge.status("writer.ensureStarted", this)

    fun flush(): Status = NativeBridge.status("writer.flush", this)

    val status: Status?
        get() =
            NativeBridge.call("writer.status", this)
                .map { value -> value?.let(NativeBridge::statusValue) }
                .value()

    suspend fun cancel(): Status = NativeBridge.async("writer.cancel", this).status

    suspend fun drainAndClose(): Status = NativeBridge.async("writer.drainAndClose", this).status

    suspend fun abortWithStatus(status: Status): Status =
        NativeBridge.async("writer.abort", this, mapOf("status" to statusFields(status))).status

    suspend fun waitForBufferToDrain(): Status =
        NativeBridge.async("writer.waitForBufferToDrain", this).status

    companion object {
        fun create(
            store: ChunkStore,
            options: ChunkStoreWriterOptions = ChunkStoreWriterOptions(),
        ): StatusOr<ChunkStoreWriter> =
            NativeBridge.call(
                    "writer.create",
                    args = mapOf("store" to store, "options" to options.fields()),
                )
                .map { value -> ChunkStoreWriter(NativeBridge.handle(value)) }
    }
}
