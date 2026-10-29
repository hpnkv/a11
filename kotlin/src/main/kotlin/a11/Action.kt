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
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Asynchronous application work invoked by [Action.run], returning its completion status. */
typealias ActionHandler = suspend (Action) -> Status

/** Synchronous hook run when cancellation is first requested. */
typealias OnActionCancelled = (Action) -> Status

/** Collaborators and instance policy supplied when creating an [Action]. */
data class ActionCreateOptions(
    val id: String? = null,
    val handler: ActionHandler? = null,
    val nodeMap: NodeMap? = null,
    val stream: WireStream? = null,
    val session: Session? = null,
    val registry: ActionRegistry? = null,
    val settings: ActionSettings = ActionSettings(),
)

private enum class ActionMode {
    NONE,
    RUN,
    CALL,
    CANCELLED,
}

/** One schema-described unit of local work or remote agent work. */
class Action private constructor(schema: ActionSchema, options: ActionCreateOptions, id: String) {
    var schema: ActionSchema = schema
        private set

    var id: String = id
        private set

    var handler: ActionHandler? = options.handler
        private set

    var session: Session? = options.session
        private set

    var nodeMap: NodeMap = options.nodeMap ?: session?.getNodeMap() ?: NodeMap()
        private set

    var stream: WireStream? = options.stream
        private set

    var registry: ActionRegistry? = options.registry ?: session?.getActionRegistry()
        private set

    private var settings: ActionSettings = options.settings.copy()

    private val headers: ByteMap = ByteMap()
    private val inputIds = LinkedHashMap<String, String>()
    private val outputIds = LinkedHashMap<String, String>()
    private val boundNodes = mutableSetOf<AsyncNode>()
    private var mode = ActionMode.NONE
    @Volatile private var completionStatus: Status? = null
    private var dispatchOutcome: Status? = null
    private val cancellationRequested = AtomicBoolean(false)
    private val cancelRequested: Boolean
        get() = cancellationRequested.get()

    private val finishing = AtomicBoolean(false)
    @Volatile private var handlerJob: Job? = null
    private var inputAutofillsApplied = false
    private var tracked = false
    // Set once a consumer has taken the log port through getLogNode(). It then
    // owns presentation, so log() stops reporting to the process sink.
    private var logClaimed = false
    private val done = Deferred<Status>()
    private val dispatched = Deferred<Status>()
    private val cancelCallbacks = mutableListOf<OnActionCancelled>()
    private var parent: Action? = null

    init {
        for ((name, header) in schema.headers) {
            header.defaultValue?.let { value -> headers[name.lowercase()] = value.copyOf() }
        }
        remapDefaultPorts()
    }

    val isCancelled: Boolean
        get() = cancelRequested || completionStatus?.code == StatusCode.CANCELLED

    val status: Status
        get() = completionStatus ?: Status.ok()

    val dispatchStatus: Status?
        get() = dispatchOutcome

    val isDone: Boolean
        get() = completionStatus != null

    fun setId(id: String): Status {
        validateName(id).orElse { status ->
            return status
        }
        if (mode != ActionMode.NONE)
            return failedPrecondition("Cannot change Action id after it has started.")
        this.id = id
        return remapDefaultPorts()
    }

    fun bindHandler(handler: ActionHandler): Status {
        if (mode != ActionMode.NONE)
            return failedPrecondition("Cannot change Action handler after it has started.")
        this.handler = handler
        return Status.ok()
    }

    fun bindNodeMap(nodeMap: NodeMap): Status {
        this.nodeMap = nodeMap
        return Status.ok()
    }

    fun bindRegistry(registry: ActionRegistry?): Status {
        this.registry = registry
        return Status.ok()
    }

    fun bindSession(session: Session?): Status {
        if (!tracked || this.session === session) {
            this.session = session
            return Status.ok()
        }
        if (session != null)
            session.trackAction(this).orElse { status ->
                return status
            }
        this.session?.untrackAction(this)
        this.session = session
        tracked = session != null
        return Status.ok()
    }

