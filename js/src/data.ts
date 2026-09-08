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

import {
  base64Decode,
  base64Encode,
  copyByteMap,
  normalizeByteMap,
  type ByteMap,
  type ByteMapInput,
} from './bytes.js';
import {
  decodeMsgpackFields,
  encodeMsgpackFields,
  msgpackByteMap,
} from './msgpack_codec.js';
import {
  failedPreconditionError,
  invalidArgumentError,
  isOk,
  notFoundError,
  okStatus,
  outOfRangeError,
  statusFromUnknown,
  type Status,
  type StatusOr,
} from './status.js';

const UINT32_MAX = 0xffff_ffff;
const UINT32_RANGE = 0x1_0000_0000;
const INT64_MIN = -(1n << 63n);
const INT64_MAX = (1n << 63n) - 1n;
const NAME_PATTERN = /^[A-Za-z0-9_](?:[A-Za-z0-9_#-]{0,253}[A-Za-z0-9_])?$/;

/** Validate an action, port, node, session, stream, or metadata identifier. */
export function validateName(name: string): Status {
  if (typeof name !== 'string' || name.length < 1 || name.length > 255) {
    return invalidArgumentError(
      'name must contain between 1 and 255 characters',
    );
  }
  if (!NAME_PATTERN.test(name)) {
    return invalidArgumentError(
      'name must start and end with an ASCII letter, digit, or underscore and contain only [a-zA-Z0-9-_#]',
    );
  }
  return okStatus();
}

function validateOptionalName(value: string): Status {
  return value === '' ? okStatus() : validateName(value);
}

function asRecord(value: unknown, context: string): StatusOr<Record<string, unknown>> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    return invalidArgumentError(`${context} must be an object.`);
  }
  return value as Record<string, unknown>;
}

function asString(value: unknown, field: string): StatusOr<string> {
  return typeof value === 'string'
    ? value
    : invalidArgumentError(`${field} must be a string.`);
}

function asBinary(value: unknown, field: string): StatusOr<Uint8Array> {
  return value instanceof Uint8Array
    ? new Uint8Array(value)
    : invalidArgumentError(`${field} must be MessagePack binary data.`);
}

function asUnsigned(
  value: unknown,
  field: string,
  maximum: number,
): StatusOr<number> {
  let numberValue: number;
  if (typeof value === 'bigint') {
    if (value < 0n || value > BigInt(maximum)) {
      return outOfRangeError(`${field} exceeds its supported range.`);
    }
    numberValue = Number(value);
  } else if (typeof value === 'number' && Number.isSafeInteger(value)) {
    numberValue = value;
  } else {
    return invalidArgumentError(`${field} must be a non-negative integer.`);
  }
  if (numberValue < 0 || numberValue > maximum) {
    return outOfRangeError(`${field} exceeds its supported range.`);
  }
  return numberValue;
}

function asSignedInt64(
  value: unknown,
  field: string,
): StatusOr<number | bigint> {
  if (typeof value === 'bigint') {
    return value >= INT64_MIN && value <= INT64_MAX
      ? value
      : outOfRangeError(`${field} exceeds int64.`);
  }
  if (typeof value !== 'number' || !Number.isSafeInteger(value)) {
    return invalidArgumentError(`${field} must be integer microseconds.`);
  }
  return value;
}

function decodeByteMap(value: unknown, field: string): StatusOr<ByteMap> {
  const object = asRecord(value, field);
  if (!isOk(object)) return object;
  const result: ByteMap = new Map();
  for (const [key, raw] of Object.entries(object)) {
    const valid = validateName(key);
    if (!isOk(valid)) {
      return invalidArgumentError(`Invalid key in ${field}: ${valid.message}`);
    }
    const bytes = asBinary(raw, `${field}.${key}`);
    if (!isOk(bytes)) return bytes;
    result.set(key, bytes);
  }
  return result;
}

function byteMapFromJson(value: unknown, field: string): StatusOr<ByteMap> {
  if (value === undefined) return new Map();
  const object = asRecord(value, field);
  if (!isOk(object)) return object;
  const result: ByteMap = new Map();
  for (const [key, raw] of Object.entries(object)) {
    const valid = validateName(key);
    if (!isOk(valid)) return valid;
    if (typeof raw !== 'string') {
      return invalidArgumentError(`${field}.${key} must be a base64 string.`);
    }
    const decoded = base64Decode(raw);
    if (!isOk(decoded)) return decoded;
    result.set(key, decoded);
  }
  return result;
}

function byteMapToJson(values: ReadonlyMap<string, Uint8Array>): Record<string, string> {
  return Object.fromEntries(
    [...values].map(([key, value]) => [key, base64Encode(value)]),
  );
}

function isValidDate(value: Date): boolean {
  try {
    return value instanceof Date && Number.isFinite(value.getTime());
  } catch {
    return false;
  }
}

/** Descriptive fields accepted by {@link ChunkMetadata}. */
export interface ChunkMetadataOptions {
  /** MIME type, including an optional canonical `type` parameter. */
  mimetype?: string;
  /** Source timestamp, or `null` when none was supplied. */
  timestamp?: Date | null;
  /** Application and runtime metadata encoded as byte values. */
  attributes?: ReadonlyMap<string, Uint8Array>;
}

/**
 * MIME type, timestamp, and binary attributes describing one {@link Chunk}.
 * Metadata travels with streamed values so a receiving agent can choose the
 * correct codec and retain application-specific context.
 */
export class ChunkMetadata {
  /** Representation and optional serialized value type. */
  mimetype: string;
  /** Source timestamp with millisecond precision in JavaScript. */
  timestamp: Date | null;
  /** Owned binary attributes keyed by valid A11 names. */
  attributes: ByteMap;

