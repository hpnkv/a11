import { decodeMulti, encode } from '@msgpack/msgpack';

import { concatBytes } from './bytes.js';
import {
  invalidArgumentError,
  statusFromUnknown,
  type StatusOr,
} from './status.js';

/** Encode A11's concatenated MessagePack field representation. */
export function encodeMsgpackFields(
  values: readonly unknown[],
): StatusOr<Uint8Array> {
  try {
    return concatBytes(values.map((value) => encode(value)));
  } catch (error) {
    return statusFromUnknown(
      error,
      'Failed to encode MessagePack.',
      3,
    );
  }
}

/** Decode exactly `count` concatenated MessagePack values. */
export function decodeMsgpackFields(
  bytes: Uint8Array,
  count: number,
  context: string,
): StatusOr<unknown[]> {
  try {
    const values = [...decodeMulti(bytes, { useBigInt64: true })];
    if (values.length !== count) {
      return invalidArgumentError(
        `${context} contains ${values.length} MessagePack fields; expected ${count}.`,
      );
    }
    return values;
  } catch (error) {
    return invalidArgumentError(
      `Failed to decode ${context} MessagePack.`,
      [],
      error,
    );
  }
}

/** Convert byte-map entries to the plain object shape accepted by MessagePack. */
export function msgpackByteMap(
  values: ReadonlyMap<string, Uint8Array>,
): Record<string, Uint8Array> {
  const result: Record<string, Uint8Array> = {};
  for (const [key, value] of values) result[key] = value;
  return result;
}
