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

import a11.net.WireStream
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/** One decoded value or the clean end of a node, including nullable values. */
sealed interface NodeRead<out T> {
    data class Value<out T>(val value: T) : NodeRead<T>

    data object End : NodeRead<Nothing>
}

/**
 * Ordered in-memory chunks with one consuming cursor.
 *
 * Writes and reads own their wire values. Finality ends the logical sequence; close ends writes,
 * and abort carries a non-OK status to readers. Suspended reads release the monitor and resume on
 * writes, closure, or cancellation.
 */
class AsyncNode
private constructor(
    val id: String,
    private var registry: SerializationRegistry,
    @Volatile private var readerOptions: ChunkStoreReaderOptions,
) {
    internal var owner: NodeMap? = null

    private data class LogicalFrame(val node: AsyncNode, var position: Long, var remaining: ULong?)

    private val logicalLock = Mutex()
    private var logicalFrames: MutableList<LogicalFrame>? = null
    private var logicalEnded = false
    private var logicalFailure: Err? = null
    private var logicalRootFinal = false
    private var logicalEmitted = 0L
    private var logicalMimetype = ""

    fun ref(
        offset: UInt = 0u,
        length: ULong? = null,
        forNode: AsyncNode? = null,
        forMap: NodeMap? = null,
        storage: String = "auto",
    ): StatusOr<NodeRef> {
        val destination = forMap ?: forNode?.owner
        if (forNode != null && destination == null)
            return failedPrecondition("Destination node has no owning node map.")
        val found =
            destination?.getIfExists(id)?.orElse {
                return it
            }
        val same = destination == null || found === this
        if (storage !in listOf("auto", "include", "omit"))
            return invalidArgument("storage must be auto, include, or omit.")
        if (storage == "omit" && !same)
            return failedPrecondition("Destination cannot resolve the unqualified source node.")
        val identity =
            if (storage == "include" || (storage == "auto" && !same)) {
                val source =
                    owner?.storage
                        ?: return failedPrecondition("Source has no portable storage identity.")
                StorageIdentity(source.id)
            } else null
        return Ok(NodeRef(id, offset, length, identity))
    }

    private val lock = Any()
    private val bySeq = HashMap<Long, NodeFragment>()
    private var nextWriteSeq = 0L
    private var nextReadSeq = 0L
    private var rawReadStarted = false
    private var logicalReadStarted = false
    private var finalSeq: Long? = null
    private var writerClosed = false
    @Volatile private var abortStatus: Err? = null
    private val streams = mutableListOf<WireStream>()
    private var readerCancelled = false
    private val changeWaiters = mutableListOf<Deferred<Unit>>()

    private var expectedPatterns: List<String> = emptyList()
    private var expectedTag: String? = null

    val serializationRegistry: SerializationRegistry
        get() = registry

    fun getReaderOptions(): ChunkStoreReaderOptions = synchronized(lock) { readerOptions.copy() }

    fun setReaderOptions(options: ChunkStoreReaderOptions): Status {
        options.validate().orElse {
            return it
        }
        synchronized(lock) {
            if (rawReadStarted || logicalReadStarted)
                return failedPrecondition("Reader options cannot change after reading starts.")
            readerOptions = options.copy()
            nextReadSeq = options.offset
        }
        return Status.ok()
    }

    fun setSerializationRegistry(registry: SerializationRegistry): Status {
        this.registry = registry
        return Status.ok()
    }

    fun setExpectedTypes(
        mimetypePatterns: List<String> = emptyList(),
        expectedTag: String? = null,
    ): Status {
        this.expectedPatterns = mimetypePatterns
        this.expectedTag = expectedTag
        return Status.ok()
    }

    // --- producing half ------------------------------------------------------

    suspend fun putChunk(chunk: Chunk, seq: Long? = null, final: Boolean = false): StatusOr<Long> {
        return putPayload(chunk, seq, final)
    }

    private suspend fun putPayload(
        payload: NodePayload,
        seq: Long?,
        final: Boolean,
    ): StatusOr<Long> {
        payload.validate().orElse { status ->
            return status
        }
        val fragment: NodeFragment
        synchronized(lock) {
            abortStatus?.let { value ->
                return value
            }
            if (writerClosed) return failedPrecondition("AsyncNode writer is closed.")
            var actualSeq = seq ?: nextWriteSeq
            if (seq == null) while (bySeq.containsKey(actualSeq)) actualSeq++
            if (actualSeq !in 0..0xffff_ffffL) {
                return (if (seq == null) resourceExhausted("Maximum implicit sequence exceeded.")
                else invalidArgument("Sequence must be a uint32 integer."))
            }
            if (bySeq.containsKey(actualSeq))
                return alreadyExists("Fragment $actualSeq already exists.")
            if (final && finalSeq != null && finalSeq != actualSeq) {
                return failedPrecondition("The node already has a different final sequence.")
            }
            val pendingFinal = if (final) actualSeq else finalSeq
            if (
                pendingFinal != null &&
                    (actualSeq > pendingFinal || bySeq.keys.any { item -> item > pendingFinal })
            ) {
                return invalidArgument("A fragment exceeds the final sequence.")
            }
            nextWriteSeq++
            fragment =
                NodeFragment(id = id, data = payload, seq = actualSeq.toUInt(), continued = !final)
                    .copy()
            bySeq[actualSeq] = fragment
            if (final) finalSeq = actualSeq
            notifyChange()
        }
        for (stream in streamsSnapshot()) {
            val sent = stream.send(WireMessage(nodeFragments = mutableListOf(fragment.copy())))
            sent.orElse { status ->
                return status
            }
        }
        return Ok(fragment.seq!!.toLong())
    }

    suspend fun putFragment(fragment: NodeFragment): StatusOr<Long> {
        fragment.validate().orElse { status ->
            return status
        }
        return putPayload(fragment.data, fragment.seq?.toLong(), !fragment.continued)
    }

    /** Serialize and persist one application value. */
    suspend fun put(
        value: Any?,
        seq: Long? = null,
        final: Boolean = false,
        mimetype: String = "",
    ): StatusOr<Long> {
        when (value) {
            is NodeFragment -> return putFragment(value)
            is Chunk -> return putChunk(value, seq, final)
            is NodeRef -> return putPayload(value, seq, final)
            else -> {
                val chunk =
                    registry.toChunk(value, mimetype).orElse { status ->
                        return status
                    }
                return putChunk(chunk, seq, final)
            }
        }
    }

    /**
     * End the stream: mark the logical end of the data, and close the writer.
     *
     * The one call an ordinary producer needs. [value] is written as the final fragment; passing
     * nothing (or null) writes a null terminator instead, which is the form to use once the last
     * visible value has already gone out with [put]. Unless [close] is cleared the writer is closed
     * too, so readers waiting for data that can no longer arrive are released and a peer's mirror
     * of the node closes as well.
     *
     * [wait] exists for byte-compatibility with the other A11 runtimes: this port keeps its log in
     * memory, so the write and the close are done by the time this returns either way.
     */
    suspend fun finalize(
        value: Any? = null,
        seq: Long? = null,
        mimetype: String = "",
        wait: Boolean = false,
        close: Boolean = true,
    ): Status {
        when (value) {
            null -> putChunk(makeNullChunk(), seq, final = true)
            is NodeFragment -> putPayload(value.data, value.seq?.toLong(), final = true)
            is Chunk -> putChunk(value, seq, final = true)
            is NodeRef -> putPayload(value, seq, final = true)
            else -> put(value, seq, final = true, mimetype = mimetype)
        }.orElse { status ->
            return status
        }
        return if (close) this.close() else Status.ok()
    }

    /**
     * Flush queued writes and close the producing half, marking nothing final.
     *
     * The specialised half of [finalize]: closure without finality, for a producer that cannot say
     * which chunk was the last one -- a log, say -- but can say that no more are coming.
     *
     * Attached streams are told, because a peer ends a node on a not-continued fragment and closing
     * writes none: after the last teed fragment the node sends one closure marker, so a mirror of
     * this node on the far side closes its write half too.
     */
    suspend fun close(): Status {
        val alreadyClosed: Boolean
        synchronized(lock) {
            alreadyClosed = writerClosed
            writerClosed = true
            notifyChange()
        }
        if (alreadyClosed) return Status.ok()
        val marker =
            statusToChunk(Status.ok(), closing = true).orElse { status ->
                return status
            }
        val fragment = NodeFragment(id = id, data = marker, seq = 0u, continued = false)
        for (stream in streamsSnapshot()) {
            val sent = stream.send(WireMessage(nodeFragments = mutableListOf(fragment.copy())))
            sent.orElse { status ->
                return status
            }
        }
        return Status.ok()
    }

    /** Fail the producing half so readers observe a structured terminal error. */
    suspend fun abortWithStatus(status: Status): Status {
        if (status.ok()) return invalidArgument("Abort status must be non-OK.")
        val notify: Boolean
        synchronized(lock) {
            if (writerClosed) return failedPrecondition("AsyncNode writer is closed.")
            if (abortStatus == null) {
                abortStatus = Err(status)
                notify = true
            } else notify = false
            if (notify) notifyChange()
        }
        return Status.ok()
    }

    suspend fun isWritable(): StatusOr<Boolean> =
        synchronized(lock) { Ok(!writerClosed && abortStatus == null) }

    fun getWriterAbortStatus(): Status? = abortStatus

    suspend fun size(): StatusOr<Int> = synchronized(lock) { Ok(bySeq.size) }

    fun attachStream(stream: WireStream): Status {
        synchronized(streams) { if (stream !in streams) streams.add(stream) }
        return Status.ok()
    }

    fun detachStream(stream: WireStream): Status {
        synchronized(streams) { streams.remove(stream) }
        return Status.ok()
    }

    private fun streamsSnapshot(): List<WireStream> = synchronized(streams) { streams.toList() }

    fun cancelReader(): Status {
        synchronized(lock) {
            readerCancelled = true
            notifyChange()
        }
        return Status.ok()
    }

    suspend fun cancelWriter(): Status = abortWithStatus(aborted("ChunkStoreWriter was cancelled"))

    // --- consuming half ------------------------------------------------------

    /** Read the next raw fragment in seq order, or null at clean end of stream. */
    suspend fun nextFragment(timeoutMs: Long? = null): StatusOr<NodeFragment?> {
        if (timeoutMs != null && timeoutMs < 0)
            return invalidArgument("Timeout must be non-negative.")
        val started = System.nanoTime()
        while (true) {
            val waiter: Deferred<Unit>
            synchronized(lock) {
                rawReadStarted = true
                if (readerCancelled) return aborted("ChunkStoreReader was cancelled")
                val fin = finalSeq
                if (fin != null && nextReadSeq > fin) return Ok(null)
                val fragment = bySeq[nextReadSeq]
                if (fragment != null) {
                    nextReadSeq += 1
                    return Ok(
                        fragment.copy().also { value ->
                            value.continued = fin == null || value.seq!!.toLong() < fin
                        }
                    )
                }
                abortStatus?.let { value ->
                    return value
                }
                if (writerClosed) {
                    if (
                        nextReadSeq >= nextWriteSeq &&
                            bySeq.keys.none { item -> item >= nextReadSeq }
                    )
                        return Ok(null)
                    return notFound("The closed node has no fragment at sequence $nextReadSeq.")
                }
                waiter = Deferred<Unit>().also { value -> changeWaiters.add(value) }
            }
            try {
                val remaining =
                    timeoutMs?.let { value ->
                        maxOf(0L, value - (System.nanoTime() - started) / 1_000_000)
                    }
                when (val waited = waitFor(remaining) { waiter.await() }) {
                    is Ok -> Unit
                    is Err -> return waited
                }
            } finally {
                synchronized(lock) { changeWaiters.remove(waiter) }
            }
        }
    }

    private suspend fun fragmentAt(position: Long, timeoutMs: Long?): StatusOr<NodeFragment?> {
        val started = System.nanoTime()
        while (true) {
            val waiter: Deferred<Unit>
            synchronized(lock) {
                if (readerCancelled) return aborted("ChunkStoreReader was cancelled")
                val fin = finalSeq
                if (fin != null && position > fin) return Ok(null)
                bySeq[position]?.let { value ->
                    return Ok(value.copy().also { it.continued = fin == null || position < fin })
                }
                abortStatus?.let {
                    return it
                }
                if (writerClosed) {
                    if (position >= nextWriteSeq && bySeq.keys.none { it >= position })
                        return Ok(null)
                    return notFound("The closed node has no fragment at sequence $position.")
                }
                waiter = Deferred<Unit>().also { changeWaiters.add(it) }
            }
            try {
                val remaining =
                    timeoutMs?.let { maxOf(0L, it - (System.nanoTime() - started) / 1_000_000) }
                waitFor(remaining) { waiter.await() }
                    .orElse {
                        return it
                    }
            } finally {
                synchronized(lock) { changeWaiters.remove(waiter) }
            }
        }
    }

    private suspend fun nextLogicalFragment(timeoutMs: Long? = null): StatusOr<NodeFragment?> =
        logicalLock.withLock {
            synchronized(lock) { logicalReadStarted = true }
            logicalFailure?.let {
                return@withLock it
            }
            if (logicalEnded) return@withLock Ok(null)
            val frames =
                logicalFrames
                    ?: mutableListOf(LogicalFrame(this, readerOptions.offset, null)).also {
                        logicalFrames = it
                    }
            if (readerOptions.maxChunksToRead == 0L) {
                logicalEnded = true
                return@withLock Ok(null)
            }
            val started = System.nanoTime()
            var steps = 0
            while (frames.isNotEmpty()) {
                if (++steps % 128 == 0) kotlinx.coroutines.yield()
                val frame = frames.last()
                if (frame.remaining == 0uL) {
                    frames.removeAt(frames.lastIndex)
                    continue
                }
                val remainingTime =
                    timeoutMs?.let { maxOf(0L, it - (System.nanoTime() - started) / 1_000_000) }
                val fragment =
                    frame.node.fragmentAt(frame.position, remainingTime).orElse { status ->
                        if (status.code != StatusCode.DEADLINE_EXCEEDED) logicalFailure = status
                        return@withLock status
                    }
                if (fragment == null) {
                    if (frame.remaining != null) {
                        val failure = dataLoss("Referenced node ended before its fragment window.")
                        logicalFailure = failure
                        return@withLock failure
                    }
                    frames.removeAt(frames.lastIndex)
                    continue
                }
                frame.position++
                frame.remaining = frame.remaining?.minus(1uL)
                if (frames.size == 1 && !fragment.continued) logicalRootFinal = true
                val reference = fragment.data as? NodeRef
                if (reference != null) {
                    if (!readerOptions.followNodeRefs) {
                        val failure = unimplemented("Reference following is disabled.")
                        logicalFailure = failure
                        return@withLock failure
                    }
                    if (reference.length == 0uL) continue
                    if (frames.size > readerOptions.maxRefDepth) {
                        val failure = resourceExhausted("Reference depth exceeded.")
                        logicalFailure = failure
                        return@withLock failure
                    }
                    val context =
                        frame.node.owner
                            ?: return@withLock notFound("Reference source has no node map.")
                    val target =
                        context.resolveForRead(reference).orElse { status ->
                            logicalFailure = status
                            return@withLock status
                        }
                    if (frames.any { it.node === target }) {
                        val failure = failedPrecondition("Reference cycle detected.")
                        logicalFailure = failure
                        return@withLock failure
                    }
                    frames.add(LogicalFrame(target, reference.offset.toLong(), reference.length))
                    continue
                }
                val chunk = fragment.data as Chunk
                if (chunk.isNull) {
                    if (!fragment.continued && frames.size == 1) {
                        logicalEnded = true
                        return@withLock Ok(null)
                    }
                    continue
                }
                if (readerOptions.stickyMimetype) {
                    if (chunk.mimetype.isNotEmpty()) logicalMimetype = chunk.mimetype
                    else if (logicalMimetype.isNotEmpty()) {
                        val metadata = chunk.metadata ?: ChunkMetadata()
                        metadata.mimetype = logicalMimetype
                        chunk.metadata = metadata
                    }
                }
                logicalEmitted++
                if (
                    readerOptions.maxChunksToRead != null &&
                        logicalEmitted >= readerOptions.maxChunksToRead!!
                )
                    logicalEnded = true
                return@withLock Ok(fragment)
            }
            logicalEnded = true
            Ok(null)
        }

    suspend fun nextChunk(timeoutMs: Long? = null): StatusOr<Chunk?> {
        val fragment =
            nextLogicalFragment(timeoutMs).orElse { status ->
                return status
            } ?: return Ok(null)
        return fragment.getChunk()
    }

    /**
     * The next fragment carrying a value, or null once the node ends.
     *
     * A null chunk is a marker, not a value: a final one says the node is finished, and a non-final
     * one says nothing at all. Neither is something a reader asked for, so both are skipped here
     * rather than surfaced as a value or rejected — which is what lets a node be closed with
     * nothing in it.
     */
    private suspend fun nextValueFragment(timeoutMs: Long?): StatusOr<NodeFragment?> {
        if (timeoutMs != null && timeoutMs < 0)
            return invalidArgument("Timeout must be non-negative.")
        val started = System.nanoTime()
        while (true) {
            val remaining =
                timeoutMs?.let { value ->
                    maxOf(0L, value - ((System.nanoTime() - started) / 1_000_000))
                }
            val fragment =
                nextLogicalFragment(remaining).orElse { status ->
                    return status
                } ?: return Ok(null)
            val chunk =
                fragment.getChunk().orElse { status ->
                    return status
                }
            if (!chunk.isNull) return Ok(fragment)
            if (!fragment.continued) return Ok(null)
        }
    }

    /** Read a value, preserving the distinction between JSON null and end. */
    suspend fun read(
        timeoutMs: Long? = null,
        mimetypePatterns: List<String>? = null,
        expectedTag: String? = null,
    ): StatusOr<NodeRead<Any?>> {
        val fragment =
            nextValueFragment(timeoutMs).orElse { status ->
                return status
            } ?: return Ok(NodeRead.End)
        val chunk =
            fragment.getChunk().orElse { status ->
                return status
            }
        return registry
            .fromChunk(chunk, mimetypePatterns ?: expectedPatterns, expectedTag ?: this.expectedTag)
            .map { value -> NodeRead.Value(value) }
    }

    /**
     * Read a value, returning null for JSON null or clean end. Use [read] when nullable payloads
     * need an explicit end marker.
     */
    suspend fun next(
        timeoutMs: Long? = null,
        mimetypePatterns: List<String>? = null,
        expectedTag: String? = null,
    ): StatusOr<Any?> =
        read(timeoutMs, mimetypePatterns, expectedTag).map { element ->
            when (element) {
                is NodeRead.Value -> element.value
                NodeRead.End -> null
            }
        }

    /**
     * Consume exactly one whole unary value and validate its terminator.
     *
     * Two spellings are accepted: the value written as final, or the value followed by a null final
     * chunk. With [allowNone] a node holding no value — closed empty, or holding nothing but a null
     * final — yields null.
     */
    suspend fun consume(
        timeoutMs: Long? = null,
        allowNone: Boolean = false,
        mimetypePatterns: List<String>? = null,
        expectedTag: String? = null,
    ): StatusOr<Any?> {
        val started = System.nanoTime()
        val first =
            nextValueFragment(timeoutMs).orElse { status ->
                return status
            }
                ?: return if (allowNone) Ok(null)
                else failedPrecondition("AsyncNode is empty at the current reader offset.")
        val chunk =
            first.getChunk().orElse { status ->
                return status
            }
        val remaining = timeoutMs?.let { maxOf(0L, it - (System.nanoTime() - started) / 1_000_000) }
        val second =
            nextLogicalFragment(remaining).orElse {
                return it
            }
        if (second != null)
            return failedPrecondition("A consumed node contains more than one value.")
        if (!logicalRootFinal)
            return failedPrecondition("A consumed node requires a final fragment.")
        return registry.fromChunk(
            chunk,
            mimetypePatterns ?: expectedPatterns,
            expectedTag ?: this.expectedTag,
        )
    }

    private fun notifyChange() {
        val waiters = changeWaiters.toList()
        changeWaiters.clear()
        for (w in waiters) w.resolve(Unit)
    }

    companion object {
        fun create(
            id: String,
            registry: SerializationRegistry = SerializationRegistry.getGlobal(),
            readerOptions: ChunkStoreReaderOptions = ChunkStoreReaderOptions(),
        ): StatusOr<AsyncNode> {
            validateName(id).orElse { status ->
                return status
            }
            readerOptions.validate().orElse {
                return it
            }
            return Ok(AsyncNode(id, registry, readerOptions.copy()))
        }
    }
}

