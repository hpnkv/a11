import { type ByteSource, toBytes } from './bytes.js';
import { Chunk, ChunkMetadata, NodeFragment, validateName } from './data.js';
import { decodeStatus, packStatus, type DecodedStatus } from './status_codec.js';
import {
  failedPreconditionError,
  invalidArgumentError,
  isOk,
  notFoundError,
  okStatus,
  type Status,
  type StatusOr,
} from './status.js';

export const ACTION_STATUS_MIMETYPE = 'application/x-a11-status';
export const ACTION_STATUS_OUTPUT = '__status__';
export const ACTION_DISPATCH_STATUS_OUTPUT = '__dispatch_status__';
export const CANCEL_ACTION_NAME = '__cancel__';
export const CANCEL_ACTION_HEADER = '__action';
export const ACTION_HEADER_PREFIX = 'x-a11-';
export const WHOLE_JSON_OUTPUT = '$';

export interface ActionPortSchemaOptions {
  name: string;
  type: string;
  description?: string;
  required?: boolean;
  unary?: boolean;
  autofills?: readonly (NodeFragment | null)[];
}

export class ActionPortSchema {
  name: string;
  type: string;
  description: string;
  required: boolean;
  unary: boolean;
  autofills: Array<NodeFragment | null>;

  constructor(options: ActionPortSchemaOptions) {
    this.name = options.name;
    this.type = options.type;
    this.description = options.description ?? '';
    this.required = options.required ?? false;
    this.unary = options.unary ?? false;
    this.autofills = [...(options.autofills ?? [])];
  }

  static create(
    options: ActionPortSchemaOptions,
  ): StatusOr<ActionPortSchema> {
    try {
      const result = new ActionPortSchema(options);
      const validation = result.validate();
      return isOk(validation) ? result : validation;
    } catch (error) {
      return invalidArgumentError('Invalid ActionPortSchema options.', [], error);
    }
  }

  validate(): Status {
    try {
      const name = validateName(this.name);
      if (!isOk(name)) return name;
      if (typeof this.type !== 'string' || this.type.length === 0) {
        return invalidArgumentError('Action port type must not be empty.');
      }
      if (typeof this.description !== 'string') {
        return invalidArgumentError('Action port description must be a string.');
      }
      if (typeof this.required !== 'boolean' || typeof this.unary !== 'boolean') {
        return invalidArgumentError('Action port required and unary flags must be boolean.');
      }
      if (!Array.isArray(this.autofills)) {
        return invalidArgumentError('Action port autofills must be an array.');
      }
      for (const fragment of this.autofills) {
        if (fragment === null) continue;
        if (!(fragment instanceof NodeFragment)) {
          return invalidArgumentError('Each Action port autofill must be a NodeFragment or null.');
        }
        const validation = fragment.validate();
        if (!isOk(validation)) return validation;
      }
      return okStatus();
    } catch (error) {
      return invalidArgumentError('Invalid ActionPortSchema value.', [], error);
    }
  }
}

export interface ActionHeaderSchemaOptions {
  name: string;
  description?: string;
  defaultValue?: ByteSource | null;
}

export class ActionHeaderSchema {
  name: string;
  description: string;
  defaultValue: Uint8Array | null;
  private constructionStatus: Status = okStatus();

  constructor(options: ActionHeaderSchemaOptions) {
    this.name = options.name;
    this.description = options.description ?? '';
    if (options.defaultValue === undefined || options.defaultValue === null) {
      this.defaultValue = null;
    } else {
      const bytes = toBytes(options.defaultValue);
      if (isOk(bytes)) this.defaultValue = bytes;
      else {
        this.defaultValue = null;
        this.constructionStatus = bytes;
      }
    }
  }

  static create(
    options: ActionHeaderSchemaOptions,
  ): StatusOr<ActionHeaderSchema> {
    try {
      const result = new ActionHeaderSchema(options);
      const validation = result.validate();
      return isOk(validation) ? result : validation;
    } catch (error) {
      return invalidArgumentError('Invalid ActionHeaderSchema options.', [], error);
    }
  }

  validate(): Status {
    try {
      if (!isOk(this.constructionStatus)) return this.constructionStatus;
      const name = validateName(this.name);
      if (!isOk(name)) return name;
      if (
        this.defaultValue !== null &&
        !(this.defaultValue instanceof Uint8Array)
      ) {
        return invalidArgumentError(
          'Action header defaultValue must be byte data or null.',
        );
      }
      return typeof this.description === 'string'
        ? okStatus()
        : invalidArgumentError('Action header description must be a string.');
    } catch (error) {
      return invalidArgumentError('Invalid ActionHeaderSchema value.', [], error);
    }
  }
}

type PortCollection =
  | ReadonlyMap<string, ActionPortSchema>
  | Readonly<Record<string, ActionPortSchema>>;
type HeaderCollection =
  | ReadonlyMap<string, ActionHeaderSchema>
  | Readonly<Record<string, ActionHeaderSchema>>;
type StringCollection =
  | ReadonlyMap<string, string>
  | Readonly<Record<string, string>>;

export interface ActionSchemaOptions {
  name: string;
  description?: string;
  inputs?: PortCollection;
  outputs?: PortCollection;
  headers?: HeaderCollection;
  outputToJsonField?: StringCollection;
}

function collectionEntries<T>(
  value: ReadonlyMap<string, T> | Readonly<Record<string, T>> | undefined,
): Iterable<[string, T]> {
  return value instanceof Map ? value.entries() : Object.entries(value ?? {});
}

