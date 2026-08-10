/**
 * The tag → class table for values A11 serializes whole.
 *
 * A JSON or MessagePack payload is a tree of plain data, but some of what A11
 * puts on the wire is not plain: an `Interaction`, a {@link Chunk}, a
 * {@link Status}. A chunk holding one of those names it in its metadata
 * (`application/json;type=a11.Chunk`), and this module is what turns that name
 * into an object and an object back into that name.
 *
 * The tag comes from the canonical table in {@link serial_tags}, so a value
 * written by one language is read by another. The runtime's own data types and
 * declared models such as SDK configs share one namespace: what a tag resolves
 * to is what tells them apart.
 *
 * Nothing inside the payload is tagged. A model's own fields say what they
 * hold, and schemaless data is just data — a peer reading `application/json`
 * with no type parameter gets objects, arrays and scalars, which is all the
 * format ever promised.
 *
 * A type joins the registry through {@link registerWireValueCodec}. Values that
 * have no class of their own in TypeScript — an SDK model is a plain object
 * here, not an instance — identify themselves with a non-enumerable brand; see
 * {@link tagValue}.
 *
 * @packageDocumentation
 */

import {
  ActionMessage,
  Chunk,
  ChunkMetadata,
  NodeFragment,
  NodeRef,
  Port,
  WireMessage,
} from './data.js';
import { base64Decode } from './bytes.js';
import * as tags from './serial_tags.js';
import {
  alreadyExistsError,
  invalidArgumentError,
  isOk,
  okStatus,
  type Status,
  type StatusOr,
} from './status.js';

/**
 * Brand marking a plain object as an instance of a tagged A11 type.
 *
 * Python and Kotlin can ask a value what class it is; TypeScript often cannot,
 * because the types that matter here — an `Interaction`, a provider config, a
 * `Status` — are declared with `zod` or as interfaces and reach runtime as
 * ordinary objects. Recognising them by shape is not good enough: a caller
 * sending their own `{code, message}` would have it silently re-typed as a
 * `Status` on the wire. So a value carries its tag explicitly, under a symbol
 * and non-enumerable, which keeps it out of `Object.entries`, `JSON.stringify`
 * and every equality check a caller might run.
 */
export const A11_SERIAL_TAG = Symbol.for('a11.serialTag');

/** Brand `value` as `tag`, and return it. */
export function tagValue<T extends object>(value: T, tag: string): T {
  Object.defineProperty(value, A11_SERIAL_TAG, {
    value: tag,
    enumerable: false,
    writable: true,
    configurable: true,
  });
  return value;
}

/** The tag `value` was branded with, or `null` if it carries none. */
export function valueTag(value: unknown): string | null {
  if (typeof value !== 'object' || value === null) return null;
  const tag = (value as Record<symbol, unknown>)[A11_SERIAL_TAG];
  return typeof tag === 'string' && tag !== '' ? tag : null;
}

/** A `test` that matches exactly the values branded with `tag`. */
export function testTagged(tag: string): (value: unknown) => boolean {
  return (value) => valueTag(value) === tag;
}

/** Plain field map, the form a value takes between object and wire. */
export type Fields = Record<string, unknown>;

/**
 * How one class is written into, and read back out of, a tagged wire object.
 *
 * `dump` and `load` deal in *plain fields only* — the surrounding encoder walks
 * whatever they return, so a field may itself hold bytes, a Date, or another
 * registered value without either side knowing.
 */
export interface WireValueCodec<T = unknown> {
  /** Canonical tag from {@link serial_tags}, written as the object's key. */
  readonly tag: string;
  readonly test: (value: unknown) => boolean;
  readonly dump: (value: T) => StatusOr<Fields>;
  readonly load: (fields: Fields) => StatusOr<T>;
}

const codecs: WireValueCodec[] = [];
const byTag = new Map<string, WireValueCodec>();

/**
 * Register a class so it survives being nested in a serialized value.
 *
 * Registration is idempotent for the same tag: importing a module twice must
 * not fail, but two different codecs claiming one tag is a bug worth reporting.
 */