    fun bindStream(stream: WireStream?): Status {
        if (stream === this.stream) return Status.ok()
        val previous = this.stream
        for (node in boundNodes) {
            previous?.let { value -> node.detachStream(value) }
            stream?.let { value -> node.attachStream(value) }
        }
        this.stream = stream
        return Status.ok()
    }

    // --- port access ---------------------------------------------------------

    suspend fun getInput(name: String, bindStream: Boolean? = null): StatusOr<AsyncNode> {
        val id = inputIds[name] ?: return notFound("Action input '$name' is not mapped.")
        val node =
            nodeMap.get(id).orElse { status ->
                return status
            }
        val bind = bindStream ?: settings.bindStreamsOnInputsByDefault ?: (mode != ActionMode.RUN)
        attachStreamIfRequested(node, bind)
        return Ok(node)
    }

    suspend fun getOutput(name: String, bindStream: Boolean? = null): StatusOr<AsyncNode> {
        val id = outputIds[name] ?: return notFound("Action output '$name' is not mapped.")
        val node =
            nodeMap.get(id).orElse { status ->
                return status
            }
        val bind = bindStream ?: settings.bindStreamsOnOutputsByDefault ?: (mode == ActionMode.RUN)
        attachStreamIfRequested(
            node,
            bind && name != ACTION_STATUS_OUTPUT && name != ACTION_DISPATCH_STATUS_OUTPUT,
        )
        return Ok(node)
    }

    /**
     * Log [value] on the reserved [ACTION_LOG_OUTPUT] port.
     *
     * The value becomes a chunk the way `node.put(value)` would make one -- a `String` is
     * `text/plain`, a `ByteArray` is `application/octet-stream` -- and the chunk always carries a
     * timestamp.
     *
     * Only a running handler may log: logging before `run`, or on the calling side of a `call`, is
     * a failed precondition, because the port would have nowhere to go and no reader to close it.
     * Nothing else about logging fails the action -- once the chunk is built, a transport or
     * lifecycle problem is reported through the sink rather than returned.
     *
     * Where it goes: always to the process's action log sink, and additionally onto the log port
     * when something could read it -- a peer is attached, or a local consumer claimed the port with
     * [getLogNode]. Nobody has to drain it and nobody has to close it.
     */
    suspend fun log(value: Any?, options: LogOptions = LogOptions()): Status {
        val chunk: Chunk
        if (value is Chunk) {
            if (options.mimetype.isNotEmpty()) {
                return invalidArgument(
                    "Cannot give a log mimetype for a chunk that already has one."
                )
            }
            chunk = value
        } else {
            chunk =
                SerializationRegistry.getGlobal().toChunk(value, options.mimetype).orElse { status
                    ->
                    return status
                }
        }
        return writeLog(chunk, options)
    }

    /**
     * Log a formatted line: `%s` is replaced by each argument in turn.
     *
     * Uses positional `%s` replacements and `%%` for literal percent signs across language
     * runtimes.
     */
    suspend fun logf(format: String, vararg args: Any?): Status =
        logfWith(LogOptions(), format, *args)

    /**
     * Log a formatted line with explicit options.
     *
     * A second name rather than an overload, so it matches the C++ surface, where a leading-options
     * overload of `Logf` is ambiguous against the format spec.
     */
    suspend fun logfWith(options: LogOptions, format: String, vararg args: Any?): Status {
        val filled = StringBuilder()
        var index = 0
        var at = 0
        while (at < format.length) {
            val char = format[at]
            if (char == '%' && at + 1 < format.length) {
                when (format[at + 1]) {
                    '%' -> {
                        filled.append('%')
                        at += 2
                        continue
                    }
                    's' -> {
                        filled.append(if (index < args.size) args[index++].toString() else "")
                        at += 2
                        continue
                    }
                }
            }
            filled.append(char)
            at += 1
        }
        return log(filled.toString(), options)
    }