  constructor(options: ChunkMetadataOptions = {}) {
    this.mimetype = options.mimetype ?? '';
    this.timestamp = options.timestamp === undefined || options.timestamp === null
      ? null
      : new Date(options.timestamp.getTime());
    this.attributes = options.attributes === undefined
      ? new Map()
      : copyByteMap(options.attributes);
  }

  /** Construct and validate metadata while normalizing byte-like attributes. */
  static create(options: Omit<ChunkMetadataOptions, 'attributes'> & {
    attributes?: ByteMapInput;
  } = {}): StatusOr<ChunkMetadata> {
    try {
      if (typeof options.mimetype !== 'undefined' && typeof options.mimetype !== 'string') {
        return invalidArgumentError('ChunkMetadata.mimetype must be a string.');
      }
      if (
        options.timestamp !== undefined &&
        options.timestamp !== null &&
        (!(options.timestamp instanceof Date) || !isValidDate(options.timestamp))
      ) {
        return invalidArgumentError('ChunkMetadata.timestamp must be a valid Date or null.');
      }
      const attributes = normalizeByteMap(options.attributes, (key) => isOk(validateName(key)));
      if (!isOk(attributes)) return attributes;
      const result = new ChunkMetadata({ ...options, attributes });
      const status = result.validate();
      return isOk(status) ? result : status;
    } catch (error) {
      return invalidArgumentError('Invalid ChunkMetadata options.', [], error);
    }
  }

  /** Approximate encoded field size. */
  get approxBytes(): number {
    let result = this.mimetype.length + 9;
    for (const [key, value] of this.attributes) result += key.length + value.byteLength;
    return result;
  }

  /** Validate MIME, timestamp, and attribute fields. */
  validate(): Status {
    try {
      if (typeof this.mimetype !== 'string') {
        return invalidArgumentError('ChunkMetadata.mimetype must be a string.');
      }
      if (this.timestamp !== null && !isValidDate(this.timestamp)) {
        return invalidArgumentError('ChunkMetadata.timestamp must be a valid Date or null.');
      }
      if (!(this.attributes instanceof Map)) {
        return invalidArgumentError('ChunkMetadata.attributes must be a Map.');
      }
      for (const [key, value] of this.attributes) {
        const status = validateName(key);
        if (!isOk(status)) return status;
        if (!(value instanceof Uint8Array)) {
          return invalidArgumentError(
            'ChunkMetadata attribute values must be Uint8Array values.',
          );
        }
      }
      return okStatus();
    } catch (error) {
      return invalidArgumentError('Invalid ChunkMetadata value.', [], error);
    }
  }

  /** Return a copy of one attribute. */
  getAttribute(key: string): StatusOr<Uint8Array> {
    try {
      const valid = validateName(key);
      if (!isOk(valid)) return valid;
      const value = this.attributes.get(key);
      return value === undefined
        ? notFoundError(`Attribute not found: ${key}`)
        : new Uint8Array(value);
    } catch (error) {
      return statusFromUnknown(error, 'Could not read chunk attribute.');
    }
  }

  /** Set one attribute after validating its name. */
  setAttribute(key: string, value: Uint8Array): Status {
    const status = validateName(key);
    if (!isOk(status)) return status;
    try {
      this.attributes.set(key, new Uint8Array(value));
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Could not set chunk attribute.');
    }
  }

  /** Encode the concatenated MessagePack metadata record. */
  toMsgpack(): StatusOr<Uint8Array> {
    const status = this.validate();
    if (!isOk(status)) return status;
    let timestamp: number | bigint | null = null;
    if (this.timestamp !== null) {
      const micros = BigInt(Math.trunc(this.timestamp.getTime())) * 1000n;
      timestamp = micros >= BigInt(Number.MIN_SAFE_INTEGER) &&
          micros <= BigInt(Number.MAX_SAFE_INTEGER)
        ? Number(micros)
        : micros;
    }
    return encodeMsgpackFields([
      this.mimetype,
      timestamp,
      msgpackByteMap(this.attributes),
    ]);
  }

  /** Decode a concatenated MessagePack metadata record. */
  static fromMsgpack(bytes: Uint8Array): StatusOr<ChunkMetadata> {
    const fields = decodeMsgpackFields(bytes, 3, 'ChunkMetadata');
    if (!isOk(fields)) return fields;
    const mimetype = asString(fields[0], 'ChunkMetadata.mimetype');
    if (!isOk(mimetype)) return mimetype;
    let timestamp: Date | null = null;
    if (fields[1] !== null) {
      const micros = asSignedInt64(fields[1], 'ChunkMetadata.timestamp');
      if (!isOk(micros)) return micros;
      const milliseconds = typeof micros === 'bigint'
        ? Number(micros / 1000n)
        : micros / 1000;
      timestamp = new Date(milliseconds);
      if (!isValidDate(timestamp)) {
        return outOfRangeError('ChunkMetadata.timestamp is outside the Date range.');
      }
    }
    const attributes = decodeByteMap(fields[2], 'ChunkMetadata.attributes');
    if (!isOk(attributes)) return attributes;
    return ChunkMetadata.create({ mimetype, timestamp, attributes });
  }
}

/** Inline payload, reference, and metadata accepted by {@link Chunk}. */
export interface ChunkOptions {
  /** Description of the bytes, or `null` when none is available. */
  metadata?: ChunkMetadata | null;
  /** External content reference; mutually exclusive with `data`. */
  ref?: string;
  /** Owned inline payload bytes. */
  data?: Uint8Array;
}

/**
 * Smallest typed payload carried through an A11 node.
 *
 * A chunk contains owned bytes plus optional metadata, or a `ref` to data held
 * elsewhere. Serialization registries turn application objects into chunks;
 * NodeFragments add identity, sequence, and finality for streaming. An empty
 * octet-stream chunk is the explicit null/end marker used by unary flows.
 */
