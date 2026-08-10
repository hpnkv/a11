package a11

import a11.net.ABORT_STATUS_HEADER
import a11.net.WireStream

/** Terminal trailer/header carrying a session's structured completion status. */
const val SESSION_STATUS_HEADER = "x-a11-session-status"

/** Which endpoint role a session asks an attached stream to drive. */
enum class StreamMode { START, ACCEPT }

/** Consumes one inbound message with its owning stream; `null` = half-close. */
typealias OnSessionStreamMessage = suspend (WireMessage?, WireStream, Session) -> Status

/** Runs after an attached stream has completely terminated. */
typealias OnSessionStreamDone = suspend (WireStream, Session) -> Status

private class StreamState(val stream: WireStream, val id: String) {
    var remoteHalfClosed = false
    var done = false
}

/**
 * Connection-scoped runtime that turns wire traffic into agent work.
 *
 * A session owns a shared [NodeMap], an optional [ActionRegistry], and one or
 * more [WireStream]s. Incoming `WireMessage`s are split into action calls and
 * node fragments and dispatched; outgoing messages round-robin across streams.
 * Ported from `js/src/session.ts` (auto-dispatch path); the reserved
 * `__status__`/`__dispatch_status__` node protocol and the session-status
 * trailer are preserved so it interoperates with the Python/JS peers.
 */