    /**
     * Return the log port's node, claiming it for this consumer.
     *
     * Claiming suppresses the process sink for this action, so a consumer that presents the logs
     * itself does not also have them reported twice. Claim before the action runs: logs written
     * earlier have already gone to the sink.
     *
     * The stream is not bound: on the calling side, binding an output would echo received fragments
     * back to the peer.
     */
    suspend fun getLogNode(): StatusOr<AsyncNode> {
        logClaimed = true
        return getOutput(ACTION_LOG_OUTPUT, bindStream = false)
    }

    /** Apply [options] to [chunk], report it, and write it where anything reads. */
    private suspend fun writeLog(chunk: Chunk, options: LogOptions): Status {
        val level =
            parseLogLevel(options.level)
                ?: return invalidArgument(
                    "Unknown log level '${options.level}'; expected one of " +
                        LOG_LEVELS.joinToString(", ") +
                        "."
                )
        if (mode != ActionMode.RUN) {
            return failedPrecondition(
                "Only a running Action may log; a caller logs on its own action."
            )
        }
        val metadata = chunk.metadata ?: ChunkMetadata()
        if (metadata.mimetype.isEmpty()) metadata.mimetype = OCTET_STREAM_MIMETYPE
        if (isStatusChunk(chunk)) {
            return invalidArgument("Cannot log a status chunk; log its message instead.")
        }
        metadata.timestampMillis = System.currentTimeMillis()
        // The caller's map first, then the named options, so an explicit level
        // wins over a "level" the same caller also put in the map.
        options.metadata?.forEach { (key, value) -> metadata.attributes[key] = value }
        metadata.attributes[LOG_LEVEL_ATTRIBUTE] = level.toByteArray()
        metadata.attributes[LOG_INTERNAL_ATTRIBUTE] =
            (if (options.internal) LOG_INTERNAL_TRUE else LOG_INTERNAL_FALSE).toByteArray()
        if (options.channel.isNotEmpty())
            metadata.attributes[LOG_CHANNEL_ATTRIBUTE] = options.channel.toByteArray()
        if (options.file.isNotEmpty())
            metadata.attributes[LOG_FILE_ATTRIBUTE] = options.file.toByteArray()
        options.lineno?.let { value ->
            metadata.attributes[LOG_LINENO_ATTRIBUTE] = value.toString().toByteArray()
        }
        chunk.metadata = metadata

        if (!logClaimed) reportLog(logRecordFromChunk(chunk, schema.name, id))
        // Nothing reads a local log port nobody claimed, so materialising it
        // would buffer every line of a narrating action for the length of the
        // run and then throw them away. A peer is always a reader: it is
        // mirroring the node.
        val readable = logClaimed || stream != null || session != null
        if (!readable || finishing.get()) return Status.ok()

        // From here on nothing is returned to the handler: a log that could not
        // be written is a fault in the logging, not in the action.
        val node =
            getOutput(ACTION_LOG_OUTPUT, bindStream = stream != null).orElse {
                return Status.ok()
            }
        node.putChunk(chunk)
        return Status.ok()
    }

    fun containsPort(name: String): Boolean =
        inputIds.containsKey(name) || outputIds.containsKey(name)

    /** Snapshot the call id, name, port mappings, and headers for dispatch. */
    fun getActionMessage(): ActionMessage =
        ActionMessage(
            id = id,
            name = schema.name,
            inputs =
                schema.inputs.keys
                    .map { value -> Port(value, inputIds[value] ?: "") }
                    .toMutableList(),
            outputs =
                schema.outputs.keys
                    .map { value -> Port(value, outputIds[value] ?: "") }
                    .toMutableList(),
            headers = copyByteMap(headers),
        )

