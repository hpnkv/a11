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
        schema.validate().let { if (!it.isOk) return it }
        if (schema.name != actionName) return invalidArgument("Registry Action name does not match schema name.")
        registrations[actionName] = Registration(schema.copy(), handler)
        return Status.ok()
    }

    fun unregister(actionName: String): Status {
        validateName(actionName).let { if (!it.isOk) return it }
        return if (registrations.remove(actionName) != null) Status.ok()
        else notFound("Action '$actionName' is not registered.")
    }

    fun isRegistered(actionName: String): Boolean = registrations.containsKey(actionName)

    override fun getSchema(actionName: String): StatusOr<ActionSchema> {
        validateName(actionName).let { if (!it.isOk) return it }
        val reg = registrations[actionName] ?: return notFound("Action '$actionName' is not registered.")
        return Ok(reg.schema.copy())
    }

    override fun getHandler(actionName: String): StatusOr<ActionHandler> {
        validateName(actionName).let { if (!it.isOk) return it }
        val reg = registrations[actionName] ?: return notFound("Action '$actionName' is not registered.")
        return reg.handler?.let { Ok(it) } ?: notFound("Action '$actionName' is registered without a handler.")
    }

    /** Instantiate a configurable action bound to this registry. */
    fun makeAction(actionName: String, options: MakeActionOptions = MakeActionOptions()): StatusOr<Action> {
        validateName(actionName).let { if (!it.isOk) return it }
        val reg = registrations[actionName] ?: return notFound("Action '$actionName' is not registered.")
        return Action.create(reg.schema.copy(), ActionCreateOptions(
            id = options.id,
            handler = reg.handler,
            nodeMap = options.nodeMap,
            stream = options.stream,
            session = options.session,
            registry = this,
        ))
    }

    fun listRegisteredActions(): List<String> = registrations.keys.toList()
}
