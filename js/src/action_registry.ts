import { Action, type ActionHandler, type ActionSessionContext } from './action.js';
import {
  getBuiltinAction,
  isBuiltinAction,
  builtinActionNames,
} from './action_builtins.js';
import {
  CANCEL_ACTION_NAME,
  ActionHeaderSchema,
  ActionPortSchema,
  ActionSchema,
} from './action_schema.js';
import type { NodeMap } from './async_node.js';
import { cloneFragment } from './chunk_store.js';
import { validateName, type ActionMessage } from './data.js';
import {
  invalidArgumentError,
  isOk,
  notFoundError,
  okStatus,
  statusFromUnknown,
  type Status,
  type StatusOr,
} from './status.js';
import type { WireStream } from './wire_stream.js';

interface Registration {
  schema: ActionSchema;
  handler: ActionHandler | null;
}

/** Per-instance collaborators supplied by {@link ActionRegistry.makeAction}. */
export interface MakeActionOptions {
  id?: string;
  nodeMap?: NodeMap;
  stream?: WireStream | null;
  session?: ActionSessionContext | null;
}

function cloneSchema(
  schema: ActionSchema,
  clearAutofills = false,
): StatusOr<ActionSchema> {
  try {
    const inputs = new Map<string, ActionPortSchema>();
    const outputs = new Map<string, ActionPortSchema>();
    for (const [name, port] of schema.inputs) {
      inputs.set(name, new ActionPortSchema({
        name: port.name,
        type: port.type,
        description: port.description,
        required: port.required,
        unary: port.unary,
        jsonSchema: port.jsonSchema,
        autofills: clearAutofills
          ? []
          : port.autofills.map((fragment) =>
              fragment === null ? null : cloneFragment(fragment),
            ),
      }));
    }
    for (const [name, port] of schema.outputs) {
      outputs.set(name, new ActionPortSchema({
        name: port.name,
        type: port.type,
        description: port.description,
        required: port.required,
        unary: port.unary,
        jsonSchema: port.jsonSchema,
        autofills: clearAutofills
          ? []
          : port.autofills.map((fragment) =>
              fragment === null ? null : cloneFragment(fragment),
            ),
      }));
    }
    const headers = new Map<string, ActionHeaderSchema>();
    for (const [name, header] of schema.headers) {
      headers.set(name, new ActionHeaderSchema({
        name: header.name,
        description: header.description,
        defaultValue:
          header.defaultValue === null
            ? null
            : new Uint8Array(header.defaultValue),
      }));
    }
    const copy = new ActionSchema({
      name: schema.name,
      description: schema.description,
      inputs,
      outputs,
      headers,
      outputToJsonField: new Map(schema.outputToJsonField),
    });
    const validation = copy.validate();
    return isOk(validation) ? copy : validation;
  } catch (error) {
    return statusFromUnknown(error, 'Copying ActionSchema raised an exception.');
  }
}

/**
 * Catalogue of action contracts and the handlers this process can execute.
 *
 * A session consults its registry when an {@link ActionMessage} arrives: the
 * schema validates and maps ports, and the optional handler performs local
 * work. Client-only entries may omit handlers while still exposing schemas for
 * constructing remote calls or model tool definitions. Registrations are
 * copied so later caller mutation cannot change a live service contract.
 */
export class ActionRegistry {
  private readonly registrations = new Map<string, Registration>();

  /** Register or replace one named schema/handler pair. */
  register(
    actionName: string,
    schema: ActionSchema,
    handler: ActionHandler | null = null,
  ): Status {
    try {
      const validName = validateName(actionName);
      if (!isOk(validName)) return validName;
      if (actionName === CANCEL_ACTION_NAME) {
        return invalidArgumentError('The cancel Action name is reserved.');
      }
      if (isBuiltinAction(actionName)) {
        // Refused rather than shadowed. These are what a peer is asked with,
        // and an application that could replace one could make itself
        // undiscoverable -- which is what this mechanism exists to end.
        return invalidArgumentError(
          `'${actionName}' is a builtin action and cannot be re-registered.`,
        );
      }
      if (!(schema instanceof ActionSchema)) {
        return invalidArgumentError('schema must be an ActionSchema.');
      }
      if (handler !== null && typeof handler !== 'function') {
        return invalidArgumentError('handler must be callable or null.');
      }
      const validation = schema.validate();
      if (!isOk(validation)) return validation;
      if (schema.name !== actionName) {
        return invalidArgumentError(
          'Registry Action name does not match schema name.',
        );
      }
      const stored = cloneSchema(schema);
      if (!isOk(stored)) return stored;
      this.registrations.set(actionName, { schema: stored, handler });
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Registering Action raised an exception.');
    }
  }

