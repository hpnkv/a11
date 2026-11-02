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
import {dataCodecBridge} from './internal/wasm/native_data_codec.js';
import {statusFromBridgeFailure} from './internal/wasm/status_bridge.js';
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

function encodeNativeData(kind: string, value: Record<string, unknown>):
    StatusOr<Uint8Array> {
  const bridge = dataCodecBridge();
  if (bridge === null) return failedPreconditionError(
    'Initialize A11 WASM before encoding A11 values.');
  try { return bridge.encode(kind, value); }
  catch (error) { return statusFromBridgeFailure(error, `${kind} encode failed.`); }
}

function decodeNativeData(kind: string, bytes: Uint8Array):
    StatusOr<Record<string, unknown>> {
  const bridge = dataCodecBridge();
  if (bridge === null) return failedPreconditionError(
    'Initialize A11 WASM before decoding A11 values.');
  try { return bridge.decode(kind, bytes); }
  catch (error) { return statusFromBridgeFailure(error, `${kind} decode failed.`); }
}

/** Validate an action, port, node, session, stream, or metadata identifier. */
export function validateName(name: string): Status {
  if (typeof name !== 'string') return invalidArgumentError(
    'name must be a string.');
  const native = dataCodecBridge();
  if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before validating A11 names.');
  try { native.validateName(name); return okStatus(); }
  catch (error) { return statusFromBridgeFailure(error, 'Invalid A11 name.'); }
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
      const native = encodeNativeData('chunk_metadata', metadataToJson(this));
      return isOk(native) ? okStatus() : native;
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
    const native = encodeNativeData('chunk_metadata', metadataToJson(this));
    return native;
}

  /** Decode a concatenated MessagePack metadata record. */
  static fromMsgpack(bytes: Uint8Array): StatusOr<ChunkMetadata> {
    const native = decodeNativeData('chunk_metadata', bytes);
    return isOk(native) ? metadataFromJson(native) : native;
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
      const native = encodeNativeData('chunk', chunkToJson(this));
      return isOk(native) ? okStatus() : native;
} catch (error) {
      return invalidArgumentError('Invalid Chunk value.', [], error);
    }
  }

  /** Encode the concatenated MessagePack chunk record. */
  toMsgpack(): StatusOr<Uint8Array> {
    const native = encodeNativeData('chunk', chunkToJson(this));
    return native;
}

  /** Decode a concatenated MessagePack chunk record. */
  static fromMsgpack(bytes: Uint8Array): StatusOr<Chunk> {
    const native = decodeNativeData('chunk', bytes);
    return isOk(native) ? chunkFromJson(native) : native;
}
}

/** Scheme-labelled string fields interpreted by a host storage resolver. */
export interface StorageCredentials { scheme: string; fields?: Record<string, string>; }
/** One named service participating in a storage backend. */
export interface StorageService { role: string; endpoint?: string; credentials?: StorageCredentials | null; }
/** Logical namespace with optional backend access information. */
export interface StorageIdentity { id: string; credentials?: StorageCredentials | null; services?: StorageService[]; }

function storageCredentialsFromValue(value: unknown): StatusOr<StorageCredentials> {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return invalidArgumentError('Invalid storage credentials.');
  const raw = value as Record<string, unknown>;
  const scheme = asString(raw.scheme, 'StorageCredentials.scheme');
  if (!isOk(scheme)) return scheme;
  const valid = validateName(scheme);
  if (!isOk(valid)) return valid;
  if (raw.fields != null && (typeof raw.fields !== 'object' || Array.isArray(raw.fields))) return invalidArgumentError('Invalid credential fields.');
  const fields: Record<string, string> = Object.create(null);
  for (const [key, item] of Object.entries(raw.fields ?? {}).sort(([a], [b]) => a.localeCompare(b))) {
    const keyStatus = validateName(key);
    if (!isOk(keyStatus)) return keyStatus;
    if (typeof item !== 'string') return invalidArgumentError('Credential fields must be strings.');
    fields[key] = item;
  }
  return {scheme, fields};
}

