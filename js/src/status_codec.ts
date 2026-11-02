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
import {statusCodecBridge} from './internal/wasm/native_status_codec.js';
import {statusFromBridgeFailure} from './internal/wasm/status_bridge.js';
import {
  invalidArgumentError,
  failedPreconditionError,
  isOk,
  isStatus,
  StatusCode,
  statusToJson,
  type Status,
  type StatusOr,
} from './status.js';

/** Box used where a decoded non-OK Status is a successful return value. */
export interface DecodedStatus {
  /** Decoded status, including non-OK values carried as data. */
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
  const native = statusCodecBridge();
  if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before using A11 protocol codecs.');
  try { return native.pack(statusToJson(status)); }
  catch (error) { return statusFromBridgeFailure(error, 'Status encode failed.'); }
}

/** Decode a Status while preserving an inner non-OK value as data. */
export function decodeStatus(source: ByteSource): StatusOr<DecodedStatus> {
  const bytes = toBytes(source);
  if (!isOk(bytes)) return bytes;
  const native = statusCodecBridge();
  if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before using A11 protocol codecs.');
  try {
      const value = native.unpack(bytes);
      return isStatus(value) ? {status: value} :
        invalidArgumentError('Native status decoder returned invalid data.');
    } catch (error) {
      return statusFromBridgeFailure(error, 'Status decode failed.');
    }
}

/** Convenience decoder; malformed bytes are themselves returned as a Status. */
export function unpackStatus(source: ByteSource): Status {
  const decoded = decodeStatus(source);
  return isOk(decoded) ? decoded.status : decoded;
}