export function registerWireValueCodec<T>(codec: WireValueCodec<T>): Status {
  if (typeof codec?.tag !== 'string' || codec.tag === '') {
    return invalidArgumentError('A wire value codec tag must be non-empty.');
  }
  if (
    typeof codec.test !== 'function' ||
    typeof codec.dump !== 'function' ||
    typeof codec.load !== 'function'
  ) {
    return invalidArgumentError(
      `The wire value codec for ${codec.tag} must provide test, dump and load.`,
    );
  }
  const existing = byTag.get(codec.tag);
  if (existing !== undefined) {
    if (existing === (codec as WireValueCodec)) return okStatus();
    return alreadyExistsError(`A wire value codec for ${codec.tag} is already registered.`);
  }
  byTag.set(codec.tag, codec as WireValueCodec);
  codecs.push(codec as WireValueCodec);
  return okStatus();
}

/** The codec that claims `value`, or `null` when it is ordinary data. */
export function wireValueCodecFor(value: unknown): WireValueCodec | null {
  for (const codec of codecs) if (codec.test(value)) return codec;
  return null;
}

/** Every registered codec, in registration order. */
export function wireValueCodecs(): readonly WireValueCodec[] {
  return codecs;
}

/**
 * How many codecs are registered.
 *
 * A registry that derives state from this one watches this count to know its
 * derivation is stale: SDK modules register on import, which can happen long
 * after a serialization registry was built.
 */
export function wireValueCodecCount(): number {
  return codecs.length;
}

/** The codec registered for `tag`. */
export function wireValueCodecByTag(tag: string): WireValueCodec | null {
  return byTag.get(tag) ?? null;
}

// --- Field helpers -----------------------------------------------------------
//
// The runtime's types omit fields that hold their default, exactly as pydantic's
// model_dump() does on the Python side. Byte-for-byte agreement matters: a value
// decoded here and sent back must be the payload the peer sent, not a re-spelling
// of it.

function put(fields: Fields, key: string, value: unknown, omit: boolean): void {
  if (!omit) fields[key] = value;
}

function readString(fields: Fields, key: string, fallback = ''): string {
  const value = fields[key];
  return typeof value === 'string' ? value : fallback;
}

function readBytes(value: unknown): Uint8Array {
  if (value instanceof Uint8Array) return value;
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  // An untagged byte field arrives as base64 from JSON and as real bytes from
  // MessagePack; both are the same field, so both have to read.
  if (typeof value === 'string') {
    const decoded = base64Decode(value);
    return isOk(decoded) ? decoded : new Uint8Array();
  }
  return new Uint8Array();
}

function readByteMap(value: unknown): Map<string, Uint8Array> {
  const result = new Map<string, Uint8Array>();
  if (value instanceof Map) {
    for (const [key, item] of value) result.set(String(key), readBytes(item));
  } else if (typeof value === 'object' && value !== null) {
    for (const [key, item] of Object.entries(value)) result.set(key, readBytes(item));
  }
  return result;
}

function byteMapFields(map: Map<string, Uint8Array>): Fields {
  const result: Fields = {};
  for (const [key, value] of map) result[key] = value;
  return result;
}

// --- The runtime's own types -------------------------------------------------

function dumpChunkMetadata(value: ChunkMetadata): StatusOr<Fields> {
  const fields: Fields = { mimetype: value.mimetype };
  put(fields, 'timestamp', value.timestamp, value.timestamp === null);
  put(fields, 'attributes', byteMapFields(value.attributes), value.attributes.size === 0);
  return fields;
}

function loadChunkMetadata(fields: Fields): StatusOr<ChunkMetadata> {
  const raw = fields['timestamp'];
  // Untagged, a timestamp is the RFC 3339 string the field's type implies.
  let timestamp: Date | null = raw instanceof Date ? raw : null;
  if (typeof raw === 'string') {
    const parsed = new Date(raw);
    if (Number.isFinite(parsed.getTime())) timestamp = parsed;
  }
  return new ChunkMetadata({
    mimetype: readString(fields, 'mimetype'),
    timestamp,
    attributes: readByteMap(fields['attributes']),
  });
}

function dumpChunk(value: Chunk): StatusOr<Fields> {
  const fields: Fields = { data: value.data };
  if (value.metadata !== null) {
    const metadata = dumpChunkMetadata(value.metadata);
    if (!isOk(metadata)) return metadata;
    fields['metadata'] = metadata;
  }
  put(fields, 'ref', value.ref, value.ref === '');
  return fields;
}

