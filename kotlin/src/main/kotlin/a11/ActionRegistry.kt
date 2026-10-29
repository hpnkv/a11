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

/** Per-instance collaborators supplied by [ActionRegistry.makeAction]. */
data class MakeActionOptions(
    val id: String? = null,
    val nodeMap: NodeMap? = null,
    val stream: WireStream? = null,
    val session: Session? = null,
)

private class Registration(val schema: ActionSchema, val handler: ActionHandler?)

/**
 * Catalogue of action contracts and the handlers this process can execute.
 *
 * A session consults its registry when an [ActionMessage] arrives: the schema validates and maps
 * ports, and the optional handler performs local work. Client-only entries may omit handlers while
 * still exposing schemas for constructing remote calls or model tool definitions. Ported from
 * `js/src/action_registry.ts`.
 */
class ActionRegistry {
    private val registrations = LinkedHashMap<String, Registration>()

    @Synchronized
    fun register(actionName: String, schema: ActionSchema, handler: ActionHandler? = null): Status {
        validateName(actionName).orElse { status ->
            return status
        }
        if (actionName == CANCEL_ACTION_NAME)
            return invalidArgument("The cancel Action name is reserved.")
        // Refused rather than shadowed. These are what a peer is asked with,
        // and
        // an application that could replace one could make itself
        // undiscoverable -- which is what this mechanism exists to end.
        if (isBuiltinAction(actionName)) {
            return invalidArgument("'$actionName' is a builtin action and cannot be re-registered.")
        }
        schema.validate().orElse { status ->
            return status
        }
        if (schema.name != actionName)
            return invalidArgument("Registry Action name does not match schema name.")
        registrations[actionName] = Registration(schema.copy(), handler)
        return Status.ok()
    }

    @Synchronized
    fun unregister(actionName: String): Status {
        validateName(actionName).orElse { status ->
            return status
        }
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
    @Synchronized
    fun isRegistered(actionName: String): Boolean =
        isBuiltinAction(actionName) || registrations.containsKey(actionName)

    @Synchronized
    fun getSchema(actionName: String): StatusOr<ActionSchema> {
        validateName(actionName).orElse { status ->
            return status
        }
        val reg =
            registrations[actionName]
                ?: return getBuiltinAction(actionName)?.let { value -> Ok(value.schema.copy()) }
                    ?: notFound("Action '$actionName' is not registered.")
        return Ok(reg.schema.copy())
    }

    @Synchronized
    fun getHandler(actionName: String): StatusOr<ActionHandler> {
        validateName(actionName).orElse { status ->
            return status
        }
        val reg =
            registrations[actionName]
                ?: return getBuiltinAction(actionName)?.let { value -> Ok(value.handler) }
                    ?: notFound("Action '$actionName' is not registered.")
        return reg.handler?.let { value -> Ok(value) }
            ?: notFound("Action '$actionName' is registered without a handler.")
    }

    /** Instantiate a configurable action bound to this registry. */
    @Synchronized
    fun makeAction(
        actionName: String,
        options: MakeActionOptions = MakeActionOptions(),
    ): StatusOr<Action> {
        validateName(actionName).orElse { status ->
            return status
        }
        val builtin =
            if (registrations.containsKey(actionName)) null else getBuiltinAction(actionName)
        val reg =
            registrations[actionName]
                ?: builtin?.let { value -> Registration(value.schema, value.handler) }
                ?: return notFound("Action '$actionName' is not registered.")
        return Action.create(
            reg.schema.copy(),
            ActionCreateOptions(
                id = options.id,
                handler = reg.handler,
                nodeMap = options.nodeMap,
                stream = options.stream,
                session = options.session,
                registry = this,
            ),
        )
    }

    @Synchronized
    fun listRegisteredActions(): List<String> {
        val builtins = builtinActionNames()
        // `register` refuses a builtin's name, so nothing can collide -- but a
        // duplicate in a listing survives a long time before anybody notices.
        return builtins + registrations.keys.filter { item -> item !in builtins }
    }
}