export class Chunk {
  /** Description of the payload. */
  metadata: ChunkMetadata | null;
  /** External content reference, or an empty string for inline data. */
  ref: string;
  /** Inline payload bytes. */
  data: Uint8Array;

  constructor(options: ChunkOptions = {}) {
    this.metadata = options.metadata ?? null;
    this.ref = options.ref ?? '';
    this.data = options.data === undefined ? new Uint8Array() : new Uint8Array(options.data);
  }

  /** Construct and validate a chunk. */
  static create(options: ChunkOptions = {}): StatusOr<Chunk> {
    try {
      if (options.metadata !== undefined && options.metadata !== null && !(options.metadata instanceof ChunkMetadata)) {
        return invalidArgumentError('Chunk.metadata must be ChunkMetadata or null.');
      }
      if (options.ref !== undefined && typeof options.ref !== 'string') {
        return invalidArgumentError('Chunk.ref must be a string.');
      }
      if (options.data !== undefined && !(options.data instanceof Uint8Array)) {
        return invalidArgumentError('Chunk.data must be Uint8Array.');
      }
      const result = new Chunk(options);
      const status = result.validate();
      return isOk(status) ? result : status;
    } catch (error) {
      return invalidArgumentError('Invalid Chunk options.', [], error);
    }
  }

  /** Approximate encoded field and payload size. */
  get approxBytes(): number {
    return this.ref.length + this.data.byteLength + (this.metadata?.approxBytes ?? 1) + 5;
  }

  /** MIME type from metadata, or an empty string. */
  get mimetype(): string {
    return this.metadata?.mimetype ?? '';
  }

  /** Whether the chunk contains neither inline nor referenced data. */
  get isEmpty(): boolean {
    return this.ref === '' && this.data.byteLength === 0;
  }

  /** Whether this is the explicit null chunk used as a final terminator. */
  get isNull(): boolean {
    return this.isEmpty && this.mimetype === 'application/octet-stream';
  }

  /** Validate payload ownership and metadata. */
  validate(): Status {
    try {
      if (typeof this.ref !== 'string') return invalidArgumentError('Chunk.ref must be a string.');
      if (!(this.data instanceof Uint8Array)) return invalidArgumentError('Chunk.data must be Uint8Array.');
      if (this.ref !== '' && this.data.byteLength !== 0) {
        return invalidArgumentError('Only one of ref or data may be set');
      }
      if (this.metadata !== null && !(this.metadata instanceof ChunkMetadata)) {
        return invalidArgumentError('Chunk.metadata must be ChunkMetadata or null.');
      }
      return this.metadata === null ? okStatus() : this.metadata.validate();
    } catch (error) {
      return invalidArgumentError('Invalid Chunk value.', [], error);
    }
  }

  /** Encode the concatenated MessagePack chunk record. */
  toMsgpack(): StatusOr<Uint8Array> {
    const status = this.validate();
    if (!isOk(status)) return status;
    const metadata = this.metadata?.toMsgpack() ?? null;
    if (metadata !== null && !isOk(metadata)) return metadata;
    return encodeMsgpackFields([metadata, this.ref, this.data]);
  }

  /** Decode a concatenated MessagePack chunk record. */
  static fromMsgpack(bytes: Uint8Array): StatusOr<Chunk> {
    const fields = decodeMsgpackFields(bytes, 3, 'Chunk');
    if (!isOk(fields)) return fields;
    let metadata: ChunkMetadata | null = null;
    if (fields[0] !== null) {
      const encoded = asBinary(fields[0], 'Chunk.metadata');
      if (!isOk(encoded)) return encoded;
      const decoded = ChunkMetadata.fromMsgpack(encoded);
      if (!isOk(decoded)) return decoded;
      metadata = decoded;
    }
    const ref = asString(fields[1], 'Chunk.ref');
    if (!isOk(ref)) return ref;
    const data = asBinary(fields[2], 'Chunk.data');
    if (!isOk(data)) return data;
    return Chunk.create({ metadata, ref, data });
  }
}

/** Node id and optional window accepted by {@link NodeRef}. */
export interface NodeRefOptions {
  /** Referenced node id. */
  id: string;
  /** First referenced fragment offset. */
  offset?: number;
  /** Number of fragments, or `null` for the remaining node. */
  length?: number | null;
}

/** Reference to all or part of another logical node instead of inline bytes. */
export class NodeRef {
  /** Referenced node id. */
  id: string;
  /** First referenced fragment offset. */
  offset: number;
  /** Number of fragments, or `null` for the remaining node. */
  length: number | null;

  constructor(options: NodeRefOptions) {
    this.id = options.id;
    this.offset = options.offset ?? 0;
    this.length = options.length ?? null;
  }

  /** Construct and validate a node reference. */
  static create(options: NodeRefOptions): StatusOr<NodeRef> {
    try {
      if (typeof options !== 'object' || options === null) {
        return invalidArgumentError('NodeRef options must be an object.');
      }
      const result = new NodeRef(options);
      const status = result.validate();
      return isOk(status) ? result : status;
    } catch (error) {
      return invalidArgumentError('Invalid NodeRef options.', [], error);
    }
  }

  /** Approximate encoded field size. */
  get approxBytes(): number {
    return this.id.length + 5 + (this.length === null ? 0 : 8);
  }

  /** Validate the id and uint32 window. */
  validate(): Status {
    try {
      const nameStatus = validateName(this.id);
      if (!isOk(nameStatus)) return nameStatus;
      if (!Number.isInteger(this.offset) || this.offset < 0 || this.offset > UINT32_MAX) {
        return invalidArgumentError('NodeRef.offset must be a uint32 integer.');
      }
      if (
        this.length !== null &&
        (!Number.isInteger(this.length) ||
          this.length < 0 ||
          this.length > UINT32_RANGE ||
          this.length + this.offset > UINT32_RANGE)
      ) {
        return invalidArgumentError('Offset + length must not exceed 2^32');
      }
      return okStatus();
    } catch (error) {
      return invalidArgumentError('Invalid NodeRef value.', [], error);
    }
  }

