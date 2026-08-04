import { decode, encode } from '@msgpack/msgpack';

import { toBytesAsync, utf8Decode, utf8Encode, type AsyncByteSource } from './bytes.js';
import { Chunk, ChunkMetadata } from './data.js';
import {
  alreadyExistsError,
  invalidArgumentError,
  isOk,
  notFoundError,
  statusFromUnknown,
  type Status,
  type StatusOr,
} from './status.js';

/** Default interoperable text codec media type. */
export const JSON_MIMETYPE = 'application/json';
/** Default compact binary object codec media type. */
export const MSGPACK_MIMETYPE = 'application/x-msgpack';
/** Raw byte/blob codec media type. */
export const OCTET_STREAM_MIMETYPE = 'application/octet-stream';

const WIRE_TAG = '__a11_serialized_type__';
const WIRE_VALUE = 'value';
const MIME_TOKEN = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
const LEGACY_TYPE_TAGS: Readonly<Record<string, string>> = {
  dict: 'object',
  list: 'array',
  tuple: 'array',
  int: 'integer',
  float: 'number',
  str: 'string',
  bool: 'boolean',
  NoneType: 'null',
  bytearray: 'bytes',
  frozenset: 'set',
};

interface ParsedMimetype {
  mediaType: string;
  parameters: Map<string, string>;
}

