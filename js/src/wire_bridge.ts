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

import {WireMessage, wireMessageFromJsonValue,
  wireMessageToJsonValue} from './data.js';
import {isOk, type StatusOr} from './status.js';
import {statusFromBridgeFailure} from './status_bridge.js';

/** Native wire codec with TypeScript WireMessage values at its public edge. */
export interface WireCodecBridge {
  encode(value: Record<string, unknown>): Uint8Array;
  decode(bytes: Uint8Array): Record<string, unknown>;
}

export function encodeWireBridge(codec: WireCodecBridge,
    message: WireMessage): StatusOr<Uint8Array> {
  const value = wireMessageToJsonValue(message);
  if (!isOk(value)) return value;
  try { return codec.encode(value); }
  catch (error) { return statusFromBridgeFailure(error, 'WASM wire encode failed.'); }
}

export function decodeWireBridge(codec: WireCodecBridge,
    bytes: Uint8Array): StatusOr<WireMessage> {
  try { return wireMessageFromJsonValue(codec.decode(bytes)); }
  catch (error) { return statusFromBridgeFailure(error, 'WASM wire decode failed.'); }
}
