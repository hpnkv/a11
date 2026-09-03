import { decode, encode } from '@msgpack/msgpack';

import { toBytesAsync, utf8Decode, utf8Encode, type AsyncByteSource } from './bytes.js';
import { Chunk, ChunkMetadata } from './data.js';
import {
  wireValueCodecCount,
  wireValueCodecFor,
  wireValueCodecs,
  type Fields,
} from './wire_values.js';
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
/** UTF-8 text codec media type. */
export const TEXT_MIMETYPE = 'text/plain';

/**
 * Media types that describe their content completely on their own.
 *
 * A chunk using one carries no `type` parameter and no framing inside the
 * payload: `text/plain` is UTF-8 text, `application/octet-stream` is bytes,
 * and `image/*` is encoded image data. There is nothing a `;type=` could add.
 * Text and octet-stream remain the defaults for strings and `Uint8Array`.
 *
 * The consequence to know: `ArrayBuffer` and `Blob` share the bytes media type,
 * so a value round-tripped without naming a type comes back as the canonical
 * `Uint8Array`. Name the tag to get one of the others, as with Python's
 * `bytearray`.
 */
const SELF_DESCRIBING_MEDIA_TYPES = new Set<string>([
  TEXT_MIMETYPE, OCTET_STREAM_MIMETYPE,
]);

function isSelfDescribingMediaType(mediaType: string): boolean {
  return SELF_DESCRIBING_MEDIA_TYPES.has(mediaType) || mediaType.startsWith('image/');
}

const MIME_TOKEN = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;

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

/**
 * Tags a JSON or MessagePack payload already spells out for itself.
 *
 * A chunk holding one of these carries no type parameter: writing `;type=object`
 * on an object says nothing a parser did not already know, and it stops a peer
 * that only has `application/json` from being understood.
 */
const GENERIC_TAGS = new Set([
  'object', 'array', 'string', 'integer', 'number', 'boolean', 'null',
]);