function parseMimetype(value: string, patterns = false): StatusOr<ParsedMimetype> {
  if (typeof value !== 'string' || value.trim() === '') {
    return invalidArgumentError('Mimetype must be a non-empty string.');
  }
  const [rawMediaType, ...rawParameters] = value.split(';');
  const mediaType = (rawMediaType ?? '').trim().toLowerCase();
  const mediaParts = mediaType.split('/');
  const validPart = (part: string): boolean =>
    part.length > 0 && (patterns ? /^[!#$%&'*+.^_`|~0-9A-Za-z?*\[\]-]+$/.test(part) : MIME_TOKEN.test(part));
  if (mediaParts.length !== 2 || !validPart(mediaParts[0] ?? '') || !validPart(mediaParts[1] ?? '')) {
    return invalidArgumentError(`Invalid mimetype: ${value}.`);
  }
  const parameters = new Map<string, string>();
  for (const raw of rawParameters) {
    const separator = raw.indexOf('=');
    if (separator < 1) return invalidArgumentError(`Invalid mimetype parameter in ${value}.`);
    const name = raw.slice(0, separator).trim().toLowerCase();
    let parameter = raw.slice(separator + 1).trim();
    if (parameter.startsWith('"') && parameter.endsWith('"')) {
      parameter = parameter.slice(1, -1).replaceAll('\\"', '"').replaceAll('\\\\', '\\');
    }
    if (!MIME_TOKEN.test(name) || parameter === '' || parameters.has(name)) {
      return invalidArgumentError(`Invalid mimetype parameter in ${value}.`);
    }
    parameters.set(name, parameter);
  }
  return { mediaType, parameters };
}

function formatMimetype(mimetype: ParsedMimetype, tag: string): string {
  const parameters = [...mimetype.parameters].filter(([name]) => name !== 'type');
  parameters.push(['type', encodeURIComponent(tag)]);
  return `${mimetype.mediaType}${parameters.map(([name, value]) => `;${name}=${value}`).join('')}`;
}

function wildcardMatches(value: string, pattern: string): boolean {
  const escaped = pattern.replace(/[.+^${}()|\\]/g, '\\$&').replaceAll('*', '.*').replaceAll('?', '.');
  try {
    return new RegExp(`^${escaped}$`).test(value);
  } catch {
    return false;
  }
}

function mimetypeMatches(actual: ParsedMimetype, selection: ParsedMimetype): boolean {
  if (!wildcardMatches(actual.mediaType, selection.mediaType)) return false;
  for (const [name, expected] of selection.parameters) {
    const value = actual.parameters.get(name);
    if (value === undefined || !wildcardMatches(value, expected)) return false;
  }
  return true;
}

function registrationMatches(
  registered: ParsedMimetype,
  selection: ParsedMimetype,
  tag: string,
): boolean {
  if (!wildcardMatches(registered.mediaType, selection.mediaType)) return false;
  for (const [name, expected] of selection.parameters) {
    if (name === 'type') {
      let decoded: string;
      try { decoded = decodeURIComponent(expected); }
      catch { return false; }
      if (!wildcardMatches(tag, canonicalTypeTag(decoded))) return false;
      continue;
    }
    const value = registered.parameters.get(name);
    if (value !== undefined && !wildcardMatches(value, expected)) return false;
  }
  return true;
}

/** Bytes/blob or a prebuilt Chunk returned by an application serializer. */
export type SerializedData = AsyncByteSource | Chunk;

/**
 * Bidirectional application-value codec registered at the node boundary.
 * `tag` is stable across languages and is written into the MIME `type`
 * parameter so a remote peer can select the matching decoder.
 */
export interface SerializationCodec<T = unknown> {
  /** Stable, cross-language value tag written as the MIME `type` parameter. */
  readonly tag: string;
  readonly mimetype: string;
  readonly test: (value: unknown) => value is T;
  readonly serialize: (
    value: T,
  ) => StatusOr<SerializedData> | Promise<StatusOr<SerializedData>>;
  readonly deserialize: (
    data: Uint8Array,
    chunk: Chunk,
  ) => T | Promise<StatusOr<T>> | StatusOr<T>;
}

interface RegisteredCodec<T = unknown> extends SerializationCodec<T> {
  parsed: ParsedMimetype;
  order: number;
}

function canonicalJsonTag(value: unknown): string | null {
  if (value === null) return 'null';
  switch (typeof value) {
    case 'boolean': return 'boolean';
    case 'string': return 'string';
    case 'number': return Number.isInteger(value) ? 'integer' : 'number';
    case 'object':
      if (Array.isArray(value)) return 'array';
      if (Object.getPrototypeOf(value) === Object.prototype || Object.getPrototypeOf(value) === null) return 'object';
  }
  return null;
}

function canonicalTypeTag(tag: string): string {
  return LEGACY_TYPE_TAGS[tag] ?? tag;
}

function wireTag(name: string, value: unknown): Record<string, unknown> {
  return { [WIRE_TAG]: name, [WIRE_VALUE]: value };
}

function toWire(value: unknown, binary: boolean, topLevel = false, seen = new Set<object>()): StatusOr<unknown> {
  if (value === null || typeof value === 'boolean' || typeof value === 'string') return value;
  if (typeof value === 'number') {
    if (Number.isFinite(value)) return value;
    return wireTag('float', Number.isNaN(value) ? 'nan' : value > 0 ? '+inf' : '-inf');
  }
  if (typeof value === 'bigint') return wireTag('bigint', value.toString());
  if (value instanceof Uint8Array) {
    if (topLevel && binary) return value;
    const encoded = binary ? value : bytesToBase64(value);
    return wireTag('bytes', encoded);
  }
  if (value instanceof ArrayBuffer) return toWire(new Uint8Array(value), binary, topLevel, seen);
  if (value instanceof Date) {
    if (!Number.isFinite(value.getTime())) return invalidArgumentError('Cannot serialize an invalid Date.');
    return topLevel ? value.toISOString() : wireTag('datetime', value.toISOString());
  }
  if (typeof value !== 'object') {
    return invalidArgumentError(`Values of type ${typeof value} cannot be serialized by the default codecs.`);
  }
  if (seen.has(value)) return invalidArgumentError('Cyclic values cannot be serialized.');
  seen.add(value);
  try {
    if (Array.isArray(value)) {
      const result: unknown[] = [];
      for (const item of value) {
        const encoded = toWire(item, binary, false, seen);
        if (!isOk(encoded)) return encoded;
        result.push(encoded);
      }
      return result;
    }
    if (value instanceof Set) {
      const result: unknown[] = [];
      for (const item of value) {
        const encoded = toWire(item, binary, false, seen);
        if (!isOk(encoded)) return encoded;
        result.push(encoded);
      }
      return topLevel ? result : wireTag('set', result);
    }
    if (value instanceof Map) {
      const pairs: unknown[] = [];
      for (const [key, item] of value) {
        const encodedKey = toWire(key, binary, false, seen);
        if (!isOk(encodedKey)) return encodedKey;
        const encodedValue = toWire(item, binary, false, seen);
        if (!isOk(encodedValue)) return encodedValue;
        pairs.push([encodedKey, encodedValue]);
      }
      return wireTag('map', pairs);
    }
    if (Object.getPrototypeOf(value) !== Object.prototype && Object.getPrototypeOf(value) !== null) {
      return invalidArgumentError(`Objects of type ${value.constructor?.name ?? 'unknown'} cannot be serialized by the default codecs.`);
    }
    const result: Record<string, unknown> = {};
    for (const [key, item] of Object.entries(value)) {
      const encoded = toWire(item, binary, false, seen);
      if (!isOk(encoded)) return encoded;
      result[key] = encoded;
    }
    return result;
  } finally {
    seen.delete(value);
  }
}

function fromWire(value: unknown): StatusOr<unknown> {
  if (Array.isArray(value)) {
    const result: unknown[] = [];
    for (const item of value) {
      const decoded = fromWire(item);
      if (!isOk(decoded)) return decoded;
      result.push(decoded);
    }
    return result;
  }
  if (typeof value !== 'object' || value === null || value instanceof Uint8Array) return value;
  const object = value as Record<string, unknown>;
  const tag = object[WIRE_TAG];
  if (typeof tag !== 'string') {
    const result: Record<string, unknown> = {};
    for (const [key, item] of Object.entries(object)) {
      const decoded = fromWire(item);
      if (!isOk(decoded)) return decoded;
      result[key] = decoded;
    }
    return result;
  }
  const encoded = object[WIRE_VALUE];
  if (tag === 'float') {
    if (encoded === 'nan') return Number.NaN;
    if (encoded === '+inf') return Number.POSITIVE_INFINITY;
    if (encoded === '-inf') return Number.NEGATIVE_INFINITY;
    return invalidArgumentError('Invalid tagged float.');
  }
  if (tag === 'bigint' || tag === 'int') {
    if (typeof encoded !== 'string' || !/^-?\d+$/.test(encoded)) return invalidArgumentError('Invalid tagged integer.');
    try { return BigInt(encoded); } catch (error) { return invalidArgumentError('Invalid tagged integer.', [], error); }
  }
  if (tag === 'bytes' || tag === 'bytearray') {
    if (encoded instanceof Uint8Array) return new Uint8Array(encoded);
    if (typeof encoded !== 'string') return invalidArgumentError('Invalid tagged bytes.');
    return base64ToBytes(encoded);
  }
  if (tag === 'datetime') {
    if (typeof encoded !== 'string') return invalidArgumentError('Invalid tagged datetime.');
    const date = new Date(encoded);
    return Number.isFinite(date.getTime()) ? date : invalidArgumentError('Invalid tagged datetime.');
  }
  if (tag === 'set') {
    const decoded = fromWire(encoded);
    return isOk(decoded) && Array.isArray(decoded)
      ? new Set(decoded)
      : isOk(decoded) ? invalidArgumentError('Invalid tagged set.') : decoded;
  }
  if (tag === 'map' || tag === 'dict') {
    if (!Array.isArray(encoded)) return invalidArgumentError('Invalid tagged map.');
    const result = new Map<unknown, unknown>();
    for (const pair of encoded) {
      if (!Array.isArray(pair) || pair.length !== 2) return invalidArgumentError('Invalid tagged map entry.');
      const key = fromWire(pair[0]);
      if (!isOk(key)) return key;
      const item = fromWire(pair[1]);
      if (!isOk(item)) return item;
      result.set(key, item);
    }
    return result;
  }
  if (tag === 'tuple') {
    // JavaScript has no tuple runtime type; arrays are its native equivalent.
    return fromWire(encoded);
  }
  return invalidArgumentError(`Unsupported A11 serialization tag: ${tag}.`);
}

function bytesToBase64(bytes: Uint8Array): string {
  let binary = '';
  for (const byte of bytes) binary += String.fromCharCode(byte);
  if (typeof btoa === 'function') return btoa(binary);
  return Buffer.from(bytes).toString('base64');
}

function base64ToBytes(value: string): StatusOr<Uint8Array> {
  try {
    if (typeof atob === 'function') {
      const decoded = atob(value);
      return Uint8Array.from(decoded, (character) => character.charCodeAt(0));
    }
    return new Uint8Array(Buffer.from(value, 'base64'));
  } catch (error) {
    return invalidArgumentError('Invalid base64 byte data.', [], error);
  }
}

function jsonSerialize(value: unknown): StatusOr<Uint8Array> {
  const wire = toWire(value, false, true);
  if (!isOk(wire)) return wire;
  try {
    return utf8Encode(JSON.stringify(wire));
  } catch (error) {
    return invalidArgumentError('Failed to serialize JSON.', [], error);
  }
}

function jsonDeserialize(data: Uint8Array): StatusOr<unknown> {
  const text = utf8Decode(data);
  if (!isOk(text)) return text;
  try {
    return fromWire(JSON.parse(text) as unknown);
  } catch (error) {
    return invalidArgumentError('Invalid JSON data.', [], error);
  }
}

function msgpackSerialize(value: unknown): StatusOr<Uint8Array> {
  const wire = toWire(value, true, true);
  if (!isOk(wire)) return wire;
  try {
    return encode(wire);
  } catch (error) {
    return invalidArgumentError('Failed to serialize MessagePack.', [], error);
  }
}

function msgpackDeserialize(data: Uint8Array): StatusOr<unknown> {
  try {
    return fromWire(decode(data, { useBigInt64: true }));
  } catch (error) {
    return invalidArgumentError('Invalid MessagePack data.', [], error);
  }
}

function deserializeWireType(
  tag: string,
  data: Uint8Array,
  mimetype: string,
): StatusOr<unknown> {
  const decoded = mimetype === JSON_MIMETYPE
    ? jsonDeserialize(data)
    : msgpackDeserialize(data);
  if (!isOk(decoded)) return decoded;
  if (tag === 'datetime') {
    if (decoded instanceof Date) return decoded;
    if (typeof decoded !== 'string') {
      return invalidArgumentError('Serialized datetime must be a string.');
    }
    const date = new Date(decoded);
    return Number.isFinite(date.getTime())
      ? date
      : invalidArgumentError('Serialized datetime is invalid.');
  }
  if (tag === 'set') {
    return Array.isArray(decoded)
      ? new Set(decoded)
      : decoded instanceof Set
        ? decoded
        : invalidArgumentError('Serialized set must be an array.');
  }
  return decoded;
}

function deserializeBytes(
  data: Uint8Array,
  mimetype: string,
): StatusOr<Uint8Array> {
  const decoded = deserializeWireType('bytes', data, mimetype);
  if (!isOk(decoded)) return decoded;
  return decoded instanceof Uint8Array
    ? decoded
    : invalidArgumentError('Serialized bytes did not decode to byte data.');
}

/**
 * Ordered collection of codecs between application values and A11 chunks.
 *
 * AsyncNodes use a registry on every typed put/read. Register domain codecs
 * (model messages, audio frames, tool events) before generic defaults when
 * they need a distinct MIME/tag contract. MIME patterns and stable type tags
 * let consumers constrain what they will decode instead of trusting arbitrary
 * peer data.
 */
export class SerializationRegistry {
  private readonly codecs: RegisteredCodec[] = [];
  private nextOrder = 0;

  constructor(options: { registerDefaults?: boolean } = {}) {
    if (options.registerDefaults ?? false) this.installDefaults();
  }

  /** Add one codec; duplicate tag/media-type pairs are rejected. */
  register<T>(codec: SerializationCodec<T>): Status {
    try {
      if (typeof codec.tag !== 'string' || codec.tag === '') return invalidArgumentError('Serialization codec tag must be non-empty.');
      if (typeof codec.test !== 'function' || typeof codec.serialize !== 'function' || typeof codec.deserialize !== 'function') {
        return invalidArgumentError('Serialization codec callbacks must be functions.');
      }
      const parsed = parseMimetype(codec.mimetype);
      if (!isOk(parsed)) return parsed;
      if (this.codecs.some((registered) => registered.tag === codec.tag && registered.parsed.mediaType === parsed.mediaType)) {
        return alreadyExistsError(`A codec for ${codec.tag} and ${parsed.mediaType} is already registered.`);
      }
      this.codecs.push({ ...codec, parsed, order: this.nextOrder++ } as RegisteredCodec);
      return { code: 0, message: 'OK' };
    } catch (error) {
      return statusFromUnknown(error, 'Could not register serialization codec.');
    }
  }

  /** Install JSON, MessagePack, bytes, Blob, Date, Set, Map, and bigint codecs. */
  registerDefaults(): Status {
    return this.installDefaults();
  }

  private installDefaults(): Status {
    const jsonTags = ['null', 'boolean', 'integer', 'number', 'string', 'array', 'object'] as const;
    for (const mimetype of [JSON_MIMETYPE, MSGPACK_MIMETYPE]) {
      for (const tag of jsonTags) {
        const status = this.register({
          tag,
          mimetype,
          test: (value): value is unknown => canonicalJsonTag(value) === tag,
          serialize: (value) => mimetype === JSON_MIMETYPE ? jsonSerialize(value) : msgpackSerialize(value),
          deserialize: (data) => mimetype === JSON_MIMETYPE ? jsonDeserialize(data) : msgpackDeserialize(data),
        });
        if (!isOk(status)) return status;
      }
    }
    let status = this.register<Uint8Array>({
      tag: 'bytes',
      mimetype: OCTET_STREAM_MIMETYPE,
      test: (value): value is Uint8Array => value instanceof Uint8Array,
      serialize: (value) => value,
      deserialize: (data) => new Uint8Array(data),
    });
    if (!isOk(status)) return status;
    status = this.register<ArrayBuffer>({
      tag: 'arraybuffer',
      mimetype: OCTET_STREAM_MIMETYPE,
      test: (value): value is ArrayBuffer => value instanceof ArrayBuffer,
      serialize: (value) => new Uint8Array(value),
      deserialize: (data) => new Uint8Array(data).buffer,
    });
    if (!isOk(status)) return status;
    if (typeof Blob !== 'undefined') {
      status = this.register<Blob>({
        tag: 'blob',
        mimetype: 'application/octet-stream',
        test: (value): value is Blob => value instanceof Blob,
        serialize: (value) => value,
        deserialize: (data, chunk) => new Blob([new Uint8Array(data).buffer], { type: mediaTypeOf(chunk.mimetype) }),
      });
      if (!isOk(status)) return status;
    }
    for (const mimetype of [JSON_MIMETYPE, MSGPACK_MIMETYPE]) {
      status = this.register<Uint8Array>({
        tag: 'bytes',
        mimetype,
        test: (value): value is Uint8Array => value instanceof Uint8Array,
        serialize: (value) => mimetype === JSON_MIMETYPE
          ? jsonSerialize(value)
          : msgpackSerialize(value),
        deserialize: (data) => deserializeBytes(data, mimetype),
      });
      if (!isOk(status)) return status;
    }
    for (const mimetype of [JSON_MIMETYPE, MSGPACK_MIMETYPE]) {
      const wireTypes: Array<[string, (value: unknown) => boolean]> = [
        ['bigint', (value) => typeof value === 'bigint'],
        ['datetime', (value) => value instanceof Date],
        ['set', (value) => value instanceof Set],
        ['map', (value) => value instanceof Map],
      ];
      for (const [tag, test] of wireTypes) {
        status = this.register<unknown>({
          tag,
          mimetype,
          test: (value): value is unknown => test(value),
          serialize: (value) => mimetype === JSON_MIMETYPE ? jsonSerialize(value) : msgpackSerialize(value),
          deserialize: (data) => deserializeWireType(tag, data, mimetype),
        });
        if (!isOk(status)) return status;
      }
    }
    return { code: 0, message: 'OK' };
  }

  /** Select a matching serializer and produce a tagged, owned Chunk. */
  async toChunk(value: unknown, mimetype = ''): Promise<StatusOr<Chunk>> {
    try {
      const selection = mimetype === '' ? null : parseMimetype(mimetype, true);
      if (selection !== null && !isOk(selection)) return selection;
      const candidates = this.codecs
        .filter((codec) =>
          codec.test(value) &&
          (selection === null || registrationMatches(codec.parsed, selection, codec.tag)),
        )
        .sort((left, right) => left.order - right.order);
      if (candidates.length === 0) {
        return notFoundError(`No serializer is registered for the value${mimetype ? ` and ${mimetype}` : ''}.`);
      }
      const codec = candidates[0]!;
      let serializedResult: StatusOr<SerializedData>;
      try {
        serializedResult = await codec.serialize(value);
      } catch (error) {
        return statusFromUnknown(error, `Serializer for ${codec.tag} failed.`);
      }
      if (!isOk(serializedResult)) return serializedResult;
      const serialized = serializedResult;
      const exactMimetype = formatMimetype(codec.parsed, codec.tag);
      if (serialized instanceof Chunk) {
        return Chunk.create({
          metadata: new ChunkMetadata({
            mimetype: exactMimetype,
            timestamp: serialized.metadata?.timestamp ?? null,
            attributes: serialized.metadata?.attributes,
          }),
          ref: serialized.ref,
          data: serialized.data,
        });
      }
      const data = await toBytesAsync(serialized);
      if (!isOk(data)) return data;
      const blobType = typeof Blob !== 'undefined' && value instanceof Blob && value.type !== ''
        ? parseMimetype(value.type)
        : null;
      const outputMimetype = blobType !== null && isOk(blobType)
        ? formatMimetype(blobType, codec.tag)
        : exactMimetype;
      return Chunk.create({ metadata: new ChunkMetadata({ mimetype: outputMimetype }), data });
    } catch (error) {
      return statusFromUnknown(error, 'Could not serialize value.');
    }
  }

  /** Validate MIME/type constraints, select a decoder, and return a typed value. */
  async fromChunk<T = unknown>(
    chunk: Chunk,
    mimetypePatterns: string | readonly string[] = '',
    expectedTag?: string,
  ): Promise<StatusOr<T>> {
    try {
      const validation = chunk.validate();
      if (!isOk(validation)) return validation;
      if (chunk.metadata === null || chunk.mimetype === '') {
        return invalidArgumentError('The chunk has no mimetype.');
      }
      const actual = parseMimetype(chunk.mimetype);
      if (!isOk(actual)) return actual;
      const encodedTagRaw = actual.parameters.get('type');
      let encodedTag: string | undefined;
      try { encodedTag = encodedTagRaw === undefined ? undefined : decodeURIComponent(encodedTagRaw); }
      catch (error) { return invalidArgumentError('The chunk contains an invalid encoded type tag.', [], error); }
      const canonicalEncodedTag = encodedTag === undefined
        ? undefined
        : canonicalTypeTag(encodedTag);
      if (
        expectedTag !== undefined &&
        canonicalEncodedTag !== canonicalTypeTag(expectedTag)
      ) {
        return invalidArgumentError(`The chunk contains ${encodedTag ?? 'no type tag'}, not ${expectedTag}.`);
      }
      const requested = typeof mimetypePatterns === 'string'
        ? (mimetypePatterns === '' ? [] : [mimetypePatterns])
        : [...mimetypePatterns];
      let selected = requested.length === 0;
      for (const pattern of requested) {
        const parsed = parseMimetype(pattern, true);
        if (!isOk(parsed)) return parsed;
        if (mimetypeMatches(actual, parsed)) selected = true;
      }
      if (!selected) {
        return notFoundError(
          `The chunk mimetype ${chunk.mimetype} does not match the requested patterns.`,
        );
      }
      const candidates = this.codecs.filter((codec) =>
        (canonicalEncodedTag === undefined || codec.tag === canonicalEncodedTag) &&
        (
          // A Blob carries its concrete browser media type (for example,
          // image/png) on the chunk. Its stable `blob` tag selects the
          // binary decoder independently of that concrete media type.
          (canonicalEncodedTag === 'blob' && codec.tag === 'blob') ||
          registrationMatches(codec.parsed, actual, codec.tag)
        ),
      );
      if (candidates.length === 0) {
        return notFoundError(`No deserializer is registered for ${chunk.mimetype}.`);
      }
      const codec = candidates.sort((left, right) => left.order - right.order)[0]!;
      try {
        const value = await codec.deserialize(new Uint8Array(chunk.data), chunk);
        return value as StatusOr<T>;
      } catch (error) {
        return statusFromUnknown(error, `Deserializer for ${codec.tag} failed.`);
      }
    } catch (error) {
      return statusFromUnknown(error, 'Could not deserialize chunk.');
    }
  }
}

function mediaTypeOf(mimetype: string): string {
  const parsed = parseMimetype(mimetype);
  return isOk(parsed) ? parsed.mediaType : OCTET_STREAM_MIMETYPE;
}

let globalRegistry = new SerializationRegistry({ registerDefaults: true });

/** Return the process-wide registry used by nodes that receive no override. */
export function getGlobalSerializationRegistry(): SerializationRegistry {
  return globalRegistry;
}

/** Replace the process-wide default for subsequently created/used nodes. */
export function setGlobalSerializationRegistry(registry: SerializationRegistry): Status {
  if (!(registry instanceof SerializationRegistry)) return invalidArgumentError('registry must be a SerializationRegistry.');
  globalRegistry = registry;
  return { code: 0, message: 'OK' };
}

/** Serialize through the current process-wide registry. */
export async function toChunk(value: unknown, mimetype = ''): Promise<StatusOr<Chunk>> {
  return globalRegistry.toChunk(value, mimetype);
}

/** Deserialize through the current process-wide registry. */
export async function fromChunk<T = unknown>(
  chunk: Chunk,
  mimetypePatterns: string | readonly string[] = '',
  expectedTag?: string,
): Promise<StatusOr<T>> {
  return globalRegistry.fromChunk<T>(chunk, mimetypePatterns, expectedTag);
}
