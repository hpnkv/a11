import { Action, type ActionHandler, type ActionSessionContext } from './action.js';
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

/** A catalogue of named Action schemas and their optional local handlers. */
export class ActionRegistry {
  private readonly registrations = new Map<string, Registration>();

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

  unregister(actionName: string): Status {
    try {
      const validName = validateName(actionName);
      if (!isOk(validName)) return validName;
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
      return isOk(validateName(actionName)) && this.registrations.has(actionName);
    } catch {
      return false;
    }
  }

  getSchema(actionName: string): StatusOr<ActionSchema> {
    try {
      const validName = validateName(actionName);
      if (!isOk(validName)) return validName;
      const registration = this.registrations.get(actionName);
      if (registration === undefined) {
        return notFoundError(`Action '${actionName}' is not registered.`);
      }
      return cloneSchema(registration.schema);
    } catch (error) {
      return statusFromUnknown(error, 'Looking up Action schema raised an exception.');
    }
  }

  getHandler(actionName: string): StatusOr<ActionHandler> {
    try {
      const validName = validateName(actionName);
      if (!isOk(validName)) return validName;
      const registration = this.registrations.get(actionName);
      if (registration === undefined) {
        return notFoundError(`Action '${actionName}' is not registered.`);
      }
      return registration.handler ?? notFoundError(
        `Action '${actionName}' is registered without a handler.`,
      );
    } catch (error) {
      return statusFromUnknown(error, 'Looking up Action handler raised an exception.');
    }
  }

  makeAction(
    actionName: string,
    options: MakeActionOptions = {},
  ): StatusOr<Action> {
    try {
      const validName = validateName(actionName);
      if (!isOk(validName)) return validName;
      const registration = this.registrations.get(actionName);
      if (registration === undefined) {
        return notFoundError(`Action '${actionName}' is not registered.`);
      }
      const schema = cloneSchema(registration.schema);
      if (!isOk(schema)) return schema;
      return Action.create(schema, {
        id: options.id,
        handler: registration.handler,
        nodeMap: options.nodeMap,
        stream: options.stream,
        session: options.session,
        registry: this,
      });
    } catch (error) {
      return statusFromUnknown(error, 'Creating registered Action raised an exception.');
    }
  }

  makeActionMessage(
    actionName: string,
    actionId = '',
  ): StatusOr<ActionMessage> {
    const action = this.makeAction(actionName, {
      ...(actionId === '' ? {} : { id: actionId }),
    });
    return isOk(action) ? action.getActionMessage() : action;
  }

  listRegisteredActions(): string[] {
    try {
      return [...this.registrations.keys()];
    } catch {
      return [];
    }
  }

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