    /** Adopt validated caller-supplied port node ids before local execution. */
    fun mapPortsFromMessage(message: ActionMessage): Status {
        if (mode != ActionMode.NONE)
            return failedPrecondition("Cannot remap Action ports after it has started.")
        message.validate().orElse { status ->
            return status
        }
        for (port in message.inputs) {
            if (!schema.inputs.containsKey(port.name))
                return failedPrecondition("Unknown Action input port '${port.name}'.")
            inputIds[port.name] = port.id
        }
        for (port in message.outputs) {
            if (!schema.outputs.containsKey(port.name))
                return failedPrecondition("Unknown Action output port '${port.name}'.")
            outputIds[port.name] = port.id
        }
        return Status.ok()
    }

    // --- headers -------------------------------------------------------------

    fun getHeaders(): ByteMap = copyByteMap(headers)

    fun getHeader(name: String): StatusOr<ByteArray?> {
        validateName(name).orElse { status ->
            return status
        }
        return Ok(headers[name.lowercase()]?.copyOf())
    }

    fun setHeader(name: String, value: ByteArray): Status {
        validateName(name).orElse { status ->
            return status
        }
        headers[name.lowercase()] = value.copyOf()
        return Status.ok()
    }

    fun setHeader(name: String, value: String): Status = setHeader(name, utf8Encode(value))

    fun removeHeader(name: String): Status {
        validateName(name).orElse { status ->
            return status
        }
        headers.remove(name.lowercase())
        return Status.ok()
    }

    fun forwardHeadersWithPrefix(target: Action, prefix: String = ACTION_HEADER_PREFIX): Status {
        val folded = prefix.lowercase()
        for ((name, value) in headers) if (
            name.startsWith(folded) && name != AUTHORIZATION_REFERENCE_HEADER
        )
            target.setHeader(name, value).orElse { status ->
                return status
            }
        return Status.ok()
    }

    // --- nested --------------------------------------------------------------

    fun makeNested(
        name: String,
        propagateIo: Boolean = true,
        forwardHeaders: Boolean = true,
    ): StatusOr<Action> {
        val reg =
            registry
                ?: return failedPrecondition("Cannot resolve a nested Action without a registry.")

        val schema =
            reg.getSchema(name).orElse { status ->
                return status
            }
        val handler =
            when (val h = reg.getHandler(name)) {
                is Ok -> h.value
                is Err -> if (h.status.code == StatusCode.NOT_FOUND) null else return h
            }
        val child =
            create(
                    schema,
                    ActionCreateOptions(
                        handler = handler,
                        nodeMap = if (propagateIo) nodeMap else null,
                        stream = if (propagateIo) stream else null,
                        session = if (propagateIo) session else null,
                        registry = registry,
                    ),
                )
                .orElse { status ->
                    return status
                }
        child.parent = this
        if (forwardHeaders)
            forwardHeadersWithPrefix(child).orElse { status ->
                return status
            }
        return Ok(child)
    }

    // --- lifecycle -----------------------------------------------------------

    /** Start the bound handler locally and return immediately. */
    fun run(): StatusOr<Action> {
        if (handler == null) return failedPrecondition("Action handler has not been set.")
        begin(ActionMode.RUN).orElse { status ->
            return status
        }
        trackInSession().orElse { status ->
            mode = ActionMode.NONE
            return status
        }
        handlerJob = A11Runtime.scope.launch(start = CoroutineStart.LAZY) { runHandler() }
        handlerJob!!.start()
        return Ok(this)
    }

    /** Queue this action for remote dispatch; use [waitForDispatch] for acceptance. */
    suspend fun call(wireHeaders: ByteMap = ByteMap()): StatusOr<Action> {
        begin(ActionMode.CALL).orElse { status ->
            return status
        }
        trackInSession().orElse { status ->
            mode = ActionMode.NONE
            return status
        }
        val autofills = collectAutofillFragments()
        val message =
            WireMessage(
                nodeFragments = autofills.toMutableList(),
                actions = mutableListOf(getActionMessage()),
                headers = copyByteMap(wireHeaders),
            )
        val sent =
            stream?.send(message)
                ?: session?.send(message)
                ?: failedPrecondition(
                    "Calling an Action requires an attached WireStream or Session."
                )
        sent.orElse { status ->
            untrackFromSession()
            mode = ActionMode.NONE
            return status
        }
        return Ok(this)
    }

