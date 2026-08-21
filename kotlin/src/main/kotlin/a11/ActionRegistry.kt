package a11

import a11.net.WireStream

/** Per-instance collaborators supplied by [ActionRegistry.makeAction]. */
class MakeActionOptions(
    var id: String? = null,
    var nodeMap: NodeMap? = null,
    var stream: WireStream? = null,
    var session: ActionSessionContext? = null,
)

private class Registration(val schema: ActionSchema, val handler: ActionHandler?)

/**
 * Catalogue of action contracts and the handlers this process can execute.
 *
 * A session consults its registry when an [ActionMessage] arrives: the schema
 * validates and maps ports, and the optional handler performs local work.
 * Client-only entries may omit handlers while still exposing schemas for
 * constructing remote calls or model tool definitions. Ported from
 * `js/src/action_registry.ts`.
 */
class ActionRegistry : ActionRegistryLike {
    private val registrations = LinkedHashMap<String, Registration>()

    fun register(actionName: String, schema: ActionSchema, handler: ActionHandler? = null): Status {
        validateName(actionName).let { if (!it.isOk) return it }
        if (actionName == CANCEL_ACTION_NAME) return invalidArgument("The cancel Action name is reserved.")
        // Refused rather than shadowed. These are what a peer is asked with, and
        // an application that could replace one could make itself
        // undiscoverable -- which is what this mechanism exists to end.
        if (isBuiltinAction(actionName)) {
            return invalidArgument("'$actionName' is a builtin action and cannot be re-registered.")
        }
        schema.validate().let { if (!it.isOk) return it }
        if (schema.name != actionName) return invalidArgument("Registry Action name does not match schema name.")
        registrations[actionName] = Registration(schema.copy(), handler)
        return Status.ok()
    }

    fun unregister(actionName: String): Status {
        validateName(actionName).let { if (!it.isOk) return it }
        // InvalidArgument, not NotFound: "you cannot" and "it is not there" are
        // different answers, and this one is the first.
        if (isBuiltinAction(actionName)) {
            return invalidArgument("'$actionName' is a builtin action and cannot be unregistered.")
        }
        return if (registrations.remove(actionName) != null) Status.ok()
        else notFound("Action '$actionName' is not registered.")
    }

    // The builtins are not entries here; they are what every registry answers
    // for even when it holds nothing. See ActionBuiltins.kt.
    fun isRegistered(actionName: String): Boolean =
        isBuiltinAction(actionName) || registrations.containsKey(actionName)

    override fun getSchema(actionName: String): StatusOr<ActionSchema> {
        validateName(actionName).let { if (!it.isOk) return it }
        val reg = registrations[actionName]
            ?: return getBuiltinAction(actionName)?.let { Ok(it.schema.copy()) }
                ?: notFound("Action '$actionName' is not registered.")
        return Ok(reg.schema.copy())
    }

    override fun getHandler(actionName: String): StatusOr<ActionHandler> {
        validateName(actionName).let { if (!it.isOk) return it }
        val reg = registrations[actionName]
            ?: return getBuiltinAction(actionName)?.let { Ok(it.handler) }
                ?: notFound("Action '$actionName' is not registered.")
        return reg.handler?.let { Ok(it) } ?: notFound("Action '$actionName' is registered without a handler.")
    }

    /** Instantiate a configurable action bound to this registry. */
    fun makeAction(actionName: String, options: MakeActionOptions = MakeActionOptions()): StatusOr<Action> {
        validateName(actionName).let { if (!it.isOk) return it }
        val builtin = if (registrations.containsKey(actionName)) null else getBuiltinAction(actionName)
        val reg = registrations[actionName]
            ?: builtin?.let { Registration(it.schema, it.handler) }
            ?: return notFound("Action '$actionName' is not registered.")
        return Action.create(reg.schema.copy(), ActionCreateOptions(
            id = options.id,
            handler = reg.handler,
            nodeMap = options.nodeMap,
            stream = options.stream,
            session = options.session,
            registry = this,
        ))
    }

    fun listRegisteredActions(): List<String> {
        val builtins = builtinActionNames()
        // `register` refuses a builtin's name, so nothing can collide -- but a
        // duplicate in a listing survives a long time before anybody notices.
        return builtins + registrations.keys.filter { it !in builtins }
    }
}
