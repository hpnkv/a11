package a11

import a11.net.WireStream
import kotlinx.coroutines.launch

/** Asynchronous application work invoked by [Action.run]; null result means OK. */
typealias ActionHandler = suspend (Action) -> Status?

/** Synchronous hook run when cancellation is first requested. */
typealias OnActionCancelled = (Action) -> Status?

/** Minimal registry contract used to resolve a nested action by name. */
interface ActionRegistryLike {
    fun getSchema(actionName: String): StatusOr<ActionSchema>
    fun getHandler(actionName: String): StatusOr<ActionHandler>
}

/** Session operations an Action needs for dispatch and lifetime tracking. */
interface ActionSessionContext {
    fun getNodeMap(): NodeMap
    fun getActionRegistry(): ActionRegistryLike?
    fun send(message: WireMessage, streamId: String = ""): Status
    fun trackAction(action: Action): Status
    fun untrackAction(action: Action)
    suspend fun acquireActionSlot(nested: Boolean): Status = Status.ok()
    fun releaseActionSlot(nested: Boolean) {}
}

/** Collaborators and instance policy supplied when creating an [Action]. */
class ActionCreateOptions(
    var id: String? = null,
    var handler: ActionHandler? = null,
    var nodeMap: NodeMap? = null,
    var stream: WireStream? = null,
    var session: ActionSessionContext? = null,
    var registry: ActionRegistryLike? = null,
    var settings: ActionSettings = ActionSettings(),
)

private enum class ActionMode { NONE, RUN, CALL, CANCELLED }