  /** Encode the concatenated MessagePack node-reference record. */
  toMsgpack(): StatusOr<Uint8Array> {
    const status = this.validate();
    return isOk(status)
      ? encodeMsgpackFields([this.id, this.offset, this.length])
      : status;
  }

  /** Decode a concatenated MessagePack node-reference record. */
  static fromMsgpack(bytes: Uint8Array): StatusOr<NodeRef> {
    const fields = decodeMsgpackFields(bytes, 3, 'NodeRef');
    if (!isOk(fields)) return fields;
    const id = asString(fields[0], 'NodeRef.id');
    if (!isOk(id)) return id;
    const offset = asUnsigned(fields[1], 'NodeRef.offset', UINT32_MAX);
    if (!isOk(offset)) return offset;
    let length: number | null = null;
    if (fields[2] !== null) {
      const decoded = asUnsigned(fields[2], 'NodeRef.length', UINT32_RANGE);
      if (!isOk(decoded)) return decoded;
      length = decoded;
    }
    return NodeRef.create({ id, offset, length });
  }
}

/** Stream identity, payload, sequence, and continuation fields. */
export interface NodeFragmentOptions {
  /** Destination node id. */
  id?: string;
  /** Inline chunk or reference to existing node data. */
  data?: Chunk | NodeRef;
  /** Logical sequence number, assigned by a writer when omitted. */
  seq?: number | null;
  /** Whether another fragment follows this one. */
  continued?: boolean;
}

/**
 * One sequenced piece of an AsyncNode's ordered stream.
 *
 * `id` selects the node, `seq` establishes logical order independently of
 * transport arrival, and `continued=false` declares this fragment final.
 * Payload is either an inline {@link Chunk} or a {@link NodeRef}.
 */
export class NodeFragment {
  /** Destination node id. */
  id: string;
  /** Inline chunk or node reference. */
  data: Chunk | NodeRef;
  /** Logical sequence number, or `null` before assignment. */
  seq: number | null;
  /** Whether another fragment follows. */
  continued: boolean;

  constructor(options: NodeFragmentOptions = {}) {
    this.id = options.id ?? '';
    this.data = options.data ?? new Chunk();
    this.seq = options.seq ?? null;
    this.continued = options.continued ?? false;
  }

  /** Construct and validate a fragment. */
  static create(options: NodeFragmentOptions = {}): StatusOr<NodeFragment> {
    try {
      const result = new NodeFragment(options);
      const status = result.validate();
      return isOk(status) ? result : status;
    } catch (error) {
      return invalidArgumentError('Invalid NodeFragment options.', [], error);
    }
  }

  /** Approximate encoded field and payload size. */
  get approxBytes(): number {
    return this.id.length + this.data.approxBytes + (this.seq === null ? 1 : 4) + 6;
  }

  /** Validate identity, sequence, continuation, and payload. */
  validate(): Status {
    try {
      const idStatus = validateOptionalName(this.id);
      if (!isOk(idStatus)) return idStatus;
      if (!(this.data instanceof Chunk) && !(this.data instanceof NodeRef)) {
        return invalidArgumentError('NodeFragment.data must be a Chunk or NodeRef.');
      }
      if (this.seq !== null && (!Number.isInteger(this.seq) || this.seq < 0 || this.seq > UINT32_MAX)) {
        return invalidArgumentError('NodeFragment.seq must be a uint32 integer or null.');
      }
      if (typeof this.continued !== 'boolean') {
        return invalidArgumentError('NodeFragment.continued must be boolean.');
      }
      return this.data.validate();
    } catch (error) {
      return invalidArgumentError('Invalid NodeFragment value.', [], error);
    }
  }

  /** Return inline data, or a failed-precondition status for a reference. */
  getChunk(): StatusOr<Chunk> {
    return this.data instanceof Chunk
      ? this.data
      : failedPreconditionError('Data is not a Chunk');
  }

  /** Return referenced data, or a failed-precondition status for a chunk. */
  getNodeRef(): StatusOr<NodeRef> {
    return this.data instanceof NodeRef
      ? this.data
      : failedPreconditionError('Data is not a NodeRef');
  }

  /** Encode the concatenated MessagePack fragment record. */
  toMsgpack(): StatusOr<Uint8Array> {
    const status = this.validate();
    if (!isOk(status)) return status;
    const encoded = this.data.toMsgpack();
    if (!isOk(encoded)) return encoded;
    return encodeMsgpackFields([
      this.id,
      this.data instanceof Chunk ? 0 : 1,
      encoded,
      this.seq,
      this.continued,
    ]);
  }

  /** Decode a concatenated MessagePack fragment record. */
  static fromMsgpack(bytes: Uint8Array): StatusOr<NodeFragment> {
    const fields = decodeMsgpackFields(bytes, 5, 'NodeFragment');
    if (!isOk(fields)) return fields;
    const id = asString(fields[0], 'NodeFragment.id');
    if (!isOk(id)) return id;
    const variant = asUnsigned(fields[1], 'NodeFragment.data index', 1);
    if (!isOk(variant)) return variant;
    const encoded = asBinary(fields[2], 'NodeFragment.data');
    if (!isOk(encoded)) return encoded;
    const data = variant === 0 ? Chunk.fromMsgpack(encoded) : NodeRef.fromMsgpack(encoded);
    if (!isOk(data)) return data;
    let seq: number | null = null;
    if (fields[3] !== null) {
      const parsed = asUnsigned(fields[3], 'NodeFragment.seq', UINT32_MAX);
      if (!isOk(parsed)) return parsed;
      seq = parsed;
    }
    if (typeof fields[4] !== 'boolean') {
      return invalidArgumentError('NodeFragment.continued must be bool.');
    }
    return NodeFragment.create({ id, data, seq, continued: fields[4] });
  }
}

