package a11

import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/** Transport sink a node may tee its writes to (subset of WireStream/Session). */
interface WritableWireStream {
    fun send(message: WireMessage): Status
}

/**
 * One ordered, typed value sequence flowing through an A11 application.
 *
 * This is the Kotlin counterpart of `js/src/async_node.ts`. The reference splits
 * the log across a pluggable ChunkStore + reader + writer; because that store is
 * a purely local abstraction, the Kotlin port keeps a single in-memory,
 * seq-ordered buffer guarded by a coroutine [Mutex]. The observable contract —
 * ordered delivery, an explicit final marker, backpressure-free local writes,
 * structured abort, and teeing writes to a wire stream — matches the reference,
 * which is what the wire protocol and the higher layers depend on.
 */
class AsyncNode private constructor(
    val id: String,
    private var registry: SerializationRegistry,
) {
    private val mutex = Mutex()
    private val bySeq = HashMap<Long, NodeFragment>()
    private var nextWriteSeq = 0L
    private var nextReadSeq = 0L
    private var finalSeq: Long? = null
    private var writerClosed = false
    private var abortStatus: Status? = null
    private val streams = mutableListOf<WritableWireStream>()
    private var readerCancelled = false
    private val changeWaiters = mutableListOf<Deferred<Unit>>()

    private var expectedPatterns: List<String> = emptyList()
    private var expectedTag: String? = null

    val serializationRegistry: SerializationRegistry get() = registry

    fun setSerializationRegistry(registry: SerializationRegistry): Status {
        this.registry = registry
        return Status.ok()
    }

    fun setExpectedTypes(mimetypePatterns: List<String> = emptyList(), expectedTag: String? = null): Status {
        this.expectedPatterns = mimetypePatterns
        this.expectedTag = expectedTag
        return Status.ok()
    }

    // --- producing half ------------------------------------------------------

    suspend fun putChunk(chunk: Chunk, seq: Long? = null, final: Boolean = false): StatusOr<Long> {
        val fragment: NodeFragment
        mutex.withLock {
            abortStatus?.let { return it }
            if (writerClosed) return failedPrecondition("AsyncNode writer is closed.")
            val actualSeq = seq ?: nextWriteSeq
            if (actualSeq + 1 > nextWriteSeq) nextWriteSeq = actualSeq + 1
            fragment = NodeFragment(id = id, data = chunk, seq = actualSeq, continued = !final)
            bySeq[actualSeq] = fragment
            if (final) finalSeq = actualSeq
            notifyChange()
        }
        for (stream in streamsSnapshot()) {
            val sent = stream.send(WireMessage(nodeFragments = mutableListOf(fragment.copy())))
            if (!sent.isOk) return sent
        }
        return Ok(fragment.seq!!)
    }

    suspend fun putFragment(fragment: NodeFragment): StatusOr<Long> {
        val chunk = fragment.getChunk().orElse {
            return unimplemented("AsyncNode writers do not resolve NodeRef payloads.")
        }
        return putChunk(chunk, fragment.seq, !fragment.continued)
    }

    /** Serialize and persist one application value. */
    suspend fun put(value: Any?, seq: Long? = null, final: Boolean = false, mimetype: String = ""): StatusOr<Long> {
        when (value) {
            is NodeFragment -> return putFragment(value)
            is Chunk -> return putChunk(value, seq, final)
            else -> {
                val chunk = registry.toChunk(value, mimetype).orElse { return it }
                return putChunk(chunk, seq, final)
            }
        }
    }

    suspend fun putFinal(value: Any?, seq: Long? = null, mimetype: String = ""): StatusOr<Long> =
        put(value, seq, final = true, mimetype = mimetype)

    suspend fun putNullFinal(seq: Long? = null): StatusOr<Long> = putChunk(makeNullChunk(), seq, final = true)

    /**
     * Close the producing half without adding a final fragment.
     *
     * Attached streams are told, because a peer ends a node on a not-continued
     * fragment and closing writes none: after the last teed fragment the node
     * sends one closure marker, so a mirror of this node on the far side closes
     * its write half too.
     */
    suspend fun drainAndClose(): Status {
        val alreadyClosed: Boolean
        mutex.withLock {
            alreadyClosed = writerClosed
            writerClosed = true
            notifyChange()
        }
        if (alreadyClosed) return Status.ok()
        val marker = statusToChunk(Status.ok(), closing = true).orElse { return it }
        val fragment = NodeFragment(id = id, data = marker, seq = 0L, continued = false)
        for (stream in streamsSnapshot()) {
            val sent = stream.send(WireMessage(nodeFragments = mutableListOf(fragment.copy())))
            if (!sent.isOk) return sent
        }
        return Status.ok()
    }

    /** Fail the producing half so readers observe a structured terminal error. */
    suspend fun abortWithStatus(status: Status): Status {
        val notify: Boolean
        mutex.withLock {
            if (abortStatus == null && !status.isOk) {
                abortStatus = status
                notify = true
            } else notify = false
            if (notify) notifyChange()
        }
        return Status.ok()
    }

    suspend fun isWritable(): StatusOr<Boolean> = mutex.withLock { Ok(!writerClosed && abortStatus == null) }

    fun getWriterAbortStatus(): Status? = abortStatus?.takeIf { !it.isOk }

    suspend fun size(): StatusOr<Int> = mutex.withLock { Ok(nextWriteSeq.toInt()) }

    fun attachStream(stream: WritableWireStream): Status {
        synchronized(streams) { if (stream !in streams) streams.add(stream) }
        return Status.ok()
    }

    fun detachStream(stream: WritableWireStream): Status {
        synchronized(streams) { streams.remove(stream) }
        return Status.ok()
    }

    private fun streamsSnapshot(): List<WritableWireStream> = synchronized(streams) { streams.toList() }

    fun cancelReader(): Status {
        readerCancelled = true
        return Status.ok()
    }

    suspend fun cancelWriter(): Status = drainAndClose()

    // --- consuming half ------------------------------------------------------

    /** Read the next raw fragment in seq order, or null at clean end of stream. */
    suspend fun nextFragment(timeoutMs: Long? = null): StatusOr<NodeFragment?> {
        val deadline = timeoutMs?.let { System.currentTimeMillis() + it }
        while (true) {
            val waiter: Deferred<Unit>
            mutex.withLock {
                abortStatus?.let { if (!it.isOk) return it }
                if (readerCancelled) return Ok(null)
                val fragment = bySeq.remove(nextReadSeq)
                if (fragment != null) {
                    nextReadSeq += 1
                    return Ok(fragment)
                }
                val fin = finalSeq
                if (fin != null && nextReadSeq > fin) return Ok(null)
                if (writerClosed && nextReadSeq >= nextWriteSeq) return Ok(null)
                waiter = Deferred<Unit>().also { changeWaiters.add(it) }
            }
            val remaining = deadline?.let { it - System.currentTimeMillis() }
            if (remaining != null && remaining <= 0) return deadlineExceeded("AsyncNode read timed out.")
            when (val waited = waitFor(remaining) { waiter.await() }) {
                is Ok -> Unit
                is Status -> return waited
            }
        }
    }

    suspend fun nextChunk(timeoutMs: Long? = null): StatusOr<Chunk?> {
        val fragment = nextFragment(timeoutMs).orElse { return it } ?: return Ok(null)
        return fragment.getChunk().let { if (it is Ok) Ok(it.value) else it as Status }
    }

    /**
     * The next fragment carrying a value, or null once the node ends.
     *
     * A null chunk is a marker, not a value: a final one says the node is
     * finished, and a non-final one says nothing at all. Neither is something a
     * reader asked for, so both are skipped here rather than surfaced as a
     * value or rejected — which is what lets a node be closed with nothing in
     * it.
     */
    private suspend fun nextValueFragment(timeoutMs: Long?): StatusOr<NodeFragment?> {
        val started = System.currentTimeMillis()
        while (true) {
            val remaining = timeoutMs?.let { maxOf(0L, it - (System.currentTimeMillis() - started)) }
            val fragment = nextFragment(remaining).orElse { return it } ?: return Ok(null)
            val chunk = fragment.getChunk().orElse { return it }
            if (!chunk.isNull) return Ok(fragment)
            if (!fragment.continued) return Ok(null)
        }
    }

    /** Read one value, or null at finality / clean closure. */
    suspend fun next(
        timeoutMs: Long? = null,
        mimetypePatterns: List<String>? = null,
        expectedTag: String? = null,
    ): StatusOr<Any?> {
        val fragment = nextValueFragment(timeoutMs).orElse { return it } ?: return Ok(null)
        val chunk = fragment.getChunk().orElse { return it }
        return registry.fromChunk(chunk, mimetypePatterns ?: expectedPatterns, expectedTag ?: this.expectedTag)
    }

    /**
     * Consume exactly one whole unary value and validate its terminator.
     *
     * Two spellings are accepted: the value written as final, or the value
     * followed by a null final chunk. With [allowNone] a node holding no value
     * — closed empty, or holding nothing but a null final — yields null.
     */
    suspend fun consume(
        timeoutMs: Long? = null,
        allowNone: Boolean = false,
        mimetypePatterns: List<String>? = null,
        expectedTag: String? = null,
    ): StatusOr<Any?> {
        val first = nextValueFragment(timeoutMs).orElse { return it }
            ?: return if (allowNone) Ok(null) else failedPrecondition("AsyncNode is empty at the current reader offset.")
        val chunk = first.getChunk().orElse { return it }
        if (first.continued) {
            val terminator = nextFragment(timeoutMs).orElse { return it }
                ?: return failedPrecondition("A continued consumed value must be followed by a null final chunk.")
            val terminatorChunk = terminator.getChunk().orElse { return it }
            if (terminator.continued || !terminatorChunk.isNull) {
                return failedPrecondition("The only fragment allowed after a consumed value is a null final chunk.")
            }
        }
        return registry.fromChunk(chunk, mimetypePatterns ?: expectedPatterns, expectedTag ?: this.expectedTag)
    }

    private fun notifyChange() {
        val waiters = changeWaiters.toList()
        changeWaiters.clear()
        for (w in waiters) w.resolve(Unit)
    }

    companion object {
        fun create(id: String, registry: SerializationRegistry = SerializationRegistry.getGlobal()): StatusOr<AsyncNode> {
            validateName(id).let { if (!it.isOk) return it }
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
        validateName(nodeId).let { if (!it.isOk) return it }
        synchronized(lock) {
            nodes[nodeId]?.let { return Ok(it) }
            val node = AsyncNode.create(nodeId).orElse { return it }
            nodes[nodeId] = node
            return Ok(node)
        }
    }

    fun getIfExists(nodeId: String): StatusOr<AsyncNode?> {
        validateName(nodeId).let { if (!it.isOk) return it }
        return Ok(synchronized(lock) { nodes[nodeId] })
    }

    fun discard(nodeId: String, expected: AsyncNode? = null): StatusOr<AsyncNode?> {
        validateName(nodeId).let { if (!it.isOk) return it }
        synchronized(lock) {
            val found = nodes[nodeId] ?: return Ok(null)
            if (expected != null && found !== expected) return Ok(null)
            nodes.remove(nodeId)
            return Ok(found)
        }
    }

    fun contains(nodeId: String): Boolean = synchronized(lock) { nodes.containsKey(nodeId) }
    val size: Int get() = synchronized(lock) { nodes.size }
}