/** Decode the structured access fields at the serialization boundary. */
export function storageIdentityFromValue(value: unknown): StatusOr<StorageIdentity> {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return invalidArgumentError('Invalid storage identity.');
  const source = value as Record<string, unknown>;
  const id = asString(source.id, 'StorageIdentity.id');
  if (!isOk(id)) return id;
  const valid = validateName(id);
  if (!isOk(valid)) return valid;
  const result: StorageIdentity = {id};
  if (source.credentials != null) {
    const access = storageCredentialsFromValue(source.credentials);
    if (!isOk(access)) return access;
    result.credentials = access;
  }
  if (source.services !== undefined) {
    if (!Array.isArray(source.services)) return invalidArgumentError('Storage services must be an array.');
    const services: StorageService[] = [];
    for (const item of source.services) {
      if (!item || typeof item !== 'object' || Array.isArray(item)) return invalidArgumentError('Invalid storage service.');
      const role = asString(item.role, 'StorageService.role');
      if (!isOk(role)) return role;
      const roleStatus = validateName(role);
      if (!isOk(roleStatus)) return roleStatus;
      const endpoint = asString(item.endpoint ?? '', 'StorageService.endpoint');
      if (!isOk(endpoint)) return endpoint;
      const service: StorageService = {role, endpoint};
      if (item.credentials != null) {
        const access = storageCredentialsFromValue(item.credentials);
        if (!isOk(access)) return access;
        service.credentials = access;
      }
      services.push(service.credentials ? {credentials: service.credentials, endpoint, role} : {endpoint, role});
    }
    if (services.length) result.services = services;
  }
  return result;
}

/** Node id and optional window accepted by {@link NodeRef}. */
export interface NodeRefOptions {
  /** Referenced node id. */
  id: string;
  /** First referenced fragment offset. */
  offset?: number;
  /** Number of fragments, or `null` for the remaining node. */
  length?: number | null;
  /** Optional host-resolved storage namespace. */
  storage?: StorageIdentity | null;
}

/** Reference to all or part of another logical node instead of inline bytes. */
export class NodeRef {
  /** Referenced node id. */
  id: string;
  /** First referenced fragment offset. */
  offset: number;
  /** Number of fragments, or `null` for the remaining node. */
  length: number | null;
  /** Optional host-resolved storage namespace. */
  storage: StorageIdentity | null;

  constructor(options: NodeRefOptions) {
    this.id = options.id;
    this.offset = options.offset ?? 0;
    this.length = options.length ?? null;
    this.storage = options.storage ?? null;
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
    return this.id.length + 5 + (this.length === null ? 0 : 8) +
      (this.storage ? new TextEncoder().encode(JSON.stringify(this.storage)).byteLength : 0);
  }

  /** Validate the id and uint32 window. */
  validate(): Status {
    try {
      const native = encodeNativeData('node_ref', nodeRefToJson(this));
      return isOk(native) ? okStatus() : native;
} catch (error) {
      return invalidArgumentError('Invalid NodeRef value.', [], error);
    }
  }

  /** Return a qualified copy with namespace or named-service credentials. */
  withCredentials(credentials: StorageCredentials, service?: string): StatusOr<NodeRef> {
    if (!this.storage) return failedPreconditionError('Credentials require a storage-qualified reference.');
    const storage = storageIdentityFromValue(this.storage);
    if (!isOk(storage)) return storage;
    const copy: StorageIdentity = {id: storage.id, ...(storage.credentials ? {credentials: storage.credentials} : {}),
      services: storage.services?.map(item => ({...item, ...(item.credentials ? {credentials: {...item.credentials, fields: {...item.credentials.fields}}} : {})})) ?? []};
    const parsed = storageCredentialsFromValue(credentials);
    if (!isOk(parsed)) return parsed;
    if (service === undefined) {
      copy.credentials = parsed;
    } else {
      const selected = copy.services?.find(item => item.role === service);
      if (!selected) return invalidArgumentError('Unknown storage service role.');
      selected.credentials = parsed;
    }
    return NodeRef.create({id: this.id, offset: this.offset, length: this.length, storage: copy});
  }

  /** Encode the concatenated MessagePack node-reference record. */
  toMsgpack(): StatusOr<Uint8Array> {
    const native = encodeNativeData('node_ref', nodeRefToJson(this));
    return native;
}

