import {
  invalidArgumentError,
  statusFromUnknown,
  type StatusOr,
} from './status.js';

/** Synchronous byte-like values accepted at A11's JavaScript boundary. */
export type ByteSource =
  | Uint8Array
  | ArrayBuffer
  | ArrayBufferView
  | string;

/** Byte source that may require asynchronously reading a browser Blob. */
export type AsyncByteSource = ByteSource | Blob;

/** Owned binary metadata map used for headers and chunk attributes. */
export type ByteMap = Map<string, Uint8Array>;
/** Map/object input whose values are normalized into owned bytes. */
export type ByteMapInput =
  | ReadonlyMap<string, ByteSource>
  | Readonly<Record<string, ByteSource>>;

const encoder = new TextEncoder();
const decoder = new TextDecoder('utf-8', { fatal: true });

/** Copy a supported byte source; strings are encoded as UTF-8. */
export function toBytes(value: ByteSource): StatusOr<Uint8Array> {
  try {
    if (typeof value === 'string') return encoder.encode(value);
    if (value instanceof Uint8Array) return new Uint8Array(value);
    if (value instanceof ArrayBuffer) return new Uint8Array(value.slice(0));
    if (ArrayBuffer.isView(value)) {
      return new Uint8Array(
        value.buffer.slice(
          value.byteOffset,
          value.byteOffset + value.byteLength,
        ),
      );
    }
    return invalidArgumentError('Expected bytes, an ArrayBuffer, or a string.');
  } catch (error) {
    return statusFromUnknown(error, 'Could not copy byte data.');
  }
}

/** Copy a byte source, awaiting Blob contents when necessary. */
export async function toBytesAsync(
  value: AsyncByteSource,
): Promise<StatusOr<Uint8Array>> {
  if (typeof Blob !== 'undefined' && value instanceof Blob) {
    try {
      return new Uint8Array(await value.arrayBuffer());
    } catch (error) {
      return statusFromUnknown(error, 'Could not read Blob data.');
    }
  }
  return toBytes(value as ByteSource);
}

/** Encode application/header text as UTF-8 bytes. */
export function utf8Encode(value: string): Uint8Array {
  return encoder.encode(value);
}

/** Decode strict UTF-8, returning InvalidArgument instead of replacement text. */
export function utf8Decode(value: Uint8Array): StatusOr<string> {
  try {
    return decoder.decode(value);
  } catch (error) {
    return invalidArgumentError('Byte data is not valid UTF-8.', [], error);
  }
}

/** Join packet or serialized-record pieces into one owned byte array. */
export function concatBytes(parts: readonly Uint8Array[]): Uint8Array {
  let length = 0;
  for (const part of parts) length += part.byteLength;
  const result = new Uint8Array(length);
  let offset = 0;
  for (const part of parts) {
    result.set(part, offset);
    offset += part.byteLength;
  }
  return result;
}

const base64Alphabet =
  'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

/** Encode binary A11 values for JSON-only metadata channels. */
export function base64Encode(bytes: Uint8Array): string {
  let result = '';
  for (let index = 0; index < bytes.length; index += 3) {
    const first = bytes[index] ?? 0;
    const second = bytes[index + 1] ?? 0;
    const third = bytes[index + 2] ?? 0;
    const value = (first << 16) | (second << 8) | third;
    result += base64Alphabet[(value >>> 18) & 0x3f];
    result += base64Alphabet[(value >>> 12) & 0x3f];
    result += index + 1 < bytes.length
      ? base64Alphabet[(value >>> 6) & 0x3f]
      : '=';
    result += index + 2 < bytes.length ? base64Alphabet[value & 0x3f] : '=';
  }
  return result;
}

/** Decode canonical base64 and reject malformed input. */
export function base64Decode(value: string): StatusOr<Uint8Array> {
  if (
    typeof value !== 'string' ||
    value.length % 4 !== 0 ||
    !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(
      value,
    )
  ) {
    return invalidArgumentError('Value is not valid base64.');
  }
  const padding = value.endsWith('==') ? 2 : value.endsWith('=') ? 1 : 0;
  const output = new Uint8Array((value.length / 4) * 3 - padding);
  let outputIndex = 0;
  for (let index = 0; index < value.length; index += 4) {
    const a = base64Alphabet.indexOf(value[index] ?? 'A');
    const b = base64Alphabet.indexOf(value[index + 1] ?? 'A');
    const c = value[index + 2] === '=' ? 0 : base64Alphabet.indexOf(value[index + 2] ?? 'A');
    const d = value[index + 3] === '=' ? 0 : base64Alphabet.indexOf(value[index + 3] ?? 'A');
    const bits = (a << 18) | (b << 12) | (c << 6) | d;
    if (outputIndex < output.length) output[outputIndex++] = bits >>> 16;
    if (outputIndex < output.length) output[outputIndex++] = bits >>> 8;
    if (outputIndex < output.length) output[outputIndex++] = bits;
  }
  return output;
}

/** Convert a map/object boundary into an owned byte map with optional key checks. */
export function normalizeByteMap(
  values: ByteMapInput | undefined,
  validateKey?: (key: string) => boolean,
): StatusOr<ByteMap> {
  const result: ByteMap = new Map();
  if (values === undefined) return result;
  try {
    const entries = values instanceof Map
      ? values.entries()
      : Object.entries(values);
    for (const [key, source] of entries) {
      if (validateKey !== undefined && !validateKey(key)) {
        return invalidArgumentError(`Invalid byte-map key: ${key}.`);
      }
      const bytes = toBytes(source);
      if (typeof bytes === 'object' && 'code' in bytes && 'message' in bytes) {
        return bytes;
      }
      result.set(key, bytes as Uint8Array);
    }
  } catch (error) {
    return statusFromUnknown(error, 'Could not normalize byte map.');
  }
  return result;
}

/** Deep-copy a byte map so caller mutation cannot change retained headers. */
export function copyByteMap(values: ReadonlyMap<string, Uint8Array>): ByteMap {
  return new Map(
    [...values].map(([key, value]) => [key, new Uint8Array(value)]),
  );
}

/** Generate a validated-name-friendly id for actions, nodes, sessions, or streams. */
export function randomId(prefix: string): string {
  try {
    const value = globalThis.crypto?.randomUUID?.();
    if (value !== undefined) return `${prefix}${value.replaceAll('-', '')}`;
  } catch {
    // The deterministic fallback below still produces a valid A11 name.
  }
  const random = Math.floor(Math.random() * Number.MAX_SAFE_INTEGER).toString(16);
  return `${prefix}${Date.now().toString(16)}${random}`;
}