/** Registry of lazily created [AsyncNode]s keyed by stable node id. */
class NodeMap(val storage: StorageIdentity? = null) {
    @Volatile private var allowLazyReadResolution = false

    fun setAllowLazyReadResolution(allowed: Boolean) {
        allowLazyReadResolution = allowed
    }

    private val storages = java.util.concurrent.ConcurrentHashMap<String, NodeMap>()
    private val imported =
        java.util.concurrent.ConcurrentHashMap<
            String,
            java.util.concurrent.ConcurrentHashMap<String, AsyncNode>,
        >()
    private var storageResolver: ((StorageIdentity) -> StatusOr<NodeMap>)? = null
    private var accessibleIds: List<String> = emptyList()

    fun registerStorage(id: String, backend: NodeMap): Status {
        validateName(id).orElse {
            return it
        }
        if (backend === this) return invalidArgument("Storage namespace requires another node map.")
        val previous = storages.putIfAbsent(id, backend)
        return if (previous != null && previous !== backend)
            alreadyExists("Storage identity is already configured.")
        else Status.ok()
    }

    fun setStorageResolver(
        resolver: (StorageIdentity) -> StatusOr<NodeMap>,
        accessibleIds: List<String> = emptyList(),
    ): Status {
        for (id in accessibleIds) validateName(id).orElse {
            return it
        }
        storageResolver = resolver
        this.accessibleIds = accessibleIds.toList()
        return Status.ok()
    }