/** Maps one schema port name onto the concrete node id for an action instance. */
export class Port {
  constructor(
    /** Schema port name. */
    public name: string = '',
    /** Concrete node id for one action instance. */
    public id: string = '',
  ) {}

  /** Construct and validate a port mapping. */
  static create(name: string = '', id: string = ''): StatusOr<Port> {
    try {
      const result = new Port(name, id);
      const status = result.validate();
      return isOk(status) ? result : status;
    } catch (error) {
      return invalidArgumentError('Invalid Port value.', [], error);
    }
  }

  /** Approximate encoded field size. */
  get approxBytes(): number { return this.name.length + this.id.length + 1; }

  /** Validate the optional port and node names. */
  validate(): Status {
    try {
      const nameStatus = validateOptionalName(this.name);
      return isOk(nameStatus) ? validateOptionalName(this.id) : nameStatus;
    } catch (error) {
      return invalidArgumentError('Invalid Port value.', [], error);
    }
  }

  /** Encode the concatenated MessagePack port record. */
  toMsgpack(): StatusOr<Uint8Array> {
    const status = this.validate();
    return isOk(status) ? encodeMsgpackFields([this.name, this.id]) : status;
  }

  /** Decode a concatenated MessagePack port record. */
  static fromMsgpack(bytes: Uint8Array): StatusOr<Port> {
    const fields = decodeMsgpackFields(bytes, 2, 'Port');
    if (!isOk(fields)) return fields;
    const name = asString(fields[0], 'Port.name');
    if (!isOk(name)) return name;
    const id = asString(fields[1], 'Port.id');
    return isOk(id) ? Port.create(name, id) : id;
  }
}

/** Action call identity, port mappings, and headers accepted on the wire. */
export interface ActionMessageOptions {
  /** Stable action-call id. */
  id?: string;
  /** Registered action name. */
  name?: string;
  /** Input port mappings. */
  inputs?: readonly Port[];
  /** Output port mappings. */
  outputs?: readonly Port[];
  /** Call-scoped binary headers. */
  headers?: ReadonlyMap<string, Uint8Array>;
}

/**
 * Wire description of one action invocation.
 *
 * The receiving session resolves `name` through its registry, maps the
 * concrete input/output node ids, applies headers, and then starts the handler.
 * Streamed values travel separately as NodeFragments in WireMessages.
 */
export class ActionMessage {
  /** Stable action-call id. */
  id: string;
  /** Registered action name. */
  name: string;
  /** Input port mappings. */
  inputs: Port[];
  /** Output port mappings. */
  outputs: Port[];
  /** Call-scoped binary headers. */
  headers: ByteMap;

  constructor(options: ActionMessageOptions = {}) {
    this.id = options.id ?? '';
    this.name = options.name ?? '';
    this.inputs = [...(options.inputs ?? [])];
    this.outputs = [...(options.outputs ?? [])];
    this.headers = options.headers === undefined ? new Map() : copyByteMap(options.headers);
  }

  /** Construct and validate an action message. */
  static create(options: ActionMessageOptions = {}): StatusOr<ActionMessage> {
    try {
      const result = new ActionMessage(options);
      const status = result.validate();
      return isOk(status) ? result : status;
    } catch (error) {
      return invalidArgumentError('Invalid ActionMessage options.', [], error);
    }
  }

  /** Approximate encoded field and payload size. */
  get approxBytes(): number {
    let result = this.id.length + this.name.length + 8;
    for (const port of [...this.inputs, ...this.outputs]) result += port.approxBytes;
    for (const [key, value] of this.headers) result += key.length + value.byteLength;
    return result;
  }

  /** Validate identity, port mappings, and headers. */
  validate(): Status {
    try {
      for (const name of [this.id, this.name]) {
        const status = validateOptionalName(name);
        if (!isOk(status)) return status;
      }
      if (!Array.isArray(this.inputs) || !Array.isArray(this.outputs)) {
        return invalidArgumentError('Action ports must be arrays.');
      }
      for (const port of [...this.inputs, ...this.outputs]) {
        if (!(port instanceof Port)) return invalidArgumentError('Action ports must be Port values.');
        const status = port.validate();
        if (!isOk(status)) return status;
      }
      if (!(this.headers instanceof Map)) {
        return invalidArgumentError('ActionMessage.headers must be a Map.');
      }
      for (const [key, value] of this.headers) {
        const status = validateName(key);
        if (!isOk(status)) return status;
        if (!(value instanceof Uint8Array)) {
          return invalidArgumentError(
            'ActionMessage header values must be Uint8Array values.',
          );
        }
      }
      return okStatus();
    } catch (error) {
      return invalidArgumentError('Invalid ActionMessage value.', [], error);
    }
  }

  /** Encode the concatenated MessagePack action record. */
  toMsgpack(): StatusOr<Uint8Array> {
    const status = this.validate();
    if (!isOk(status)) return status;
    const inputs: Uint8Array[] = [];
    const outputs: Uint8Array[] = [];
    for (const [ports, encoded] of [[this.inputs, inputs], [this.outputs, outputs]] as const) {
      for (const port of ports) {
        const bytes = port.toMsgpack();
        if (!isOk(bytes)) return bytes;
        encoded.push(bytes);
      }
    }
    return encodeMsgpackFields([this.id, this.name, inputs, outputs, msgpackByteMap(this.headers)]);
  }