function formatMimetype(mimetype: ParsedMimetype, tag: string): string {
  const parameters = [...mimetype.parameters].filter(([name]) => name !== 'type');
  if (!GENERIC_TAGS.has(tag) && !isSelfDescribingMediaType(mimetype.mediaType)) {
    parameters.push(['type', encodeURIComponent(tag)]);
  }
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

/**
 * Whether a chunk's own mimetype is one the caller asked for.
 *
 * The `type` parameter takes no part: a selector chooses a *representation*,
 * and which type comes back is settled separately by the tag.
 */
function mimetypeMatches(actual: ParsedMimetype, selection: ParsedMimetype): boolean {
  if (!wildcardMatches(actual.mediaType, selection.mediaType)) return false;
  for (const [name, expected] of selection.parameters) {
    if (name === 'type') continue;
    const value = actual.parameters.get(name);
    if (value === undefined || !wildcardMatches(value, expected)) return false;
  }
  return true;
}

function registrationMatches(
  registered: ParsedMimetype,
  selection: ParsedMimetype,
): boolean {
  if (!wildcardMatches(registered.mediaType, selection.mediaType)
      && !wildcardMatches(selection.mediaType, registered.mediaType)) return false;
  for (const [name, expected] of selection.parameters) {
    if (name === 'type') continue;
    const value = registered.parameters.get(name);
    if (value !== undefined && !wildcardMatches(value, expected)
        && !wildcardMatches(expected, value)) return false;
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

/**
 * Encode `value` as a JSON- or MessagePack-ready tree.
 *
 * Encodes JavaScript values using standard wire representations: a
 * `Uint8Array` becomes base64 (or, in MessagePack, real bytes), a `Date`
 * becomes an ISO string, and a `Set` becomes an array. Recovering the original
 * type is
 * the reader's job, and it does that from the chunk's `;type=`.
 */
function toWire(
  value: unknown,
  binary: boolean,
  seen = new Set<object>(),
): StatusOr<unknown> {
  if (value === null || typeof value === 'boolean' || typeof value === 'string') return value;
  if (typeof value === 'number') return value;
  if (typeof value === 'bigint') {
    // JSON writes an exact integer literal; MessagePack has nowhere to put one
    // wider than 64 bits, and says so rather than quietly re-spelling it.
    if (!binary) return value;
    if (value < -(2n ** 63n) || value > 2n ** 64n - 1n) {
      return invalidArgumentError(
        'MessagePack cannot represent integers outside the 64-bit range; use JSON for arbitrary-precision integers.',
      );
    }
    return value;
  }
  if (value instanceof Uint8Array) return binary ? value : bytesToBase64(value);
  if (value instanceof ArrayBuffer) return toWire(new Uint8Array(value), binary, seen);
  if (value instanceof Date) {
    if (!Number.isFinite(value.getTime())) return invalidArgumentError('Cannot serialize an invalid Date.');
    return value.toISOString();
  }
  if (typeof value !== 'object') {
    return invalidArgumentError(`Values of type ${typeof value} cannot be serialized by the default codecs.`);
  }
  if (seen.has(value)) return invalidArgumentError('Cyclic values cannot be serialized.');
  seen.add(value);
  try {
    if (Array.isArray(value) || value instanceof Set) {
      const result: unknown[] = [];
      for (const item of value) {
        const encoded = toWire(item, binary, seen);
        if (!isOk(encoded)) return encoded;
        result.push(encoded);
      }
      return result;
    }
    if (value instanceof Map) {
      const result: Record<string, unknown> = {};
      for (const [key, item] of value) {
        const encoded = toWire(item, binary, seen);
        if (!isOk(encoded)) return encoded;
        result[String(key)] = encoded;
      }
      return result;
    }
    // A registered class — a Chunk, a Status, an SDK model — is written as its
    // own fields. The chunk's `;type=` says which class produced them.
    const wireValue = wireValueCodecFor(value);
    if (wireValue !== null) {
      const dumped = wireValue.dump(value);
      if (!isOk(dumped)) return dumped;
      return toWire(dumped, binary, seen);
    }
    if (Object.getPrototypeOf(value) !== Object.prototype && Object.getPrototypeOf(value) !== null) {
      return invalidArgumentError(`Objects of type ${value.constructor?.name ?? 'unknown'} cannot be serialized by the default codecs.`);
    }
    const result: Record<string, unknown> = {};
    for (const [key, item] of Object.entries(value)) {
      const encoded = toWire(item, binary, seen);
      if (!isOk(encoded)) return encoded;
      result[key] = encoded;
    }
    return result;
  } finally {
    seen.delete(value);
  }
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
  const wire = toWire(value, false);
  if (!isOk(wire)) return wire;
  try {
    return utf8Encode(JSON.stringify(wire, (_key, item: unknown) =>
      typeof item === 'bigint' ? Number(item) : item));
  } catch (error) {
    return invalidArgumentError('Failed to serialize JSON.', [], error);
  }
}

function jsonDeserialize(data: Uint8Array): StatusOr<unknown> {
  const text = utf8Decode(data);
  if (!isOk(text)) return text;
  try {
    return JSON.parse(text) as unknown;
  } catch (error) {
    return invalidArgumentError('Invalid JSON data.', [], error);
  }
}

function msgpackSerialize(value: unknown): StatusOr<Uint8Array> {
  const wire = toWire(value, true);
  if (!isOk(wire)) return wire;
  try {
    return encode(wire);
  } catch (error) {
    return invalidArgumentError('Failed to serialize MessagePack.', [], error);
  }
}

function msgpackDeserialize(data: Uint8Array): StatusOr<unknown> {
  try {
    return decode(data, { useBigInt64: true });
  } catch (error) {
    return invalidArgumentError('Invalid MessagePack data.', [], error);
  }
}

function decodePayload(data: Uint8Array, mimetype: string): StatusOr<unknown> {
  return mimetype === JSON_MIMETYPE
    ? jsonDeserialize(data)
    : msgpackDeserialize(data);
}

/**
 * Rebuild the value a `;type=` names from the plain tree the format carried.
 *
 * These are the types JSON and MessagePack have no shape for, so the chunk's
 * tag is the only thing that says what they were.
 */
function deserializeWireType(
  tag: string,
  data: Uint8Array,
  mimetype: string,
): StatusOr<unknown> {
  const decoded = decodePayload(data, mimetype);
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
    if (decoded instanceof Set) return decoded;
    return Array.isArray(decoded)
      ? new Set(decoded)
      : invalidArgumentError('Serialized set must be an array.');
  }
  if (tag === 'map') {
    if (decoded instanceof Map) return decoded;
    if (typeof decoded !== 'object' || decoded === null || Array.isArray(decoded)) {
      return invalidArgumentError('Serialized map must be an object.');
    }
    return new Map(Object.entries(decoded));
  }
  if (tag === 'bigint') {
    if (typeof decoded === 'bigint') return decoded;
    if (typeof decoded === 'number' && Number.isInteger(decoded)) return BigInt(decoded);
    return invalidArgumentError('Serialized bigint must be an integer.');
  }
  return decoded;
}

function deserializeBytes(
  data: Uint8Array,
  mimetype: string,
): StatusOr<Uint8Array> {
  const decoded = decodePayload(data, mimetype);
  if (!isOk(decoded)) return decoded;
  if (decoded instanceof Uint8Array) return decoded;
  // The chunk's `;type=bytes` already said what this is, so JSON carries it as
  // a plain base64 string.
  if (typeof decoded === 'string') return base64ToBytes(decoded);
  return invalidArgumentError('Serialized bytes did not decode to byte data.');
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
  private wireValueCache: RegisteredCodec[] | null = null;
  private wireValueGeneration = -1;

  constructor(options: { registerDefaults?: boolean } = {}) {
    if (options.registerDefaults ?? false) this.installDefaults();
  }

  /** Add one codec; duplicate tag/media-type pairs are rejected. */
  register<T>(codec: SerializationCodec<T>): Status {
    return this.addCodec(codec, false);
  }

  private addCodec<T>(codec: SerializationCodec<T>, patterns: boolean): Status {
    try {
      if (typeof codec.tag !== 'string' || codec.tag === '') return invalidArgumentError('Serialization codec tag must be non-empty.');
      if (typeof codec.test !== 'function' || typeof codec.serialize !== 'function' || typeof codec.deserialize !== 'function') {
        return invalidArgumentError('Serialization codec callbacks must be functions.');
      }
      const parsed = parseMimetype(codec.mimetype, patterns);
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
    // Text first: registration order picks the representation when a caller
    // names no mimetype, and a string travelling as itself beats a JSON-quoted
    // copy of itself. The JSON and MessagePack codecs below stay registered, so
    // asking for them still works and a peer that sends them is still read.
    const textStatus = this.register<string>({
      tag: 'string',
      mimetype: TEXT_MIMETYPE,
      test: (value): value is string => typeof value === 'string',
      // utf8Decode is the strict one: it returns InvalidArgument rather
      // than substituting U+FFFD, so a peer's encoding bug is reported
      // where it arrives instead of becoming silent corruption.
      serialize: (value) => utf8Encode(value),
      deserialize: (data) => utf8Decode(data),
    });
    if (!isOk(textStatus)) return textStatus;
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
      const blobCodec = (mimetype: string): SerializationCodec<Blob> => ({
        tag: 'blob',
        mimetype,
        test: (value): value is Blob => value instanceof Blob,
        serialize: (value) => value,
        deserialize: (data, chunk) => new Blob([new Uint8Array(data).buffer], { type: mediaTypeOf(chunk.mimetype) }),
      });
      status = this.register<Blob>(blobCodec(OCTET_STREAM_MIMETYPE));
      if (!isOk(status)) return status;
      for (const mimetype of ['image/png', 'image/jpeg']) {
        status = this.register<Blob>(blobCodec(mimetype));
        if (!isOk(status)) return status;
      }
      status = this.addCodec<Blob>(blobCodec('image/*'), true);
      if (!isOk(status)) return status;
    }
    const imageBytesCodec = (mimetype: string): SerializationCodec<Uint8Array> => ({
      tag: 'bytes',
      mimetype,
      test: (value): value is Uint8Array => value instanceof Uint8Array,
      serialize: (value) => value,
      deserialize: (data) => new Uint8Array(data),
    });
    for (const mimetype of ['image/png', 'image/jpeg']) {
      status = this.register<Uint8Array>(imageBytesCodec(mimetype));
      if (!isOk(status)) return status;
    }
    status = this.addCodec<Uint8Array>(imageBytesCodec('image/*'), true);
    if (!isOk(status)) return status;
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

  /**
   * Codecs for the class-tagged types, derived from the wire-value registry.
   *
   * They are derived rather than registered because that registry grows on
   * import: an SDK module adds `a11.sdk.Interaction` whenever the application
   * first pulls it in, which is routinely after this registry was built. They
   * sort ahead of everything else, since a value that knows its own class must
   * not be claimed by the generic `object` codec that also matches it.
   */
  private wireValueCodecs(): RegisteredCodec[] {
    if (this.wireValueCache !== null && this.wireValueGeneration === wireValueCodecCount()) {
      return this.wireValueCache;
    }
    const derived: RegisteredCodec[] = [];
    for (const wireValue of wireValueCodecs()) {
      for (const mimetype of [JSON_MIMETYPE, MSGPACK_MIMETYPE]) {
        const parsed = parseMimetype(mimetype);
        if (!isOk(parsed)) continue;
        const json = mimetype === JSON_MIMETYPE;
        derived.push({
          tag: wireValue.tag,
          mimetype,
          parsed,
          order: -1,
          test: (value): value is unknown => wireValue.test(value),
          serialize: (value) => (json ? jsonSerialize(value) : msgpackSerialize(value)),
          deserialize: (data) => {
            const decoded = json ? jsonDeserialize(data) : msgpackDeserialize(data);
            if (!isOk(decoded)) return decoded;
            if (typeof decoded !== 'object' || decoded === null || Array.isArray(decoded)) {
              return invalidArgumentError(`A ${wireValue.tag} payload must be an object.`);
            }
            return wireValue.load(decoded as Fields);
          },
        } as RegisteredCodec);
      }
    }
    this.wireValueCache = derived;
    this.wireValueGeneration = wireValueCodecCount();
    return derived;
  }

  /** Select a matching serializer and produce a tagged, owned Chunk. */
  async toChunk(value: unknown, mimetype = ''): Promise<StatusOr<Chunk>> {
    try {
      // A promise is the one value worth naming: the SDK's builders are async
      // (`makeTextMessageInteraction`, `toChunk`), and a forgotten `await`
      // otherwise arrives here as an anonymous object no codec claims, which
      // reads as "nothing can serialize an interaction" rather than "you sent
      // the promise, not the value".
      if (typeof (value as { then?: unknown } | null)?.then === 'function') {
        return invalidArgumentError(
          'A Promise cannot be serialized: await the value before writing it.',
        );
      }
      const selection = mimetype === '' ? null : parseMimetype(mimetype, true);
      if (selection !== null && !isOk(selection)) return selection;
      // A selector picks a representation; the value's own type decides the
      // tag, so a type parameter in it is ignored.
      const candidates = [...this.wireValueCodecs(), ...this.codecs]
        .filter((codec) =>
          codec.test(value) &&
          (selection === null || registrationMatches(codec.parsed, selection)),
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
      const outputType = selection !== null && codec.parsed.mediaType.includes('*')
        && !selection.mediaType.includes('*') ? selection : codec.parsed;
      const exactMimetype = formatMimetype(outputType, codec.tag);
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

  /**
   * Select a decoder from the chunk's metadata and return a typed value.
   *
   * `mimetypePatterns` constrains the *representation*. Which type comes back
   * is the chunk's `;type=`, or `expectedTag` when the caller names one. A
   * chunk with no type parameter is not underspecified — it holds exactly what
   * its format describes, and decodes to that.
   */
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
      // An untagged chunk contradicts nothing: it holds what its format
      // describes, and `expectedTag` is then a request to read it as that type.
      if (expectedTag !== undefined && encodedTag !== undefined && encodedTag !== expectedTag) {
        return invalidArgumentError(`The chunk contains ${encodedTag}, not ${expectedTag}.`);
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
      const wanted = encodedTag ?? expectedTag;
      const byMediaType = [...this.wireValueCodecs(), ...this.codecs].filter((codec) =>
        // A Blob carries its concrete browser media type (for example,
        // image/png) on the chunk. Its stable `blob` tag selects the binary
        // decoder independently of that concrete media type.
        (wanted === 'blob' && codec.tag === 'blob') ||
        registrationMatches(codec.parsed, actual),
      );
      const generic = byMediaType.filter((codec) => GENERIC_TAGS.has(codec.tag));
      let candidates = wanted === undefined
        ? generic
        : byMediaType.filter((codec) => codec.tag === wanted);
      if (candidates.length === 0 && expectedTag === undefined) {
        // Either nothing named a type, or the tag names one this peer never
        // loaded. The bytes are still valid JSON or MessagePack, so hand back
        // what the format describes rather than refusing to look at it.
        candidates = generic.length > 0 ? generic : byMediaType;
      }
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