    suspend fun waitForDispatch(timeoutMs: Long? = null): Status {
        if (mode != ActionMode.CALL)
            return failedPrecondition("Only a called Action has a dispatch status.")
        return when (
            val r = waitFor(timeoutMs, "Action dispatch timed out.") { dispatched.await() }
        ) {
            is Ok -> r.value
            is Err -> r.status
        }
    }

    suspend fun wait(timeoutMs: Long? = null): StatusOr<Action> {
        if (mode == ActionMode.NONE) return failedPrecondition("Action has not been run or called.")
        val completed =
            waitFor(timeoutMs, "Action wait timed out.") { done.await() }
                .orElse { status ->
                    return status
                }
        completed.orElse { status ->
            return status
        }
        return Ok(this)
    }

    fun cancel(): Status {
        if (
            completionStatus != null ||
                finishing.get() ||
                !cancellationRequested.compareAndSet(false, true)
        )
            return Status.ok()
        var first = Status.ok()
        for (callback in cancelCallbacks) {
            val status =
                try {
                    callback(this)
                } catch (error: Exception) {
                    Status.fromException(error)
                }
            first = first.updated(status)
        }
        val cancelledStatus = cancelled("Action was cancelled.")
        when (mode) {
            ActionMode.CALL -> {
                sendRemoteCancel()
                completeCall(cancelledStatus, removeFromSession = false)
            }
            ActionMode.RUN -> {
                handlerJob?.cancel()
                A11Runtime.scope.launch { finishRun(cancelledStatus) }
            }
            ActionMode.NONE -> {
                mode = ActionMode.CANCELLED
                completionStatus = cancelledStatus
                done.resolve(cancelledStatus)
            }
            ActionMode.CANCELLED -> {}
        }
        return first
    }

    fun setOnCancelled(callback: OnActionCancelled): Status {
        cancelCallbacks.add(callback)
        return Status.ok()
    }

    /** Session protocol hook for the reserved dispatch-status node. */
    fun setDispatchStatus(status: Status): Status {
        if (mode != ActionMode.CALL || dispatchOutcome != null) return Status.ok()
        dispatchOutcome = status
        dispatched.resolve(status)
        return Status.ok()
    }

    /** Session protocol hook for the reserved completion-status node. */
    fun setCompletionStatus(status: Status): Status {
        if (mode != ActionMode.CALL) return failedPrecondition("Action is not a call.")
        if (dispatchOutcome == null) {
            dispatchOutcome = Status.ok()
            dispatched.resolve(Status.ok())
        }
        if (completionStatus == null) completeCall(status, removeFromSession = true)
        else if (cancelRequested) untrackFromSession()
        return Status.ok()
    }

    suspend fun applyInputAutofills(): Status {
        if (inputAutofillsApplied) return Status.ok()
        for ((name, port) in schema.inputs) {
            if (port.autofills.isEmpty()) continue
            val id = inputIds[name] ?: continue
            val node =
                nodeMap.get(id).orElse { status ->
                    return status
                }
            for (autofill in port.autofills) {
                if (autofill == null) {
                    node.finalize(wait = true, close = false).orElse { status ->
                        return status
                    }
                } else {
                    val stored =
                        node.putFragment(
                            NodeFragment(id, autofill.data, autofill.seq, autofill.continued)
                        )
                    if (stored is Err) return stored.status
                }
            }
            node.close().orElse { status ->
                return status
            }
        }
        inputAutofillsApplied = true
        return Status.ok()
    }