  /** Decode a concatenated MessagePack action record. */
  static fromMsgpack(bytes: Uint8Array): StatusOr<ActionMessage> {
    const fields = decodeMsgpackFields(bytes, 5, 'ActionMessage');
    if (!isOk(fields)) return fields;
    const id = asString(fields[0], 'ActionMessage.id');
    if (!isOk(id)) return id;
    const name = asString(fields[1], 'ActionMessage.name');
    if (!isOk(name)) return name;
    const decodedPorts: Port[][] = [];
    for (const [index, field] of [[2, 'inputs'], [3, 'outputs']] as const) {
      const raw = fields[index];
      if (!Array.isArray(raw)) return invalidArgumentError(`ActionMessage.${field} must be a list.`);
      const ports: Port[] = [];
      for (const item of raw) {
        const encoded = asBinary(item, `ActionMessage.${field}`);
        if (!isOk(encoded)) return encoded;
        const port = Port.fromMsgpack(encoded);
        if (!isOk(port)) return port;
        ports.push(port);
      }
      decodedPorts.push(ports);
    }
    const headers = decodeByteMap(fields[4], 'ActionMessage.headers');
    if (!isOk(headers)) return headers;
    return ActionMessage.create({ id, name, inputs: decodedPorts[0], outputs: decodedPorts[1], headers });
  }
}

/** Fragments, calls, and connection metadata batched into a WireMessage. */
export interface WireMessageOptions {
  /** Node data carried by this message. */
  nodeFragments?: readonly NodeFragment[];
  /** Action calls carried by this message. */
  actions?: readonly ActionMessage[];
  /** Message-scoped binary headers. */
  headers?: ReadonlyMap<string, Uint8Array>;
}

/**
 * Atomic application message exchanged by every A11 transport.
 *
 * It batches independent node fragments and action calls to reduce transport
 * overhead. Ordering belongs to each fragment's node sequence, not to the
 * WireMessage or transport. An otherwise empty message with headers is also
 * used for lifecycle metadata such as session completion.
 */
export class WireMessage {
  /** Current MessagePack/JSON wire schema version. */
  static readonly VERSION = 1;
  /** Node data carried by this message. */
  nodeFragments: NodeFragment[];
  /** Action calls carried by this message. */
  actions: ActionMessage[];
  /** Message-scoped binary headers. */
  headers: ByteMap;

  constructor(options: WireMessageOptions = {}) {
    this.nodeFragments = [...(options.nodeFragments ?? [])];
    this.actions = [...(options.actions ?? [])];
    this.headers = options.headers === undefined ? new Map() : copyByteMap(options.headers);
  }

  /** Construct and validate a wire message. */
  static create(options: WireMessageOptions = {}): StatusOr<WireMessage> {
    try {
      const result = new WireMessage(options);
      const status = result.validate();
      return isOk(status) ? result : status;
    } catch (error) {
      return invalidArgumentError('Invalid WireMessage options.', [], error);
    }
  }

  /** Approximate encoded field and payload size. */
  get approxBytes(): number {
    let result = 8;
    for (const fragment of this.nodeFragments) result += fragment.approxBytes;
    for (const action of this.actions) result += action.approxBytes;
    for (const [key, value] of this.headers) result += key.length + value.byteLength;
    return result;
  }

  /** Validate all fragments, calls, and headers. */
  validate(): Status {
    try {
      if (!Array.isArray(this.nodeFragments)) {
        return invalidArgumentError('WireMessage.nodeFragments must be an array.');
      }
      if (!Array.isArray(this.actions)) {
        return invalidArgumentError('WireMessage.actions must be an array.');
      }
      for (const fragment of this.nodeFragments) {
        if (!(fragment instanceof NodeFragment)) return invalidArgumentError('WireMessage.nodeFragments must contain NodeFragment values.');
        const status = fragment.validate();
        if (!isOk(status)) return status;
      }
      for (const action of this.actions) {
        if (!(action instanceof ActionMessage)) return invalidArgumentError('WireMessage.actions must contain ActionMessage values.');
        const status = action.validate();
        if (!isOk(status)) return status;
      }
      if (!(this.headers instanceof Map)) {
        return invalidArgumentError('WireMessage.headers must be a Map.');
      }
      for (const [key, value] of this.headers) {
        const status = validateName(key);
        if (!isOk(status)) return status;
        if (!(value instanceof Uint8Array)) {
          return invalidArgumentError(
            'WireMessage header values must be Uint8Array values.',
          );
        }
      }
      return okStatus();
    } catch (error) {
      return invalidArgumentError('Invalid WireMessage value.', [], error);
    }
  }

  /** Whether this carries only headers and can serve as a lifecycle marker. */
  get isHalfClose(): boolean {
    return this.actions.length === 0 && this.nodeFragments.length === 0;
  }

  /** Encode the current concatenated MessagePack wire record. */
  toMsgpack(): StatusOr<Uint8Array> {
    const status = this.validate();
    if (!isOk(status)) return status;
    const fragments: Uint8Array[] = [];
    const actions: Uint8Array[] = [];
    for (const fragment of this.nodeFragments) {
      const bytes = fragment.toMsgpack();
      if (!isOk(bytes)) return bytes;
      fragments.push(bytes);
    }
    for (const action of this.actions) {
      const bytes = action.toMsgpack();
      if (!isOk(bytes)) return bytes;
      actions.push(bytes);
    }
    return encodeMsgpackFields([WireMessage.VERSION, fragments, actions, msgpackByteMap(this.headers)]);
  }