  /** Decode a concatenated MessagePack node-reference record. */
  static fromMsgpack(bytes: Uint8Array): StatusOr<NodeRef> {
    const native = decodeNativeData('node_ref', bytes);
    return isOk(native) ? nodeRefFromJson(native) : native;
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
      const native = encodeNativeData('node_fragment', fragmentToJson(this));
      return isOk(native) ? okStatus() : native;
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
    const native = encodeNativeData('node_fragment', fragmentToJson(this));
    return native;
}

  /** Decode a concatenated MessagePack fragment record. */
  static fromMsgpack(bytes: Uint8Array): StatusOr<NodeFragment> {
    const native = decodeNativeData('node_fragment', bytes);
    return isOk(native) ? fragmentFromJson(native) : native;
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
      const native = encodeNativeData('port', portToJson(this));
      return isOk(native) ? okStatus() : native;
} catch (error) {
      return invalidArgumentError('Invalid Port value.', [], error);
    }
  }

  /** Encode the concatenated MessagePack port record. */
  toMsgpack(): StatusOr<Uint8Array> {
    const native = encodeNativeData('port', portToJson(this));
    return native;
}

  /** Decode a concatenated MessagePack port record. */
  static fromMsgpack(bytes: Uint8Array): StatusOr<Port> {
    const native = decodeNativeData('port', bytes);
    return isOk(native) ? portFromJson(native) : native;
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
      const native = encodeNativeData('action_message', actionToJson(this));
      return isOk(native) ? okStatus() : native;
} catch (error) {
      return invalidArgumentError('Invalid ActionMessage value.', [], error);
    }
  }

  /** Encode the concatenated MessagePack action record. */
  toMsgpack(): StatusOr<Uint8Array> {
    const native = encodeNativeData('action_message', actionToJson(this));
    return native;
}

  /** Decode a concatenated MessagePack action record. */
  static fromMsgpack(bytes: Uint8Array): StatusOr<ActionMessage> {
    const native = decodeNativeData('action_message', bytes);
    return isOk(native) ? actionFromJson(native) : native;
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
      const native = encodeNativeData('wire_message', wireMessageJsonUnchecked(this));
      return isOk(native) ? okStatus() : native;
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
    const native = encodeNativeData('wire_message', wireMessageJsonUnchecked(this));
    return native;
}

  /** Decode a concatenated MessagePack wire record. */
  static fromMsgpack(bytes: Uint8Array): StatusOr<WireMessage> {
    const native = decodeNativeData('wire_message', bytes);
    return isOk(native) ?
      wireMessageFromJsonValueUnchecked(native) : native;
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

function nodeRefToJson(ref: NodeRef): Record<string, unknown> {
  return {id: ref.id, ...(ref.offset === 0 ? {} : {offset: ref.offset}),
    ...(ref.length === null ? {} : {length: ref.length}),
    ...(ref.storage === null ? {} : {storage: ref.storage})};
}

function fragmentToJson(fragment: NodeFragment): Record<string, unknown> {
  const result: Record<string, unknown> = {};
  if (fragment.id !== '') result.id = fragment.id;
  result.data = fragment.data instanceof Chunk
    ? chunkToJson(fragment.data)
    : nodeRefToJson(fragment.data);
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
    return wireMessageJsonUnchecked(message);
  } catch (error) {
    return statusFromUnknown(error, 'Serializing WireMessage JSON value raised.');
  }
}

function wireMessageJsonUnchecked(message: WireMessage): Record<string, unknown> {
  return {
    ...(message.nodeFragments.length === 0 ? {} : { node_fragments: message.nodeFragments.map(fragmentToJson) }),
    ...(message.actions.length === 0 ? {} : { actions: message.actions.map(actionToJson) }),
    ...(message.headers.size === 0 ? {} : { headers: byteMapToJson(message.headers) }),
  };
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
  const storage = value.storage === undefined || value.storage === null
    ? null : storageIdentityFromValue(value.storage);
  if (!isOk(storage)) return storage;
  return NodeRef.create({ id, offset, length, storage });
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