    fun clearInputsAfterRun(clear: Boolean = true): Status {
        settings.clearInputsAfterRun = clear
        return Status.ok()
    }

    fun clearOutputsAfterRun(clear: Boolean = true): Status {
        settings.clearOutputsAfterRun = clear
        return Status.ok()
    }

    // --- internals -----------------------------------------------------------

    private fun begin(mode: ActionMode): Status {
        if (cancelRequested) return cancelled("Action was cancelled.")
        if (this.mode != ActionMode.NONE) return failedPrecondition("Action has already started.")
        this.mode = mode
        return Status.ok()
    }

    private fun remapDefaultPorts(): Status {
        inputIds.clear()
        outputIds.clear()
        for (name in schema.inputs.keys) inputIds[name] =
            makeNodeId(id, name).orElse { status ->
                return status
            }
        for (name in schema.outputs.keys) outputIds[name] =
            makeNodeId(id, name).orElse { status ->
                return status
            }
        for (name in
            listOf(ACTION_STATUS_OUTPUT, ACTION_DISPATCH_STATUS_OUTPUT, ACTION_LOG_OUTPUT)) {
            outputIds[name] =
                makeNodeId(id, name).orElse { status ->
                    return status
                }
        }
        return Status.ok()
    }

    private fun attachStreamIfRequested(node: AsyncNode, bind: Boolean) {
        val s = stream
        if (!bind || s == null) return
        node.attachStream(s)
        boundNodes.add(node)
    }

    private fun collectAutofillFragments(): List<NodeFragment> {
        val fragments = mutableListOf<NodeFragment>()
        for ((name, port) in schema.inputs) {
            if (port.autofills.isEmpty()) continue
            val id = inputIds[name] ?: continue
            val start = fragments.size
            for (autofill in port.autofills) {
                fragments.add(
                    NodeFragment(
                        id,
                        autofill?.data ?: makeNullChunk(),
                        autofill?.seq,
                        autofill?.continued ?: false,
                    )
                )
            }
            if (fragments.size > start) fragments.last().continued = false
        }
        return fragments
    }

    private suspend fun runHandler() {
        var status: Status = Status.ok()
        val nested = parent != null
        var acquired = false
        try {
            session?.let { value ->
                status = value.acquireActionSlot(nested)
                acquired = status.ok()
            }
            if (status.ok()) {
                if (cancelRequested) status = cancelled("Action was cancelled.")
                else {
                    status = applyInputAutofills()
                    if (status.ok()) {
                        status =
                            try {
                                handler!!(this)
                            } catch (error: Exception) {
                                Status.fromException(error, "Action handler raised an exception.")
                            }
                    }
                }
            }
        } catch (error: CancellationException) {
            withContext(NonCancellable) { finishRun(cancelled("Action handler was cancelled.")) }
            throw error
        } finally {
            if (acquired) session?.releaseActionSlot(nested)
        }
        if (cancelRequested) status = cancelled("Action was cancelled.")
        finishRun(status)
    }

    private suspend fun finishRun(initialStatus: Status) {
        if (completionStatus != null || !finishing.compareAndSet(false, true)) return
        var finalStatus = initialStatus
        val outputStatus = finishOutputNodes(finalStatus)
        finalStatus = finalStatus.updated(outputStatus)
        val communicated = communicateStatus(finalStatus)
        finalStatus = finalStatus.updated(communicated)
        releaseNodesAfterRun()
        completionStatus = finalStatus
        done.resolve(finalStatus)
        untrackFromSession()
    }

