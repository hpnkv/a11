import { type ByteSource, toBytes } from './bytes.js';
import { decodeMsgpackFields, encodeMsgpackFields } from './msgpack_codec.js';
import {
  invalidArgumentError,
  isOk,
  StatusCode,
  type Status,
  type StatusOr,
} from './status.js';

/** Box used where a decoded non-OK Status is a successful return value. */
export interface DecodedStatus {
  status: Status;
}

/** Encode the native A11 concatenated MessagePack Status layout. */
export function packStatus(status: Status): StatusOr<Uint8Array> {
  if (
    typeof status !== 'object' ||
    status === null ||
    !Number.isInteger(status.code) ||
    status.code < StatusCode.OK ||
    status.code > StatusCode.UNAUTHENTICATED ||
    typeof status.message !== 'string'
  ) {
    return invalidArgumentError('Value is not a canonical Status.');
  }
  const details = status.details ?? [];
  if (
    !Array.isArray(details) ||
    details.some((detail) => typeof detail !== 'object' || detail === null)
  ) {
    return invalidArgumentError('Every Status detail must be an object.');
  }
  return encodeMsgpackFields([status.code, status.message, details]);
}

/** Decode a Status while preserving an inner non-OK value as data. */
export function decodeStatus(source: ByteSource): StatusOr<DecodedStatus> {
  const bytes = toBytes(source);
  if (!isOk(bytes)) return bytes;
  const fields = decodeMsgpackFields(bytes, 3, 'Status');
  if (!isOk(fields)) return fields;
  const [code, message, rawDetails] = fields;
  const numericCode = typeof code === 'bigint' ? Number(code) : code;
  if (
    !Number.isInteger(numericCode) ||
    (numericCode as number) < StatusCode.OK ||
    (numericCode as number) > StatusCode.UNAUTHENTICATED ||
    typeof message !== 'string' ||
    (rawDetails !== null && !Array.isArray(rawDetails))
  ) {
    return invalidArgumentError(
      'MessagePack does not contain a valid Status.',
    );
  }
  const details = rawDetails ?? [];
  if (
    (details as unknown[]).some(
      (detail) =>
        typeof detail !== 'object' || detail === null || Array.isArray(detail),
    )
  ) {
    return invalidArgumentError('Every Status detail must be an object.');
  }
  return {
    status: {
      code: numericCode as StatusCode,
      message,
      details: details as object[],
    } as Status,
  };
}

/** Convenience decoder; malformed bytes are themselves returned as a Status. */
export function unpackStatus(source: ByteSource): Status {
  const decoded = decodeStatus(source);
  return isOk(decoded) ? decoded.status : decoded;
}