class Session(
    private val nodeMap: NodeMap = NodeMap(),
    private var actionRegistry: ActionRegistry? = null,
    private val id: String = randomId("session-"),
    private val headers: ByteMap = ByteMap(),
    onStreamMessage: OnSessionStreamMessage? = null,
    onStreamDone: OnSessionStreamDone? = null,
) : ActionSessionContext {

    private val streams = mutableListOf<StreamState>()
    private val streamsById = HashMap<String, StreamState>()
    private var roundRobin = 0
    private val activeActions = HashMap<String, Action>()
    private var phase = Phase.OPEN
    private var sessionStatus: Status = Status.ok()
    private var remoteClosed = false
    private val doneDeferred = Deferred<Status>()

    private val onMessage: OnSessionStreamMessage =
        onStreamMessage ?: { message, stream, _ ->
            if (message == null) Status.ok() else dispatchWireMessage(message, stream)
        }
    private val onDone: OnSessionStreamDone = onStreamDone ?: { _, _ -> Status.ok() }

    private enum class Phase { OPEN, CLOSING, ABORTED }

    override fun getNodeMap(): NodeMap = nodeMap
    override fun getActionRegistry(): ActionRegistryLike? = actionRegistry
    fun getId(): String = id
    fun getStatus(): Status = sessionStatus
    fun isClosed(): Boolean = remoteClosed || phase != Phase.OPEN
    fun done(): Deferred<Status> = doneDeferred

    fun setActionRegistry(registry: ActionRegistry?): Status { actionRegistry = registry; return Status.ok() }

    fun getAction(actionId: String): StatusOr<Action> =
        activeActions[actionId]?.let { Ok(it) } ?: notFound("Action '$actionId' is not active in the Session.")

    // --- ActionSessionContext ------------------------------------------------

    override fun trackAction(action: Action): Status {
        if (phase != Phase.OPEN || remoteClosed) return failedPrecondition("Session is no longer accepting Actions.")
        val existing = activeActions[action.getId()]
        if (existing != null && existing !== action) return alreadyExists("Action '${action.getId()}' already exists.")
        activeActions[action.getId()] = action
        return Status.ok()
    }

    override fun untrackAction(action: Action) {
        if (activeActions[action.getId()] === action) activeActions.remove(action.getId())
    }

    override suspend fun acquireActionSlot(nested: Boolean): Status = Status.ok()
    override fun releaseActionSlot(nested: Boolean) {}

    override fun send(message: WireMessage, streamId: String): Status {
        if (phase != Phase.OPEN) return failedPrecondition("Messages cannot be sent after the Session ends.")
        val target = if (streamId.isNotEmpty()) {
            streamsById[streamId]?.stream ?: return notFound("Session stream was not found.")
        } else {
            val available = streams.filter { !it.done }
            if (available.isEmpty()) return notFound("Session has no attached streams.")
            val index = roundRobin % available.size
            roundRobin = (index + 1) % available.size
            available[index].stream
        }
        return target.send(message)
    }

    // --- stream attachment ---------------------------------------------------

    suspend fun addStream(stream: WireStream, mode: StreamMode = StreamMode.START): Status {
        val streamId = stream.getId()
        validateName(streamId).let { if (!it.isOk) return it }
        if (phase != Phase.OPEN || remoteClosed) return failedPrecondition("No streams can be attached after the Session ends.")
        if (streamsById.containsKey(streamId)) return alreadyExists("Stream is already attached to the Session.")
        val state = StreamState(stream, streamId)
        streams.add(state)
        streamsById[streamId] = state

        val onMsg: a11.net.OnWireMessage = { message -> handleStreamMessage(state, message) }
        val onDn: a11.net.OnWireDone = { handleStreamDone(state) }
        val startup = if (mode == StreamMode.START) stream.start(onMsg, onDn) else stream.accept(onMsg, onDn)
        if (!startup.isOk) removeStream(state)
        return startup
    }

    private suspend fun handleStreamMessage(state: StreamState, message: WireMessage?): Status {
        if (message == null) {
            state.remoteHalfClosed = true
            state.stream.getTrailers()?.get(SESSION_STATUS_HEADER)?.let { encoded ->
                val decoded = decodeStatus(encoded).orElse { return it }
                if (!decoded.status.isOk) return failedPrecondition("A peer must abort, not half-close, a failed Session.")
                remoteClosed = true
            }
            return onMessage(null, state.stream, this)
        }
        if (state.remoteHalfClosed) return failedPrecondition("WireStream delivered data after its remote half-close.")
        return onMessage(message, state.stream, this)
    }

    private suspend fun handleStreamDone(state: StreamState): Status {
        val streamStatus = state.stream.getStatus()
        if (!streamStatus.isOk && phase != Phase.ABORTED) {
            phase = Phase.ABORTED
            sessionStatus = sessionStatusFromTrailers(state.stream) ?: streamStatus
            cancelAllActions()
        }
        val status = onDone(state.stream, this)
        removeStream(state)
        return status
    }

    private fun removeStream(state: StreamState) {
        if (state.done) return
        state.done = true
        streams.remove(state)
        if (streamsById[state.id] === state) streamsById.remove(state.id)
        if (streams.isEmpty()) finishIfPossible()
    }

    // --- dispatch ------------------------------------------------------------

    suspend fun dispatchWireMessage(message: WireMessage, originStream: WireStream? = null): Status {
        message.validate().let { if (!it.isOk) return it }
        var first: Status = Status.ok()
        for (action in message.actions) {
            val status = dispatchActionMessage(action, originStream)
            if (!status.isOk) first = firstError(first, status)
        }
        for (fragment in message.nodeFragments) {
            val status = dispatchNodeFragment(fragment)
            if (status is Status && !status.isOk) {
                val separator = fragment.id.indexOf('#')
                if (separator >= 0) getAction(fragment.id.substring(0, separator)).let { if (it is Ok) it.value.cancel() }
                first = firstError(first, status)
            }
        }
        return first
    }

    /** Resolve, acknowledge, and start one inbound registered action call. */
    suspend fun dispatchActionMessage(message: ActionMessage, originStream: WireStream?): Status {
        message.validate().let { if (!it.isOk) return it }
        if (message.name == CANCEL_ACTION_NAME) {
            val encodedId = message.headers.entries.firstOrNull { it.key.lowercase() == CANCEL_ACTION_HEADER }?.value
                ?: return invalidArgument("Cancel Action requires the __action header.")
            val actionId = utf8Decode(encodedId).orElse { return it }
            return when (val action = getAction(actionId)) {
                is Ok -> action.value.cancel()
                is Status -> if (action.code == StatusCode.NOT_FOUND) Status.ok() else action
            }
        }

        var dispatchStatus: Status = Status.ok()
        var action: Action? = null
        val registry = actionRegistry
        when {
            phase != Phase.OPEN || remoteClosed -> dispatchStatus = failedPrecondition("Session is no longer accepting Actions.")
            activeActions.containsKey(message.id) -> dispatchStatus = alreadyExists("Action already exists in the Session.")
            registry == null -> dispatchStatus = failedPrecondition("Session has no ActionRegistry.")
            else -> when (val created = registry.makeAction(message.name, MakeActionOptions(id = message.id, nodeMap = nodeMap, stream = originStream, session = this))) {
                is Ok -> action = created.value
                is Status -> dispatchStatus = created
            }
        }
        val act = action
        if (dispatchStatus.isOk && act != null) {
            dispatchStatus = act.mapPortsFromMessage(message)
            if (dispatchStatus.isOk) for ((name, value) in message.headers) {
                dispatchStatus = act.setHeader(name, value)
                if (!dispatchStatus.isOk) break
            }
            if (dispatchStatus.isOk) dispatchStatus = act.applyInputAutofills()
            if (dispatchStatus.isOk) dispatchStatus = when (val r = act.run()) { is Ok -> Status.ok(); is Status -> r }
        }

        if (originStream != null) {
            val chunk = statusToChunk(dispatchStatus).orElse { return it }
            val dispatchId = Action.makeNodeId(message.id, ACTION_DISPATCH_STATUS_OUTPUT).orElse { return it }
            val fragments = mutableListOf(NodeFragment(dispatchId, chunk, 0, false))
            if (!dispatchStatus.isOk) {
                val statusId = Action.makeNodeId(message.id, ACTION_STATUS_OUTPUT).orElse { return it }
                fragments.add(NodeFragment(statusId, chunk, 0, false))
            }
            return originStream.send(WireMessage(nodeFragments = fragments))
        }
        return dispatchStatus
    }

    /** Apply a received fragment, including reserved action-status nodes. */
    suspend fun dispatchNodeFragment(fragment: NodeFragment): StatusOr<Long> {
        fragment.validate().let { if (!it.isOk) return it }
        val special = specialActionNode(fragment.id)
        val chunk = fragment.getChunk()
        var protocolStatus: Status? = null
        var action: Action? = null

        // A closure marker reports that the peer drained the node and closed its
        // write half; it carries no value, so it is applied to the local mirror
        // rather than stored. Checked before the reserved status nodes so that
        // closing an Action's status node is not read as a second status value.
        if (chunk is Ok && isCloseStatusChunk(chunk.value)) {
            val closed = decodeStatusChunk(chunk.value).orElse { return it }.status
            val seq = Ok(fragment.seq ?: 0L)
            // Dropping a marker for a released node loses nothing, whereas
            // creating one would resurrect it.
            val mirror = nodeMap.getIfExists(fragment.id).orElse { return it } ?: return seq
            if (!mirror.isWritable().orElse { return it }) return seq
            val applied = if (closed.isOk) mirror.drainAndClose() else mirror.abortWithStatus(closed)
            if (!applied.isOk) return applied
            return seq
        }

        if (special != null) {
            if (chunk !is Ok || !isStatusChunk(chunk.value)) return invalidArgument("An Action status node requires a status Chunk.")
            protocolStatus = decodeStatusChunk(chunk.value).orElse { return it }.status
            action = getAction(special.first).orElse { return notFound("Received status for an unknown Action.") }
        }

        val node = nodeMap.get(fragment.id).orElse { return it }
        if (chunk is Ok && isStatusChunk(chunk.value) && special == null) {
            val decoded = decodeStatusChunk(chunk.value).orElse { return it }
            if (decoded.status.isOk) return invalidArgument("An ordinary node cannot be aborted with an OK status.")
            if (node.isWritable().orElse { return it }) node.abortWithStatus(decoded.status)
            return Ok(fragment.seq ?: 0L)
        }
        if (node.getWriterAbortStatus() != null) return Ok(fragment.seq ?: 0L)
        val stored = node.putFragment(fragment)
        if (stored is Status && !stored.isOk) return stored
        if (special != null && action != null && protocolStatus != null) {
            val applied = if (special.second == ACTION_DISPATCH_STATUS_OUTPUT) action.setDispatchStatus(protocolStatus)
            else action.setCompletionStatus(protocolStatus)
            if (!applied.isOk) return applied
        }
        return stored
    }

    // --- lifecycle -----------------------------------------------------------

    fun halfClose(): Status {
        if (phase != Phase.OPEN) return Status.ok()
        val packed = packStatus(Status.ok()).orElse { return it }
        val trailers = copyByteMap(headers).apply { put(SESSION_STATUS_HEADER, packed) }
        phase = Phase.CLOSING
        var first: Status = Status.ok()
        for (state in streams.toList()) if (!state.done) first = firstError(first, state.stream.halfClose(trailers))
        finishIfPossible()
        return first
    }

    fun abort(status: Status): Status {
        if (status.isOk) return invalidArgument("An aborted Session needs a non-OK status.")
        if (phase != Phase.OPEN) return Status.ok()
        val sessionPacked = packStatus(status).orElse { return it }
        val streamAbort = aborted("Session has aborted its streams")
        val streamAbortPacked = packStatus(streamAbort).orElse { return it }
        val terminalHeaders = copyByteMap(headers).apply {
            put(SESSION_STATUS_HEADER, sessionPacked)
            put(ABORT_STATUS_HEADER, streamAbortPacked)
        }
        phase = Phase.ABORTED
        sessionStatus = status
        cancelAllActions()
        var first: Status = Status.ok()
        for (state in streams.toList()) {
            if (state.done) continue
            val sent = state.stream.send(WireMessage(headers = terminalHeaders))
            if (!sent.isOk) { first = firstError(first, sent); state.stream.abort(streamAbort) }
        }
        finishIfPossible()
        return first
    }

    private fun cancelAllActions() {
        for (action in activeActions.values.toList()) action.cancel()
    }

    private fun finishIfPossible() {
        if ((phase == Phase.OPEN && !remoteClosed) || streams.isNotEmpty()) return
        doneDeferred.resolve(sessionStatus)
    }

    private fun sessionStatusFromTrailers(stream: WireStream): Status? {
        val encoded = stream.getTrailers()?.get(SESSION_STATUS_HEADER) ?: return null
        return when (val decoded = decodeStatus(encoded)) { is Ok -> decoded.value.status; is Status -> null }
    }

    private fun specialActionNode(nodeId: String): Pair<String, String>? {
        for (name in listOf(ACTION_DISPATCH_STATUS_OUTPUT, ACTION_STATUS_OUTPUT)) {
            val suffix = "#$name"
            if (nodeId.length > suffix.length && nodeId.endsWith(suffix)) {
                return nodeId.substring(0, nodeId.length - suffix.length) to name
            }
        }
        return null
    }
}