    fun storageIds(): List<String> =
        (storages.keys + accessibleIds + listOfNotNull(storage?.id)).distinct().sorted()

    private val nodes = HashMap<String, AsyncNode>()
    private val lock = Any()

    /** Return the canonical node, creating it on first access. */
    fun get(nodeId: String): StatusOr<AsyncNode> {
        validateName(nodeId).orElse { status ->
            return status
        }
        synchronized(lock) {
            nodes[nodeId]?.let { value ->
                return Ok(value)
            }
            val node =
                AsyncNode.create(nodeId).orElse { status ->
                    return status
                }
            node.owner = this
            nodes[nodeId] = node
            return Ok(node)
        }
    }

    fun getIfExists(nodeId: String): StatusOr<AsyncNode?> {
        validateName(nodeId).orElse { status ->
            return status
        }
        return Ok(synchronized(lock) { nodes[nodeId] })
    }

    /** Resolve an existing reference target without creating an empty node. */
    fun resolveForRead(reference: NodeRef): StatusOr<AsyncNode> {
        reference.validate().orElse {
            return it
        }
        val identity = reference.storage
        val local = identity == null || identity.id == storage?.id
        val registered = if (local) this else storages[identity!!.id]
        if (registered != null) {
            if (registered.allowLazyReadResolution) return registered.get(reference.id)
            val target =
                registered.getIfExists(reference.id).orElse {
                    return it
                }
            return target?.let { Ok(it) } ?: notFound("Referenced node does not exist.")
        }
        if (identity != null && storageResolver != null) {
            val resolved =
                try {
                    storageResolver!!.invoke(identity)
                } catch (error: Exception) {
                    Status.fromException(error)
                }
            if (resolved is Ok) {
                if (resolved.value.allowLazyReadResolution) return resolved.value.get(reference.id)
                val target =
                    resolved.value.getIfExists(reference.id).orElse {
                        return it
                    }
                return target?.let { Ok(it) } ?: notFound("Referenced node does not exist.")
            }
            if (resolved.status.code != StatusCode.NOT_FOUND) return resolved as Err
        }
        return imported[identity!!.id]?.get(reference.id)?.let { Ok(it) }
            ?: notFound("Storage identity is not configured on this host.")
    }