function loadChunk(fields: Fields): StatusOr<Chunk> {
  const metadata = fields['metadata'];
  let parsed: ChunkMetadata | null = null;
  if (metadata instanceof ChunkMetadata) {
    parsed = metadata;
  } else if (typeof metadata === 'object' && metadata !== null) {
    const loaded = loadChunkMetadata(metadata as Fields);
    if (!isOk(loaded)) return loaded;
    parsed = loaded;
  }
  return new Chunk({
    data: readBytes(fields['data']),
    metadata: parsed,
    ref: readString(fields, 'ref'),
  });
}

function dumpNodeRef(value: NodeRef): StatusOr<Fields> {
  const fields: Fields = { id: value.id };
  put(fields, 'offset', value.offset, value.offset === 0);
  put(fields, 'length', value.length, value.length === null);
  return fields;
}

function loadNodeRef(fields: Fields): StatusOr<NodeRef> {
  const length = fields['length'];
  const offset = fields['offset'];
  return new NodeRef({
    id: readString(fields, 'id'),
    offset: typeof offset === 'number' ? offset : Number(offset ?? 0),
    length: length === null || length === undefined ? null : Number(length),
  });
}

function dumpNodeFragment(value: NodeFragment): StatusOr<Fields> {
  const data = value.data instanceof NodeRef ? dumpNodeRef(value.data) : dumpChunk(value.data);
  if (!isOk(data)) return data;
  const fields: Fields = { data };
  put(fields, 'id', value.id, value.id === '');
  put(fields, 'seq', value.seq, value.seq === null);
  put(fields, 'continued', value.continued, !value.continued);
  return fields;
}

function loadNodeFragment(fields: Fields): StatusOr<NodeFragment> {
  const raw = fields['data'];
  let data: Chunk | NodeRef;
  if (raw instanceof Chunk || raw instanceof NodeRef) {
    data = raw;
  } else if (typeof raw === 'object' && raw !== null) {
    // A NodeRef payload has no `data` key of its own; a Chunk always does.
    const inner = raw as Fields;
    const loaded = 'data' in inner || 'metadata' in inner ? loadChunk(inner) : loadNodeRef(inner);
    if (!isOk(loaded)) return loaded;
    data = loaded;
  } else {
    data = new Chunk();
  }
  const seq = fields['seq'];
  return new NodeFragment({
    id: readString(fields, 'id'),
    data,
    seq: seq === null || seq === undefined ? null : Number(seq),
    continued: fields['continued'] === true,
  });
}

function dumpPort(value: Port): StatusOr<Fields> {
  const fields: Fields = {};
  put(fields, 'name', value.name, value.name === '');
  put(fields, 'id', value.id, value.id === '');
  return fields;
}

function loadPort(fields: Fields): StatusOr<Port> {
  return new Port(readString(fields, 'name'), readString(fields, 'id'));
}

function dumpPorts(ports: Port[]): StatusOr<Fields[]> {
  const result: Fields[] = [];
  for (const port of ports) {
    const dumped = dumpPort(port);
    if (!isOk(dumped)) return dumped;
    result.push(dumped);
  }
  return result;
}

function loadPorts(value: unknown): Port[] {
  if (!Array.isArray(value)) return [];
  return value.map((entry) =>
    entry instanceof Port
      ? entry
      : new Port(
          readString((entry ?? {}) as Fields, 'name'),
          readString((entry ?? {}) as Fields, 'id'),
        ),
  );
}

function dumpActionMessage(value: ActionMessage): StatusOr<Fields> {
  const inputs = dumpPorts(value.inputs);
  if (!isOk(inputs)) return inputs;
  const outputs = dumpPorts(value.outputs);
  if (!isOk(outputs)) return outputs;
  const fields: Fields = { id: value.id, name: value.name };
  put(fields, 'inputs', inputs, inputs.length === 0);
  put(fields, 'outputs', outputs, outputs.length === 0);
  put(fields, 'headers', byteMapFields(value.headers), value.headers.size === 0);
  return fields;
}

function loadActionMessage(fields: Fields): StatusOr<ActionMessage> {
  return new ActionMessage({
    id: readString(fields, 'id'),
    name: readString(fields, 'name'),
    inputs: loadPorts(fields['inputs']),
    outputs: loadPorts(fields['outputs']),
    headers: readByteMap(fields['headers']),
  });
}