/** One schema-described unit of local work or remote agent work. */
class Action private constructor(
    private var schema: ActionSchema,
    options: ActionCreateOptions,
    private var id: String,
) {
    private var handler: ActionHandler? = options.handler
    private var session: ActionSessionContext? = options.session
    private var nodeMap: NodeMap = options.nodeMap ?: session?.getNodeMap() ?: NodeMap()
    private var stream: WireStream? = options.stream
    private var registry: ActionRegistryLike? = options.registry ?: session?.getActionRegistry()
    private var settings: ActionSettings = options.settings.copy()

    private val headers: ByteMap = ByteMap()
    private val inputIds = LinkedHashMap<String, String>()
    private val outputIds = LinkedHashMap<String, String>()
    private val boundNodes = mutableSetOf<AsyncNode>()
    private var mode = ActionMode.NONE
    private var completionStatus: Status? = null
    private var dispatchStatus: Status? = null
    @Volatile private var cancelRequested = false
    private var finishing = false
    private var inputAutofillsApplied = false
    private var tracked = false
    private val done = Deferred<Status>()
    private val dispatched = Deferred<Status>()
    private val cancelCallbacks = mutableListOf<OnActionCancelled>()
    private var parent: Action? = null

    init {
        for ((name, header) in schema.headers) {
            header.defaultValue?.let { headers[name.lowercase()] = it.copyOf() }
        }
        remapDefaultPorts()
    }

    val isCancelled: Boolean get() = cancelRequested || completionStatus?.code == StatusCode.CANCELLED
    fun getId(): String = id
    fun getSchema(): ActionSchema = schema
    fun getHandler(): ActionHandler? = handler
    fun getNodeMap(): NodeMap = nodeMap
    fun getStream(): WireStream? = stream
    fun getRegistry(): ActionRegistryLike? = registry
    fun getSession(): ActionSessionContext? = session
    fun getStatus(): Status = completionStatus ?: Status.ok()
    fun getDispatchStatus(): Status? = dispatchStatus
    fun isDone(): Boolean = completionStatus != null

    fun setId(id: String): Status {
        validateName(id).let { if (!it.isOk) return it }
        if (mode != ActionMode.NONE) return failedPrecondition("Cannot change Action id after it has started.")
        this.id = id
        return remapDefaultPorts()
    }

    fun bindHandler(handler: ActionHandler): Status {
        if (mode != ActionMode.NONE) return failedPrecondition("Cannot change Action handler after it has started.")
        this.handler = handler
        return Status.ok()
    }

    fun bindNodeMap(nodeMap: NodeMap): Status { this.nodeMap = nodeMap; return Status.ok() }
    fun bindRegistry(registry: ActionRegistryLike?): Status { this.registry = registry; return Status.ok() }
    fun bindSession(session: ActionSessionContext?): Status {
        if (!tracked || this.session === session) { this.session = session; return Status.ok() }
        if (session != null) session.trackAction(this).let { if (!it.isOk) return it }
        this.session?.untrackAction(this)
        this.session = session
        tracked = session != null
        return Status.ok()
    }

    fun bindStream(stream: WireStream?): Status {
        if (stream === this.stream) return Status.ok()
        val previous = this.stream
        for (node in boundNodes) {
            previous?.let { node.detachStream(it) }
            stream?.let { node.attachStream(it) }
        }
        this.stream = stream
        return Status.ok()
    }

    // --- port access ---------------------------------------------------------

    suspend fun getInput(name: String, bindStream: Boolean? = null): StatusOr<AsyncNode> {
        val id = inputIds[name] ?: return notFound("Action input '$name' is not mapped.")
        val node = nodeMap.get(id).orElse { return it }
        val bind = bindStream ?: settings.bindStreamsOnInputsByDefault ?: (mode != ActionMode.RUN)
        attachStreamIfRequested(node, bind)
        return Ok(node)
    }

    suspend fun getOutput(name: String, bindStream: Boolean? = null): StatusOr<AsyncNode> {
        val id = outputIds[name] ?: return notFound("Action output '$name' is not mapped.")
        val node = nodeMap.get(id).orElse { return it }
        val bind = bindStream ?: settings.bindStreamsOnOutputsByDefault ?: (mode == ActionMode.RUN)
        attachStreamIfRequested(node, bind && name != ACTION_STATUS_OUTPUT && name != ACTION_DISPATCH_STATUS_OUTPUT)
        return Ok(node)
    }

    fun containsPort(name: String): Boolean = inputIds.containsKey(name) || outputIds.containsKey(name)

    /** Snapshot the call id, name, port mappings, and headers for dispatch. */
    fun getActionMessage(): ActionMessage = ActionMessage(
        id = id,
        name = schema.name,
        inputs = schema.inputs.keys.map { Port(it, inputIds[it] ?: "") }.toMutableList(),
        outputs = schema.outputs.keys.map { Port(it, outputIds[it] ?: "") }.toMutableList(),
        headers = copyByteMap(headers),
    )

    /** Adopt validated caller-supplied port node ids before local execution. */
    fun mapPortsFromMessage(message: ActionMessage): Status {
        if (mode != ActionMode.NONE) return failedPrecondition("Cannot remap Action ports after it has started.")
        message.validate().let { if (!it.isOk) return it }
        for (port in message.inputs) {
            if (!schema.inputs.containsKey(port.name)) return failedPrecondition("Unknown Action input port '${port.name}'.")
            inputIds[port.name] = port.id
        }
        for (port in message.outputs) {
            if (!schema.outputs.containsKey(port.name)) return failedPrecondition("Unknown Action output port '${port.name}'.")
            outputIds[port.name] = port.id
        }
        return Status.ok()
    }

    // --- headers -------------------------------------------------------------

    fun getHeaders(): ByteMap = copyByteMap(headers)
    fun getHeader(name: String): StatusOr<ByteArray?> {
        validateName(name).let { if (!it.isOk) return it }
        return Ok(headers[name.lowercase()]?.copyOf())
    }

    fun setHeader(name: String, value: ByteArray): Status {
        validateName(name).let { if (!it.isOk) return it }
        headers[name.lowercase()] = value.copyOf()
        return Status.ok()
    }

    fun setHeader(name: String, value: String): Status = setHeader(name, utf8Encode(value))

    fun forwardHeadersWithPrefix(target: Action, prefix: String = ACTION_HEADER_PREFIX): Status {
        val folded = prefix.lowercase()
        for ((name, value) in headers) if (name.startsWith(folded)) target.setHeader(name, value).let { if (!it.isOk) return it }
        return Status.ok()
    }

    // --- nested --------------------------------------------------------------

    fun makeNested(name: String, propagateIo: Boolean = true, forwardHeaders: Boolean = true): StatusOr<Action> {
        val reg = registry ?: return failedPrecondition("Cannot resolve a nested Action without a registry.")
        val schema = reg.getSchema(name).orElse { return it }
        val handler = when (val h = reg.getHandler(name)) { is Ok -> h.value; is Status -> null }
        val child = create(schema, ActionCreateOptions(
            handler = handler,
            nodeMap = if (propagateIo) nodeMap else null,
            stream = if (propagateIo) stream else null,
            session = if (propagateIo) session else null,
            registry = registry,
        )).orElse { return it }
        child.parent = this
        if (forwardHeaders) forwardHeadersWithPrefix(child).let { if (!it.isOk) return it }
        return Ok(child)
    }

    // --- lifecycle -----------------------------------------------------------

    /** Start the bound handler locally and return immediately. */
    fun run(): StatusOr<Action> {
        if (handler == null) return failedPrecondition("Action handler has not been set.")
        begin(ActionMode.RUN).let { if (!it.isOk) return it }
        trackInSession().let { if (!it.isOk) { mode = ActionMode.NONE; return it } }
        A11Runtime.scope.launch { runHandler() }
        return Ok(this)
    }

    /** Queue this action for remote dispatch; use [waitForDispatch] for acceptance. */
    suspend fun call(wireHeaders: ByteMap = ByteMap()): StatusOr<Action> {
        begin(ActionMode.CALL).let { if (!it.isOk) return it }
        trackInSession().let { if (!it.isOk) { mode = ActionMode.NONE; return it } }
        val autofills = collectAutofillFragments()
        val message = WireMessage(
            nodeFragments = autofills.toMutableList(),
            actions = mutableListOf(getActionMessage()),
            headers = copyByteMap(wireHeaders),
        )
        val sent = stream?.send(message) ?: session?.send(message)
            ?: failedPrecondition("Calling an Action requires an attached WireStream or Session.")
        if (!sent.isOk) { untrackFromSession(); mode = ActionMode.NONE; return sent }
        return Ok(this)
    }

    suspend fun waitForDispatch(timeoutMs: Long? = null): Status {
        if (mode != ActionMode.CALL) return failedPrecondition("Only a called Action has a dispatch status.")
        return when (val r = waitFor(timeoutMs, "Action dispatch timed out.") { dispatched.await() }) {
            is Ok -> r.value; is Status -> r
        }
    }

    suspend fun wait(timeoutMs: Long? = null): StatusOr<Action> {
        if (mode == ActionMode.NONE) return failedPrecondition("Action has not been run or called.")
        return when (val r = waitFor(timeoutMs, "Action wait timed out.") { done.await() }) {
            is Ok -> if (r.value.isOk) Ok(this) else r.value
            is Status -> r
        }
    }

    fun cancel(): Status {
        if (completionStatus != null || finishing || cancelRequested) return Status.ok()
        cancelRequested = true
        for (cb in cancelCallbacks) cb(this)
        val cancelledStatus = cancelled("Action was cancelled.")
        when (mode) {
            ActionMode.CALL -> {
                sendRemoteCancel()
                completeCall(cancelledStatus, removeFromSession = false)
            }
            ActionMode.RUN -> A11Runtime.scope.launch { finishRun(cancelledStatus) }
            ActionMode.NONE -> {
                mode = ActionMode.CANCELLED
                completionStatus = cancelledStatus
                done.resolve(cancelledStatus)
            }
            ActionMode.CANCELLED -> {}
        }
        return Status.ok()
    }

    fun setOnCancelled(callback: OnActionCancelled): Status { cancelCallbacks.add(callback); return Status.ok() }

    /** Session protocol hook for the reserved dispatch-status node. */
    fun setDispatchStatus(status: Status): Status {
        if (mode != ActionMode.CALL || dispatchStatus != null) return Status.ok()
        dispatchStatus = status
        dispatched.resolve(status)
        return Status.ok()
    }

    /** Session protocol hook for the reserved completion-status node. */
    fun setCompletionStatus(status: Status): Status {
        if (mode != ActionMode.CALL) return failedPrecondition("Action is not a call.")
        if (dispatchStatus == null) { dispatchStatus = Status.ok(); dispatched.resolve(Status.ok()) }
        if (completionStatus == null) completeCall(status, removeFromSession = true)
        else if (cancelRequested) untrackFromSession()
        return Status.ok()
    }

    suspend fun applyInputAutofills(): Status {
        if (inputAutofillsApplied) return Status.ok()
        for ((name, port) in schema.inputs) {
            if (port.autofills.isEmpty()) continue
            val id = inputIds[name] ?: continue
            val node = nodeMap.get(id).orElse { return it }
            for (autofill in port.autofills) {
                val stored = if (autofill == null) node.putNullFinal()
                else node.putFragment(NodeFragment(id, autofill.data, autofill.seq, autofill.continued))
                if (stored is Status && !stored.isOk) return stored
            }
            node.drainAndClose().let { if (!it.isOk) return it }
        }
        inputAutofillsApplied = true
        return Status.ok()
    }

    fun clearInputsAfterRun(clear: Boolean = true): Status { settings.clearInputsAfterRun = clear; return Status.ok() }
    fun clearOutputsAfterRun(clear: Boolean = true): Status { settings.clearOutputsAfterRun = clear; return Status.ok() }

    // --- internals -----------------------------------------------------------

    private fun begin(mode: ActionMode): Status {
        if (cancelRequested) return cancelled("Action was cancelled.")
        if (this.mode != ActionMode.NONE) return failedPrecondition("Action has already started.")
        this.mode = mode
        return Status.ok()
    }

    private fun remapDefaultPorts(): Status {
        inputIds.clear(); outputIds.clear()
        for (name in schema.inputs.keys) inputIds[name] = makeNodeId(id, name).orElse { return it }
        for (name in schema.outputs.keys) outputIds[name] = makeNodeId(id, name).orElse { return it }
        for (name in listOf(ACTION_STATUS_OUTPUT, ACTION_DISPATCH_STATUS_OUTPUT)) outputIds[name] = makeNodeId(id, name).orElse { return it }
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
                fragments.add(NodeFragment(id, autofill?.data ?: makeNullChunk(), autofill?.seq, autofill?.continued ?: false))
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
            session?.let {
                status = it.acquireActionSlot(nested)
                acquired = status.isOk
            }
            if (status.isOk) {
                if (cancelRequested) status = cancelled("Action was cancelled.")
                else {
                    status = applyInputAutofills()
                    if (status.isOk) {
                        status = try {
                            handler!!(this) ?: Status.ok()
                        } catch (error: Throwable) {
                            Status.fromException(error, "Action handler raised an exception.")
                        }
                    }
                }
            }
        } finally {
            if (acquired) session?.releaseActionSlot(nested)
        }
        if (cancelRequested) status = cancelled("Action was cancelled.")
        finishRun(status)
    }

    private suspend fun finishRun(initialStatus: Status) {
        if (finishing || completionStatus != null) return
        finishing = true
        var finalStatus = initialStatus
        val outputStatus = finishOutputNodes(finalStatus)
        if (finalStatus.isOk && !outputStatus.isOk) finalStatus = outputStatus
        val communicated = communicateStatus(finalStatus)
        if (finalStatus.isOk && !communicated.isOk) finalStatus = communicated
        releaseNodesAfterRun()
        completionStatus = finalStatus
        done.resolve(finalStatus)
        untrackFromSession()
    }

    private suspend fun finishOutputNodes(status: Status): Status {
        val ids = schema.outputs.keys.mapNotNull { outputIds[it] }
        var first: Status = Status.ok()
        val s = stream
        if (!status.isOk && s != null && ids.isNotEmpty()) {
            val chunk = statusToChunk(status)
            if (chunk is Ok) {
                val fragments = ids.map { NodeFragment(it, chunk.value, 0, false) }.toMutableList()
                s.send(WireMessage(nodeFragments = fragments))
            }
        }
        for (id in ids) {
            val node = nodeMap.get(id).orElse { first = firstError(first, it); continue }
            val writable = node.isWritable().orElse { first = firstError(first, it); continue }
            if (!writable) continue
            val closed = if (status.isOk) node.drainAndClose() else node.abortWithStatus(status)
            first = firstError(first, closed)
        }
        return first
    }

    private suspend fun communicateStatus(status: Status): Status {
        val chunk = statusToChunk(status).orElse { return it }
        val id = outputIds[ACTION_STATUS_OUTPUT] ?: return internal("Action status output is not mapped.")
        val node = nodeMap.get(id).orElse { return it }
        val writable = node.isWritable().orElse { return it }
        if (!writable) return failedPrecondition("Action status node was already finalized.")
        stream?.let { node.attachStream(it); boundNodes.add(node) }
        node.putFragment(NodeFragment(id, chunk, 0, false)).let { if (it is Status && !it.isOk) return it }
        return node.drainAndClose()
    }

    private suspend fun releaseNodesAfterRun(): Status {
        var first = detachBoundNodes()
        if (settings.clearInputsAfterRun == true) for (id in inputIds.values) {
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
        if (status.isOk) tracked = true
        return status
    }

    private fun untrackFromSession() {
        if (!tracked) return
        tracked = false
        session?.untrackAction(this)
    }

    private fun sendRemoteCancel(): Status {
        val cancel = ActionMessage(
            id = randomId("action-"),
            name = CANCEL_ACTION_NAME,
            headers = ByteMap().apply { put(CANCEL_ACTION_HEADER, utf8Encode(id)) },
        )
        val message = WireMessage(actions = mutableListOf(cancel))
        return stream?.send(message) ?: session?.send(message)
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
        fun create(schema: ActionSchema, options: ActionCreateOptions = ActionCreateOptions()): StatusOr<Action> {
            schema.validate().let { if (!it.isOk) return it }
            val id = options.id ?: randomId("action-")
            validateName(id).let { if (!it.isOk) return it }
            return Ok(Action(schema, options, id))
        }

        /** Derive the stable `action-id#port-name` id for one action port node. */
        fun makeNodeId(actionId: String, nodeName: String): StatusOr<String> {
            validateName(actionId).let { if (!it.isOk) return it }
            validateName(nodeName).let { if (!it.isOk) return it }
            val result = "$actionId#$nodeName"
            validateName(result).let { if (!it.isOk) return it }
            return Ok(result)
        }
    }
}

internal fun firstError(first: Status, next: Status): Status =
    if (first.isOk && !next.isOk) next else first