    /** Resolve a whole-node reference; windows require a fragment-window reader. */
    fun resolve(reference: NodeRef): StatusOr<AsyncNode> {
        reference.validate().orElse {
            return it
        }
        if (reference.offset != 0u || reference.length != null)
            return unimplemented("Resolving a NodeRef window requires a fragment-window reader.")
        val identity = reference.storage
        if (identity == null || identity.id == storage?.id) return get(reference.id)
        storages[identity.id]?.let {
            return it.get(reference.id)
        }
        storageResolver?.let { resolver ->
            val resolved =
                try {
                    resolver(identity)
                } catch (error: Exception) {
                    Status.fromException(error)
                }
            if (resolved is Ok) return resolved.value.get(reference.id)
            if (resolved.status.code != StatusCode.NOT_FOUND) return resolved as Err
        }
        return imported[identity.id]?.get(reference.id)?.let { Ok(it) }
            ?: notFound("Storage identity is not configured on this host.")
    }

    /** Share a referenced node from another scope, preserving its identity. */
    fun importReference(reference: NodeRef, source: NodeMap): StatusOr<AsyncNode> {
        val identity = reference.storage
        if (identity != null) {
            val direct = resolve(reference)
            if (direct is Ok) return direct
            if (direct.status.code != StatusCode.NOT_FOUND) return direct
            var original = source.resolve(reference)
            if (original is Err && original.status.code == StatusCode.NOT_FOUND)
                original = source.get(reference.id)
            val node =
                original.orElse {
                    return it
                }
            val group =
                imported.computeIfAbsent(identity.id) { java.util.concurrent.ConcurrentHashMap() }
            val previous = group.putIfAbsent(reference.id, node)
            if (previous != null && previous !== node)
                return alreadyExists("Conflicting storage-qualified node.")
            return Ok(node)
        }
        val node =
            source.resolve(reference).orElse {
                return it
            }
        insert(node).orElse {
            return it
        }
        return Ok(node)
    }

    /** Share an existing node under its id; a different existing node is an error. */
    private fun insert(node: AsyncNode): Status =
        synchronized(lock) {
            val found = nodes[node.id]
            if (found != null && found !== node)
                return alreadyExists("A different node has this id.")
            nodes[node.id] = node
            Status.ok()
        }

    fun discard(nodeId: String, expected: AsyncNode? = null): StatusOr<AsyncNode?> {
        validateName(nodeId).orElse { status ->
            return status
        }
        synchronized(lock) {
            val found = nodes[nodeId] ?: return Ok(null)
            if (expected != null && found !== expected) return Ok(null)
            nodes.remove(nodeId)
            return Ok(found)
        }
    }

    fun contains(nodeId: String): Boolean = synchronized(lock) { nodes.containsKey(nodeId) }

    val size: Int
        get() = synchronized(lock) { nodes.size }
}