function dumpWireMessage(value: WireMessage): StatusOr<Fields> {
  const actions: Fields[] = [];
  for (const action of value.actions) {
    const dumped = dumpActionMessage(action);
    if (!isOk(dumped)) return dumped;
    actions.push(dumped);
  }
  const fragments: Fields[] = [];
  for (const fragment of value.nodeFragments) {
    const dumped = dumpNodeFragment(fragment);
    if (!isOk(dumped)) return dumped;
    fragments.push(dumped);
  }
  const fields: Fields = {};
  put(fields, 'actions', actions, actions.length === 0);
  put(fields, 'node_fragments', fragments, fragments.length === 0);
  put(fields, 'headers', byteMapFields(value.headers), value.headers.size === 0);
  return fields;
}

function loadWireMessage(fields: Fields): StatusOr<WireMessage> {
  const actions: ActionMessage[] = [];
  for (const entry of Array.isArray(fields['actions']) ? fields['actions'] : []) {
    const loaded =
      entry instanceof ActionMessage ? entry : loadActionMessage((entry ?? {}) as Fields);
    if (!isOk(loaded)) return loaded;
    actions.push(loaded);
  }
  const fragments: NodeFragment[] = [];
  for (const entry of Array.isArray(fields['node_fragments']) ? fields['node_fragments'] : []) {
    const loaded =
      entry instanceof NodeFragment ? entry : loadNodeFragment((entry ?? {}) as Fields);
    if (!isOk(loaded)) return loaded;
    fragments.push(loaded);
  }
  return new WireMessage({
    actions,
    nodeFragments: fragments,
    headers: readByteMap(fields['headers']),
  });
}

/**
 * A Status carried as data — an interaction's outcome, say.
 *
 * `Status` is an interface here, not a class, so a Status is recognised by its
 * brand rather than its shape: an ordinary `{code, message}` object a caller is
 * sending as data must stay ordinary data. Decoding brands what it builds, so a
 * Status that arrived from a peer goes back out as one.
 */
export function asStatusValue(status: Status): Status {
  return tagValue({ ...status }, tags.STATUS_TAG);
}

function dumpStatus(value: Status): StatusOr<Fields> {
  const fields: Fields = { code: value.code, message: value.message };
  const details = value.details ?? [];
  put(fields, 'details', details, details.length === 0);
  return fields;
}

function loadStatus(fields: Fields): StatusOr<Status> {
  const code = fields['code'];
  const details = fields['details'];
  const status: Status = {
    code: typeof code === 'number' ? code : Number(code ?? 0),
    message: readString(fields, 'message'),
  };
  if (Array.isArray(details) && details.length > 0) {
    status.details = details as Status['details'];
  }
  return tagValue(status, tags.STATUS_TAG);
}

function install(): void {
  const entries: WireValueCodec<never>[] = [
    {
      tag: tags.CHUNK_METADATA_TAG,
      test: (value) => value instanceof ChunkMetadata,
      dump: dumpChunkMetadata,
      load: loadChunkMetadata,
    },
    {
      tag: tags.CHUNK_TAG,
      test: (value) => value instanceof Chunk,
      dump: dumpChunk,
      load: loadChunk,
    },
    {
      tag: tags.NODE_REF_TAG,
      test: (value) => value instanceof NodeRef,
      dump: dumpNodeRef,
      load: loadNodeRef,
    },
    {
      tag: tags.NODE_FRAGMENT_TAG,
      test: (value) => value instanceof NodeFragment,
      dump: dumpNodeFragment,
      load: loadNodeFragment,
    },
    {
      tag: tags.PORT_TAG,
      test: (value) => value instanceof Port,
      dump: dumpPort,
      load: loadPort,
    },
    {
      tag: tags.ACTION_MESSAGE_TAG,
      test: (value) => value instanceof ActionMessage,
      dump: dumpActionMessage,
      load: loadActionMessage,
    },
    {
      tag: tags.WIRE_MESSAGE_TAG,
      test: (value) => value instanceof WireMessage,
      dump: dumpWireMessage,
      load: loadWireMessage,
    },
    {
      tag: tags.STATUS_TAG,
      test: testTagged(tags.STATUS_TAG),
      dump: dumpStatus,
      load: loadStatus,
    },
  ] as WireValueCodec<never>[];
  for (const codec of entries) registerWireValueCodec(codec);
}

install();