    private suspend fun finishOutputNodes(status: Status): Status {
        // Close the log port with ordinary outputs. Handlers do not close this
        // reserved port themselves.
        val ids = (schema.outputs.keys + ACTION_LOG_OUTPUT).mapNotNull { item -> outputIds[item] }
        var first: Status = Status.ok()
        val s = stream
        if (!status.ok() && s != null && ids.isNotEmpty()) {
            val chunk = statusToChunk(status)
            if (chunk is Ok) {
                val fragments =
                    ids.map { element -> NodeFragment(element, chunk.value, 0u, false) }
                        .toMutableList()
                first = first.updated(s.send(WireMessage(nodeFragments = fragments)))
            }
        }
        for (id in ids) {
            val node =
                nodeMap.get(id).orElse { status ->
                    first = first.updated(status)
                    continue
                }
            val writable =
                node.isWritable().orElse { status ->
                    first = first.updated(status)
                    continue
                }
            if (!writable) continue
            val closed = if (status.ok()) node.close() else node.abortWithStatus(status)
            first = first.updated(closed)
        }
        return first
    }

    private suspend fun communicateStatus(status: Status): Status {
        val chunk =
            statusToChunk(status).orElse { status ->
                return status
            }
        val id =
            outputIds[ACTION_STATUS_OUTPUT]
                ?: return internal("Action status output is not mapped.")
        val node =
            nodeMap.get(id).orElse { status ->
                return status
            }
        val writable =
            node.isWritable().orElse { status ->
                return status
            }
        if (!writable) return failedPrecondition("Action status node was already finalized.")
        stream?.let { value ->
            node.attachStream(value)
            boundNodes.add(node)
        }
        node.putFragment(NodeFragment(id, chunk, 0u, false)).let { value ->
            if (value is Err) return value.status
        }
        return node.close()
    }

    private suspend fun releaseNodesAfterRun(): Status {
        var first = detachBoundNodes()
        if (settings.clearInputsAfterRun == true)
            for (id in inputIds.values) {
                (nodeMap.discard(id) as? Ok)?.value?.cancelReader()
            }
        if (settings.clearOutputsAfterRun == true) for (id in outputIds.values) nodeMap.discard(id)
        return first
    }

    private fun detachBoundNodes(): Status {
        stream?.let { s -> for (node in boundNodes) node.detachStream(s) }
        boundNodes.clear()
        return Status.ok()
    }

    private fun trackInSession(): Status {
        val s = session ?: return Status.ok()
        if (tracked) return Status.ok()
        val status = s.trackAction(this)
        if (status.ok()) tracked = true
        return status
    }

    private fun untrackFromSession() {
        if (!tracked) return
        tracked = false
        session?.untrackAction(this)
    }

    private fun sendRemoteCancel(): Status {
        val cancel =
            ActionMessage(
                id = randomId("action-"),
                name = CANCEL_ACTION_NAME,
                headers = ByteMap().apply { put(CANCEL_ACTION_HEADER, utf8Encode(id)) },
            )
        val message = WireMessage(actions = mutableListOf(cancel))
        return stream?.send(message)
            ?: session?.send(message)
            ?: failedPrecondition("Cancelling a called Action requires a WireStream or Session.")
    }

    private fun completeCall(status: Status, removeFromSession: Boolean) {
        if (completionStatus == null) {
            completionStatus = status
            detachBoundNodes()
            done.resolve(status)
        }
        if (removeFromSession) untrackFromSession()
    }

    companion object {
        fun create(
            schema: ActionSchema,
            options: ActionCreateOptions = ActionCreateOptions(),
        ): StatusOr<Action> {
            schema.validate().orElse { status ->
                return status
            }
            val id = options.id ?: randomId("action-")
            validateName(id).orElse { status ->
                return status
            }
            return Ok(Action(schema.copy(), options, id))
        }

        /** Derive the stable `action-id#port-name` id for one action port node. */
        fun makeNodeId(actionId: String, nodeName: String): StatusOr<String> {
            validateName(actionId).orElse { status ->
                return status
            }
            validateName(nodeName).orElse { status ->
                return status
            }
            val result = "$actionId#$nodeName"
            validateName(result).orElse { status ->
                return status
            }
            return Ok(result)
        }
    }
}