  /** Remove an action so future calls are no longer dispatchable. */
  unregister(actionName: string): Status {
    try {
      const validName = validateName(actionName);
      if (!isOk(validName)) return validName;
      if (isBuiltinAction(actionName)) {
        // InvalidArgument, not NotFound: "you cannot" and "it is not there" are
        // different answers, and this one is the first.
        return invalidArgumentError(
          `'${actionName}' is a builtin action and cannot be unregistered.`,
        );
      }
      if (!this.registrations.delete(actionName)) {
        return notFoundError(`Action '${actionName}' is not registered.`);
      }
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Unregistering Action raised an exception.');
    }
  }

  isRegistered(actionName: string): boolean {
    try {
      if (!isOk(validateName(actionName))) return false;
      // The builtins are not entries here; they are what every registry answers
      // for even when it holds nothing. See ./action_builtins.js.
      return isBuiltinAction(actionName) || this.registrations.has(actionName);
    } catch {
      return false;
    }
  }

  /** Return an isolated copy of a registered callable contract. */
  getSchema(actionName: string): StatusOr<ActionSchema> {
    try {
      const validName = validateName(actionName);
      if (!isOk(validName)) return validName;
      const registration = this.registrations.get(actionName);
      if (registration === undefined) {
        const builtin = getBuiltinAction(actionName);
        if (builtin !== undefined) return cloneSchema(builtin.schema);
        return notFoundError(`Action '${actionName}' is not registered.`);
      }
      return cloneSchema(registration.schema);
    } catch (error) {
      return statusFromUnknown(error, 'Looking up Action schema raised an exception.');
    }
  }

  /** Return the local implementation, or NotFound for remote-only entries. */
  getHandler(actionName: string): StatusOr<ActionHandler> {
    try {
      const validName = validateName(actionName);
      if (!isOk(validName)) return validName;
      const registration = this.registrations.get(actionName);
      if (registration === undefined) {
        const builtin = getBuiltinAction(actionName);
        if (builtin !== undefined) return builtin.handler;
        return notFoundError(`Action '${actionName}' is not registered.`);
      }
      return registration.handler ?? notFoundError(
        `Action '${actionName}' is registered without a handler.`,
      );
    } catch (error) {
      return statusFromUnknown(error, 'Looking up Action handler raised an exception.');
    }
  }

  /** Instantiate a configurable action bound to this registry. */
  makeAction(
    actionName: string,
    options: MakeActionOptions = {},
  ): StatusOr<Action> {
    try {
      const validName = validateName(actionName);
      if (!isOk(validName)) return validName;
      const registration = this.registrations.get(actionName);
      const builtin =
        registration === undefined ? getBuiltinAction(actionName) : undefined;
      if (registration === undefined && builtin === undefined) {
        return notFoundError(`Action '${actionName}' is not registered.`);
      }
      const schema = cloneSchema(
        registration !== undefined ? registration.schema : builtin!.schema,
      );
      if (!isOk(schema)) return schema;
      return Action.create(schema, {
        id: options.id,
        handler:
          registration !== undefined ? registration.handler : builtin!.handler,
        nodeMap: options.nodeMap,
        stream: options.stream,
        session: options.session,
        registry: this,
      });
    } catch (error) {
      return statusFromUnknown(error, 'Creating registered Action raised an exception.');
    }
  }

  /** Build the initial wire description for a registered action call. */
  makeActionMessage(
    actionName: string,
    actionId = '',
  ): StatusOr<ActionMessage> {
    const action = this.makeAction(actionName, {
      ...(actionId === '' ? {} : { id: actionId }),
    });
    return isOk(action) ? action.getActionMessage() : action;
  }

  /** Snapshot registered names in insertion order. */
  listRegisteredActions(): string[] {
    try {
      const builtins = builtinActionNames();
      const own = [...this.registrations.keys()].filter(
        (name) => !builtins.includes(name),
      );
      // `register` refuses a builtin's name, so nothing can collide -- but a
      // duplicate in a listing survives a long time before anybody notices.
      return [...builtins, ...own];
    } catch {
      return [];
    }
  }

  /**
   * Clone registrations, normally removing all input and output autofills.
   * Use the cleared copy before sharing a registry across an agent or trust
   * boundary so context-specific defaults do not cross implicitly.
   */
  copy(clearAutofills = true): StatusOr<ActionRegistry> {
    if (typeof clearAutofills !== 'boolean') {
      return invalidArgumentError('clearAutofills must be boolean.');
    }
    const result = new ActionRegistry();
    try {
      for (const [name, registration] of this.registrations) {
        const schema = cloneSchema(registration.schema, clearAutofills);
        if (!isOk(schema)) return schema;
        result.registrations.set(name, {
          schema,
          handler: registration.handler,
        });
      }
      return result;
    } catch (error) {
      return statusFromUnknown(error, 'Copying ActionRegistry raised an exception.');
    }
  }
}