  /** Decode a concatenated MessagePack wire record. */
  static fromMsgpack(bytes: Uint8Array): StatusOr<WireMessage> {
    const fields = decodeMsgpackFields(bytes, 4, 'WireMessage');
    if (!isOk(fields)) return fields;
    const version = asUnsigned(fields[0], 'WireMessage.version', UINT32_MAX);
    if (!isOk(version)) return version;
    if (version !== WireMessage.VERSION) {
      return invalidArgumentError(`Invalid serialized WireMessage version: ${version}`);
    }
    const fragments: NodeFragment[] = [];
    const actions: ActionMessage[] = [];
    for (const [index, target, decode, field] of [
      [1, fragments, NodeFragment.fromMsgpack, 'node_fragments'],
      [2, actions, ActionMessage.fromMsgpack, 'actions'],
    ] as const) {
      const raw = fields[index];
      if (!Array.isArray(raw)) return invalidArgumentError(`WireMessage.${field} must be a list.`);
      for (const item of raw) {
        const encoded = asBinary(item, `WireMessage.${field}`);
        if (!isOk(encoded)) return encoded;
        const value = decode(encoded);
        if (!isOk(value)) return value;
        (target as Array<NodeFragment | ActionMessage>).push(value);
      }
    }
    const headers = decodeByteMap(fields[3], 'WireMessage.headers');
    if (!isOk(headers)) return headers;
    return WireMessage.create({ nodeFragments: fragments, actions, headers });
  }

  /** Convert the message to its language-neutral JSON field map. */
  toJsonValue(): StatusOr<Record<string, unknown>> {
    const status = this.validate();
    if (!isOk(status)) return status;
    return wireMessageToJsonValue(this);
  }

  /** Encode the message as JSON text. */
  toJson(): StatusOr<string> {
    const value = this.toJsonValue();
    if (!isOk(value)) return value;
    try {
      return JSON.stringify(value);
    } catch (error) {
      return statusFromUnknown(error, 'Failed to serialize WireMessage JSON.');
    }
  }

  /** Decode a message from JSON text. */
  static fromJson(value: string): StatusOr<WireMessage> {
    if (typeof value !== 'string') {
      return invalidArgumentError('WireMessage JSON must be a string.');
    }
    try {
      return wireMessageFromJsonValue(JSON.parse(value) as unknown);
    } catch (error) {
      return invalidArgumentError('Failed to parse WireMessage JSON.', [], error);
    }
  }
}

/** Build an empty lifecycle message carrying optional terminal headers. */
export function makeHalfCloseMessage(headers: ReadonlyMap<string, Uint8Array> = new Map()): WireMessage {
  return new WireMessage({ headers });
}

/** Build the explicit octet-stream null chunk used to terminate unary data. */
export function makeNullChunk(): Chunk {
  return new Chunk({ metadata: new ChunkMetadata({ mimetype: 'application/octet-stream' }) });
}

function metadataToJson(metadata: ChunkMetadata): Record<string, unknown> {
  const result: Record<string, unknown> = { mimetype: metadata.mimetype };
  if (metadata.timestamp !== null) result.timestamp = metadata.timestamp.toISOString();
  if (metadata.attributes.size > 0) result.attributes = byteMapToJson(metadata.attributes);
  return result;
}

function chunkToJson(chunk: Chunk): Record<string, unknown> {
  const result: Record<string, unknown> = {};
  if (chunk.metadata !== null) result.metadata = metadataToJson(chunk.metadata);
  if (chunk.ref !== '') result.ref = chunk.ref;
  result.data = base64Encode(chunk.data);
  return result;
}

function fragmentToJson(fragment: NodeFragment): Record<string, unknown> {
  const result: Record<string, unknown> = {};
  if (fragment.id !== '') result.id = fragment.id;
  result.data = fragment.data instanceof Chunk
    ? chunkToJson(fragment.data)
    : {
        id: fragment.data.id,
        ...(fragment.data.offset === 0 ? {} : { offset: fragment.data.offset }),
        ...(fragment.data.length === null ? {} : { length: fragment.data.length }),
      };
  if (fragment.seq !== null) result.seq = fragment.seq;
  if (fragment.continued) result.continued = true;
  return result;
}

function portToJson(port: Port): Record<string, unknown> {
  return {
    ...(port.name === '' ? {} : { name: port.name }),
    ...(port.id === '' ? {} : { id: port.id }),
  };
}

function actionToJson(action: ActionMessage): Record<string, unknown> {
  return {
    id: action.id,
    name: action.name,
    ...(action.inputs.length === 0 ? {} : { inputs: action.inputs.map(portToJson) }),
    ...(action.outputs.length === 0 ? {} : { outputs: action.outputs.map(portToJson) }),
    ...(action.headers.size === 0 ? {} : { headers: byteMapToJson(action.headers) }),
  };
}

/** Convert a WireMessage into the interoperable JSON object representation. */
export function wireMessageToJsonValue(
  message: WireMessage,
): StatusOr<Record<string, unknown>> {
  try {
    if (!(message instanceof WireMessage)) {
      return invalidArgumentError('message must be a WireMessage.');
    }
    const validation = message.validate();
    if (!isOk(validation)) return validation;
    return {
      ...(message.nodeFragments.length === 0 ? {} : { node_fragments: message.nodeFragments.map(fragmentToJson) }),
      ...(message.actions.length === 0 ? {} : { actions: message.actions.map(actionToJson) }),
      ...(message.headers.size === 0 ? {} : { headers: byteMapToJson(message.headers) }),
    };
  } catch (error) {
    return statusFromUnknown(error, 'Serializing WireMessage JSON value raised.');
  }
}

function metadataFromJson(value: unknown): StatusOr<ChunkMetadata> {
  const object = asRecord(value, 'ChunkMetadata');
  if (!isOk(object)) return object;
  const mimetype = object.mimetype === undefined ? '' : asString(object.mimetype, 'ChunkMetadata.mimetype');
  if (!isOk(mimetype)) return mimetype;
  let timestamp: Date | null = null;
  if (object.timestamp !== undefined && object.timestamp !== null) {
    if (typeof object.timestamp !== 'string') return invalidArgumentError('ChunkMetadata.timestamp must be an RFC 3339 string or null.');
    timestamp = new Date(object.timestamp);
    if (!isValidDate(timestamp)) return invalidArgumentError('Invalid ChunkMetadata.timestamp.');
  }
  const attributes = byteMapFromJson(object.attributes, 'ChunkMetadata.attributes');
  if (!isOk(attributes)) return attributes;
  return ChunkMetadata.create({ mimetype, timestamp, attributes });
}

