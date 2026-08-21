import { type ByteMap, type ByteSource, toBytes } from './bytes.js';
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

/** MIME type used for structured action dispatch/completion status chunks. */
export const ACTION_STATUS_MIMETYPE = 'application/x-a11-status';
/**
 * Chunk metadata attribute marking a status chunk as a closure marker: the
 * producer drained the node and closed its write half with that status. The
 * marker carries no application value and is never stored.
 */
export const CLOSE_STATUS_ATTRIBUTE = 'a11-close';
/** Reserved output node carrying the action's eventual completion status. */
export const ACTION_STATUS_OUTPUT = '__status__';
/** Reserved output node acknowledging whether a remote call was dispatched. */
export const ACTION_DISPATCH_STATUS_OUTPUT = '__dispatch_status__';
/**
 * Reserved output node carrying the action's log.
 *
 * Every action has one, declared by nobody: it is not in the schema, so it never
 * appears in an {@link ActionMessage} or in a tool definition. Written through
 * `Action.log`, closed with the action's other outputs. See `./action_log.js`.
 */
export const ACTION_LOG_OUTPUT = '__log__';
/** Reserved wire action name used to request remote cancellation. */
export const CANCEL_ACTION_NAME = '__cancel__';
/** Header naming the action id targeted by a cancellation message. */
export const CANCEL_ACTION_HEADER = '__action';
/** Prefix for framework headers normally forwarded to nested actions. */
export const ACTION_HEADER_PREFIX = 'x-a11-';
/** Output mapping sentinel meaning the output is the complete JSON value. */
export const WHOLE_JSON_OUTPUT = '$';

/** Declarative input/output port fields accepted by {@link ActionPortSchema}. */
export interface ActionPortSchemaOptions {
  /** Port name used by handlers and on the wire. */
  name: string;
  /** Application type or MIME-facing type description. */
  type: string;
  /** Developer/model-facing explanation of what the port carries. */
  description?: string;
  /** Whether callers must provide the port. */
  required?: boolean;
  /** Whether the port represents one whole value rather than a stream. */
  unary?: boolean;
  /** Default input fragments; an autofilled input must otherwise be empty. */
  autofills?: readonly (NodeFragment | null)[];
  /**
   * JSON Schema for the port's payload, as text. Empty when unknown.
   *
   * The describable half of a port's type. A TypeScript type does not survive
   * to runtime, so unlike Python this side has nothing to derive one from --
   * but it carries and forwards whatever a peer sent, which is what lets a
   * model be shown a remote tool's real argument types.
   */
  jsonSchema?: string;
}

/**
 * Contract for one named action input or output node.
 *
 * Port descriptions and types form the interface shown to application code
 * and, commonly, to an LLM tool schema. `unary` guides consumers toward one
 * whole value while streaming ports may yield many fragments. Input autofills
 * can supply defaults before the handler runs, but the runtime rejects an
 * autofilled input that already contains data.
 */
export class ActionPortSchema {
  name: string;
  type: string;
  description: string;
  required: boolean;
  unary: boolean;
  autofills: Array<NodeFragment | null>;
  jsonSchema: string;

  constructor(options: ActionPortSchemaOptions) {
    this.name = options.name;
    this.type = options.type;
    this.description = options.description ?? '';
    this.required = options.required ?? false;
    this.unary = options.unary ?? false;
    this.autofills = [...(options.autofills ?? [])];
    this.jsonSchema = options.jsonSchema ?? '';
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

/** Declarative header fields accepted by {@link ActionHeaderSchema}. */
export interface ActionHeaderSchemaOptions {
  name: string;
  description?: string;
  defaultValue?: ByteSource | null;
}

/** Describes one binary metadata value accepted by an action call. */
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

/** Complete declarative interface accepted by {@link ActionSchema}. */
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

/**
 * Typed port and header contract for an {@link Action}.
 *
 * A schema is the stable boundary between callers and handlers: it names the
 * operation, defines input/output AsyncNodes, declares metadata headers, and
 * optionally maps outputs into an LLM/tool JSON result. Registries use the
 * schema to instantiate local handlers. A remote call sends the action name
 * and port mappings, not this schema; the peer resolves its own registration,
 * so caller and receiver schemas must agree on the wire-facing contract.
 */
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

  /** Construct and validate a schema before registering it. */
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
    const reserved = new Set([
      ACTION_STATUS_OUTPUT,
      ACTION_DISPATCH_STATUS_OUTPUT,
      ACTION_LOG_OUTPUT,
    ]);
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

  /** Map an output into a JSON field, or `$` as the whole result value. */
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

/** Per-instance policy for binding and retaining an action's port nodes. */
export interface ActionSettings {
  /** Mirror input-node writes to the action's bound stream by default. */
  bindStreamsOnInputsByDefault?: boolean;
  /** Mirror output-node writes to the action's bound stream by default. */
  bindStreamsOnOutputsByDefault?: boolean;
  /** Discard input nodes from the map after local completion. */
  clearInputsAfterRun?: boolean;
  /** Discard output nodes from the map after local completion. */
  clearOutputsAfterRun?: boolean;
}

/**
 * Encode a structured action status as a reserved-MIME chunk.
 *
 * With `closing` set the chunk is a node lifecycle marker rather than a value:
 * it says the producer drained the node and closed its write half with this
 * status. See {@link CLOSE_STATUS_ATTRIBUTE}.
 */
export function statusToChunk(status: Status, closing = false): StatusOr<Chunk> {
  const bytes = packStatus(status);
  if (!isOk(bytes)) return bytes;
  const attributes: ByteMap = new Map();
  if (closing) attributes.set(CLOSE_STATUS_ATTRIBUTE, Uint8Array.from([0x31])); // '1'
  return Chunk.create({
    metadata: new ChunkMetadata({ mimetype: ACTION_STATUS_MIMETYPE, attributes }),
    data: bytes,
  });
}

/** Decode a status chunk while retaining outer parsing errors separately. */
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

/** Whether a chunk carries the reserved action status MIME type. */
export function isStatusChunk(chunk: Chunk): boolean {
  return chunk instanceof Chunk && chunk.mimetype === ACTION_STATUS_MIMETYPE;
}

/** Whether a chunk is a status chunk reporting that a node's writer closed. */
export function isCloseStatusChunk(chunk: Chunk): boolean {
  return isStatusChunk(chunk) && chunk.metadata?.attributes.has(CLOSE_STATUS_ATTRIBUTE) === true;
}
