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
class AsyncNode private constructor(val id: String, private var registry: SerializationRegistry) {
    private val lock = Any()
    private val bySeq = HashMap<Long, NodeFragment>()
    private var nextWriteSeq = 0L
    private var nextReadSeq = 0L
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
        chunk.validate().orElse { status ->
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
                NodeFragment(id = id, data = chunk, seq = actualSeq.toUInt(), continued = !final)
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
        val chunk =
            fragment.getChunk().orElse {
                return unimplemented("AsyncNode writers do not resolve NodeRef payloads.")
            }
        return putChunk(chunk, fragment.seq?.toLong(), !fragment.continued)
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
            is NodeFragment ->
                putChunk(
                    value.getChunk().orElse { status ->
                        return status
                    },
                    value.seq?.toLong(),
                    final = true,
                )
            is Chunk -> putChunk(value, seq, final = true)
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

    suspend fun nextChunk(timeoutMs: Long? = null): StatusOr<Chunk?> {
        val fragment =
            nextFragment(timeoutMs).orElse { status ->
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
                nextFragment(remaining).orElse { status ->
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
        if (first.continued) {
            val terminator =
                nextFragment(timeoutMs).orElse { status ->
                    return status
                }
                    ?: return failedPrecondition(
                        "A continued consumed value must be followed by a null final chunk."
                    )

            val terminatorChunk =
                terminator.getChunk().orElse { status ->
                    return status
                }
            if (terminator.continued || !terminatorChunk.isNull) {
                return failedPrecondition(
                    "The only fragment allowed after a consumed value is a null final chunk."
                )
            }
        }
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
        ): StatusOr<AsyncNode> {
            validateName(id).orElse { status ->
                return status
            }
            return Ok(AsyncNode(id, registry))
        }
    }
}

/** Registry of lazily created [AsyncNode]s keyed by stable node id. */
class NodeMap {
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
