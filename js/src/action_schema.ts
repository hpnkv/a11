/**
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

import { type ByteSource, toBytes } from './bytes.js';
import { Chunk, NodeFragment, validateName } from './data.js';
import {schemaBridge} from './internal/wasm/native_schema.js';
import {statusCodecBridge} from './internal/wasm/native_status_codec.js';
import {statusFromBridgeFailure} from './internal/wasm/status_bridge.js';
import { type DecodedStatus } from './status_codec.js';
import {
  failedPreconditionError,
  invalidArgumentError,
  isOk,
  notFoundError,
  okStatus,
  statusToJson,
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
/** Reserved output discovering auxiliary streams in an action subtree. */
export const ACTION_STREAMS_OUTPUT = '__streams__';
/** Header enabling auxiliary stream propagation across action calls. */
export const STREAM_SCOPE_HEADER = 'x-a11-stream-scope';
/** Collector storage namespaces that require no payload mirroring. */
export const STREAM_STORAGE_HEADER = 'x-a11-stream-storage';
/** Reserved wire action name used to request remote cancellation. */
export const CANCEL_ACTION_NAME = '__cancel__';
/** Header naming the action id targeted by a cancellation message. */
export const CANCEL_ACTION_HEADER = '__action';
/** Prefix for framework headers normally forwarded to nested actions. */
export const ACTION_HEADER_PREFIX = 'x-a11-';
/** Connection-scoped authorization reference; never forwarded to a child. */
export const AUTHORIZATION_REFERENCE_HEADER = 'x-a11-auth-ref';
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
  /** Port name used by handlers and on the wire. */
  name: string;
  /** MIME-facing type description. */
  type: string;
  /** Developer and model-facing explanation of the value. */
  description: string;
  /** Whether callers must provide the port. */
  required: boolean;
  /** Whether the port carries at most one value. */
  unary: boolean;
  /** Default input fragments applied before a handler runs. */
  autofills: Array<NodeFragment | null>;
  /** JSON Schema text supplied by a peer or caller. */
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

  /** Construct and validate a port schema. */
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

  /** Validate the port contract and autofill fragments. */
  validate(): Status {
    try {
      const native = schemaBridge();
      if (native === null) return failedPreconditionError(
        'Initialize A11 WASM before validating action ports.');
      return native.validate(new ActionSchema({name: 'a11_port_validation',
        inputs: {[this.name]: this}}));
    } catch (error) {
      return invalidArgumentError('Invalid ActionPortSchema value.', [], error);
    }
  }
}

/** Declarative header fields accepted by {@link ActionHeaderSchema}. */
export interface ActionHeaderSchemaOptions {
  /** Header name. */
  name: string;
  /** Developer-facing explanation of the metadata. */
  description?: string;
  /** Receiver-owned default bytes. */
  defaultValue?: ByteSource | null;
}

/** Describes one binary metadata value accepted by an action call. */
export class ActionHeaderSchema {
  /** Header name. */
  name: string;
  /** Developer-facing explanation of the metadata. */
  description: string;
  /** Receiver-owned default bytes. */
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

  /** Construct and validate a header schema. */
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

  /** Validate the name, description, and default bytes. */
  validate(): Status {
    try {
      if (!isOk(this.constructionStatus)) return this.constructionStatus;
      const native = schemaBridge();
      if (native === null) return failedPreconditionError(
        'Initialize A11 WASM before validating action headers.');
      return native.validate(new ActionSchema({name: 'a11_header_validation',
        headers: {[this.name]: this}}));
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
  /** Registered operation name. */
  name: string;
  /** Developer and model-facing operation description. */
  description?: string;
  /** Named input contracts. */
  inputs?: PortCollection;
  /** Named output contracts. */
  outputs?: PortCollection;
  /** Named binary metadata contracts. */
  headers?: HeaderCollection;
  /** Output port names mapped to tool-result JSON fields. */
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
  /** Registered operation name. */
  name: string;
  /** Developer and model-facing operation description. */
  description: string;
  /** Named input contracts. */
  inputs: Map<string, ActionPortSchema>;
  /** Named output contracts. */
  outputs: Map<string, ActionPortSchema>;
  /** Named binary metadata contracts. */
  headers: Map<string, ActionHeaderSchema>;
  /** Output ports mapped to tool-result JSON fields. */
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

  /** Validate names, contracts, reserved ports, and output mappings. */
  validate(): Status {
    try {
      const native = schemaBridge();
      if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before using A11 protocol codecs.');
      return native.validate(this);
    } catch (error) {
      return invalidArgumentError('Invalid ActionSchema value.', [], error);
    }
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
  const native = statusCodecBridge();
  if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before using A11 protocol codecs.');

    try { return Chunk.fromMsgpack(native.chunk(statusToJson(status), closing)); }
  catch (error) { return statusFromBridgeFailure(error,
      'Native status chunk encoding failed.'); }
}

/** Decode a status chunk while retaining outer parsing errors separately. */
export function decodeStatusChunk(chunk: Chunk): StatusOr<DecodedStatus> {
  if (!(chunk instanceof Chunk)) return invalidArgumentError('chunk must be a Chunk.');
  const native = statusCodecBridge();
  if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before using A11 protocol codecs.');
  const bytes = chunk.toMsgpack();
    if (!isOk(bytes)) return bytes;
    try { return {status: native.fromChunk(bytes)}; }
  catch (error) { return statusFromBridgeFailure(error,
      'Native status chunk decoding failed.'); }
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