/** Typed port and header contract for an Action. */
export class ActionSchema {
  name: string;
  description: string;
  inputs: Map<string, ActionPortSchema>;
  outputs: Map<string, ActionPortSchema>;
  headers: Map<string, ActionHeaderSchema>;
  outputToJsonField: Map<string, string>;

  constructor(options: ActionSchemaOptions) {
    this.name = options.name;
    this.description = options.description ?? '';
    this.inputs = new Map(collectionEntries(options.inputs));
    this.outputs = new Map(collectionEntries(options.outputs));
    this.headers = new Map(collectionEntries(options.headers));
    this.outputToJsonField = new Map(collectionEntries(options.outputToJsonField));
  }

  static create(options: ActionSchemaOptions): StatusOr<ActionSchema> {
    try {
      const result = new ActionSchema(options);
      const validation = result.validate();
      return isOk(validation) ? result : validation;
    } catch (error) {
      return invalidArgumentError('Could not create ActionSchema.', [], error);
    }
  }

  validate(): Status {
    try {
      return this.validateUnchecked();
    } catch (error) {
      return invalidArgumentError('Invalid ActionSchema value.', [], error);
    }
  }

  private validateUnchecked(): Status {
    const name = validateName(this.name);
    if (!isOk(name)) return name;
    if (typeof this.description !== 'string') {
      return invalidArgumentError('Action description must be a string.');
    }
    const reserved = new Set([ACTION_STATUS_OUTPUT, ACTION_DISPATCH_STATUS_OUTPUT]);
    for (const ports of [this.inputs, this.outputs]) {
      if (!(ports instanceof Map)) return invalidArgumentError('Action ports must be a Map.');
      for (const [key, port] of ports) {
        const validKey = validateName(key);
        if (!isOk(validKey)) return validKey;
        if (!(port instanceof ActionPortSchema)) {
          return invalidArgumentError(`Action port '${key}' must be an ActionPortSchema.`);
        }
        const validPort = port.validate();
        if (!isOk(validPort)) return validPort;
        if (key !== port.name) {
          return invalidArgumentError(
            `Action port key '${key}' does not match port name '${port.name}'.`,
          );
        }
        if (reserved.has(key)) {
          return invalidArgumentError(`Action port name '${key}' is reserved.`);
        }
      }
    }
    for (const [key, header] of this.headers) {
      const validKey = validateName(key);
      if (!isOk(validKey)) return validKey;
      if (!(header instanceof ActionHeaderSchema)) {
        return invalidArgumentError(`Action header '${key}' must be an ActionHeaderSchema.`);
      }
      const validHeader = header.validate();
      if (!isOk(validHeader)) return validHeader;
      if (key !== header.name) {
        return invalidArgumentError(
          `Action header key '${key}' does not match header name '${header.name}'.`,
        );
      }
    }
    let wholeValues = 0;
    for (const [output, field] of this.outputToJsonField) {
      if (!this.outputs.has(output)) {
        return notFoundError(`Output '${output}' is not in the Action schema.`);
      }
      if (field === WHOLE_JSON_OUTPUT) ++wholeValues;
      else {
        const validField = validateName(field);
        if (!isOk(validField)) return validField;
      }
    }
    if (
      wholeValues > 1 ||
      (wholeValues === 1 && this.outputToJsonField.size !== 1)
    ) {
      return failedPreconditionError(
        'Only one output can map to the complete JSON value.',
      );
    }
    return okStatus();
  }

  mapOutputToJson(outputName: string, fieldName = ''): Status {
    try {
      const valid = validateName(outputName);
      if (!isOk(valid)) return valid;
      if (!this.outputs.has(outputName)) {
        return notFoundError(`Output '${outputName}' is not in the Action schema.`);
      }
      const field = fieldName || outputName;
      if (field === WHOLE_JSON_OUTPUT) {
        if (
          this.outputToJsonField.size > 0 &&
          !(
            this.outputToJsonField.size === 1 &&
            this.outputToJsonField.get(outputName) === WHOLE_JSON_OUTPUT
          )
        ) {
          return failedPreconditionError(
            'Only one output can map to the complete JSON value.',
          );
        }
      } else {
        const validField = validateName(field);
        if (!isOk(validField)) return validField;
      }
      this.outputToJsonField.set(outputName, field);
      return okStatus();
    } catch (error) {
      return invalidArgumentError('Mapping Action output raised.', [], error);
    }
  }
}

export interface ActionSettings {
  bindStreamsOnInputsByDefault?: boolean;
  bindStreamsOnOutputsByDefault?: boolean;
  clearInputsAfterRun?: boolean;
  clearOutputsAfterRun?: boolean;
}

export function statusToChunk(status: Status): StatusOr<Chunk> {
  const bytes = packStatus(status);
  if (!isOk(bytes)) return bytes;
  return Chunk.create({
    metadata: new ChunkMetadata({ mimetype: ACTION_STATUS_MIMETYPE }),
    data: bytes,
  });
}

export function decodeStatusChunk(chunk: Chunk): StatusOr<DecodedStatus> {
  if (!(chunk instanceof Chunk)) return invalidArgumentError('chunk must be a Chunk.');
  const validation = chunk.validate();
  if (!isOk(validation)) return validation;
  if (!isStatusChunk(chunk)) {
    return invalidArgumentError('Chunk does not contain an Action status.');
  }
  return decodeStatus(chunk.data);
}

/** Decode an Action status; malformed status chunks return their parsing status. */
export function statusFromChunk(chunk: Chunk): Status {
  const decoded = decodeStatusChunk(chunk);
  return isOk(decoded) ? decoded.status : decoded;
}

export function isStatusChunk(chunk: Chunk): boolean {
  return chunk instanceof Chunk && chunk.mimetype === ACTION_STATUS_MIMETYPE;
}