function chunkFromJson(value: unknown): StatusOr<Chunk> {
  const object = asRecord(value, 'Chunk');
  if (!isOk(object)) return object;
  let metadata: ChunkMetadata | null = null;
  if (object.metadata !== undefined && object.metadata !== null) {
    const decoded = metadataFromJson(object.metadata);
    if (!isOk(decoded)) return decoded;
    metadata = decoded;
  }
  const ref = object.ref === undefined ? '' : asString(object.ref, 'Chunk.ref');
  if (!isOk(ref)) return ref;
  let data: Uint8Array = new Uint8Array();
  if (object.data !== undefined) {
    if (typeof object.data !== 'string') return invalidArgumentError('Chunk.data must be a base64 string.');
    const decoded = base64Decode(object.data);
    if (!isOk(decoded)) return decoded;
    data = decoded;
  }
  return Chunk.create({ metadata, ref, data });
}

function nodeRefFromJson(value: Record<string, unknown>): StatusOr<NodeRef> {
  const id = value.id === undefined ? '' : asString(value.id, 'NodeRef.id');
  if (!isOk(id)) return id;
  const offset = value.offset === undefined ? 0 : asUnsigned(value.offset, 'NodeRef.offset', UINT32_MAX);
  if (!isOk(offset)) return offset;
  let length: number | null = null;
  if (value.length !== undefined && value.length !== null) {
    const decoded = asUnsigned(value.length, 'NodeRef.length', UINT32_RANGE);
    if (!isOk(decoded)) return decoded;
    length = decoded;
  }
  return NodeRef.create({ id, offset, length });
}

function fragmentFromJson(value: unknown): StatusOr<NodeFragment> {
  const object = asRecord(value, 'NodeFragment');
  if (!isOk(object)) return object;
  const id = object.id === undefined ? '' : asString(object.id, 'NodeFragment.id');
  if (!isOk(id)) return id;
  const dataObject = asRecord(object.data, 'NodeFragment.data');
  if (!isOk(dataObject)) return dataObject;
  const isNodeRef = dataObject.id !== undefined && dataObject.data === undefined && dataObject.ref === undefined && dataObject.metadata === undefined;
  const data = isNodeRef ? nodeRefFromJson(dataObject) : chunkFromJson(dataObject);
  if (!isOk(data)) return data;
  let seq: number | null = null;
  if (object.seq !== undefined && object.seq !== null) {
    const decoded = asUnsigned(object.seq, 'NodeFragment.seq', UINT32_MAX);
    if (!isOk(decoded)) return decoded;
    seq = decoded;
  }
  if (object.continued !== undefined && typeof object.continued !== 'boolean') {
    return invalidArgumentError('NodeFragment.continued must be a boolean.');
  }
  return NodeFragment.create({ id, data, seq, continued: object.continued as boolean | undefined });
}

function portFromJson(value: unknown): StatusOr<Port> {
  const object = asRecord(value, 'Port');
  if (!isOk(object)) return object;
  const name = object.name === undefined ? '' : asString(object.name, 'Port.name');
  if (!isOk(name)) return name;
  const id = object.id === undefined ? '' : asString(object.id, 'Port.id');
  return isOk(id) ? Port.create(name, id) : id;
}

function actionFromJson(value: unknown): StatusOr<ActionMessage> {
  const object = asRecord(value, 'ActionMessage');
  if (!isOk(object)) return object;
  const id = object.id === undefined ? '' : asString(object.id, 'ActionMessage.id');
  if (!isOk(id)) return id;
  const name = object.name === undefined ? '' : asString(object.name, 'ActionMessage.name');
  if (!isOk(name)) return name;
  const ports: [Port[], Port[]] = [[], []];
  for (const [index, field] of [[0, 'inputs'], [1, 'outputs']] as const) {
    const raw = object[field];
    if (raw === undefined) continue;
    if (!Array.isArray(raw)) return invalidArgumentError(`ActionMessage.${field} must be an array.`);
    for (const item of raw) {
      const port = portFromJson(item);
      if (!isOk(port)) return port;
      ports[index].push(port);
    }
  }
  const headers = byteMapFromJson(object.headers, 'ActionMessage.headers');
  if (!isOk(headers)) return headers;
  return ActionMessage.create({ id, name, inputs: ports[0], outputs: ports[1], headers });
}

/** Validate and build a WireMessage from its decoded JSON representation. */
export function wireMessageFromJsonValue(value: unknown): StatusOr<WireMessage> {
  try {
    return wireMessageFromJsonValueUnchecked(value);
  } catch (error) {
    return invalidArgumentError('Invalid WireMessage JSON value.', [], error);
  }
}

function wireMessageFromJsonValueUnchecked(
  value: unknown,
): StatusOr<WireMessage> {
  const object = asRecord(value, 'WireMessage');
  if (!isOk(object)) return object;
  const fragments: NodeFragment[] = [];
  const actions: ActionMessage[] = [];
  if (object.node_fragments !== undefined) {
    if (!Array.isArray(object.node_fragments)) return invalidArgumentError('WireMessage.node_fragments must be an array.');
    for (const item of object.node_fragments) {
      const fragment = fragmentFromJson(item);
      if (!isOk(fragment)) return fragment;
      fragments.push(fragment);
    }
  }
  if (object.actions !== undefined) {
    if (!Array.isArray(object.actions)) return invalidArgumentError('WireMessage.actions must be an array.');
    for (const item of object.actions) {
      const action = actionFromJson(item);
      if (!isOk(action)) return action;
      actions.push(action);
    }
  }
  const headers = byteMapFromJson(object.headers, 'WireMessage.headers');
  if (!isOk(headers)) return headers;
  return WireMessage.create({ nodeFragments: fragments, actions, headers });
}
