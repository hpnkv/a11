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

import {toBytes, type ByteSource} from './bytes.js';
import {framingBridge, type ReassemblerBridge} from './internal/wasm/native_framing.js';
import {statusFromBridgeFailure} from './internal/wasm/status_bridge.js';
import {
  failedPreconditionError,
  invalidArgumentError,
  isOk,
  okStatus,
  outOfRangeError,
  type Status,
  type StatusOr,
} from './status.js';

const FIRST_CHUNK_METADATA_SIZE = 17;
/** Smallest packet that can carry first-chunk metadata and one payload byte. */
export const MINIMUM_BYTE_PACKET_SIZE = FIRST_CHUNK_METADATA_SIZE + 1;
const UINT64_MAX = 0xffff_ffff_ffff_ffffn;

/** Packet shapes in the Action Engine-compatible byte framing format. */
export enum BytePacketType {
  /** One packet contains the complete logical message. */
  COMPLETE_BYTES = 0x00,
  /** One numbered packet of a known packet count. */
  BYTE_CHUNK = 0x01,
  /** One numbered packet whose final bytes carry the message length. */
  LENGTH_SUFFIXED_BYTE_CHUNK = 0x02,
}

/** Parsed packet metadata plus an owned payload slice. */
export interface BytePacket {
  /** Framing layout used by this packet. */
  type: BytePacketType;
  /** Owned application payload slice. */
  payload: Uint8Array;
  /** Temporary logical-message id. */
  transientId: bigint;
  /** Zero-based packet position. */
  sequence: number;
  /** Total packets, when carried by this layout. */
  packetCount: number;
}

/** Packet and incomplete-message memory bounds for a binary channel. */
export interface ByteChunkingOptions {
  /** Maximum encoded size of each transport packet. */
  packetSize?: number;
  /** Maximum reassembled logical message size. */
  maxMessageSize?: number;
  /** Simultaneous incomplete message ids retained. */
  maxPendingMessages?: number;
  /** Aggregate bytes retained by incomplete messages. */
  maxPendingBytes?: number;
}

/** Validated, default-filled form of {@link ByteChunkingOptions}. */
export interface NormalizedByteChunkingOptions {
  /** Maximum encoded size of each transport packet. */
  packetSize: number;
  /** Maximum reassembled logical message size. */
  maxMessageSize: number;
  /** Simultaneous incomplete message ids retained. */
  maxPendingMessages: number;
  /** Aggregate bytes retained by incomplete messages. */
  maxPendingBytes: number;
}

/** Validate packet/memory relationships and apply channel defaults. */
export function normalizeByteChunkingOptions(
  options: ByteChunkingOptions = {},
): StatusOr<NormalizedByteChunkingOptions> {
  try {
    if (typeof options !== 'object' || options === null) {
      return invalidArgumentError('Byte chunking options must be an object.');
    }
    const result: NormalizedByteChunkingOptions = {
      packetSize: options.packetSize ?? 64 * 1024,
      maxMessageSize: options.maxMessageSize ?? 32 * 1024 * 1024,
      maxPendingMessages: options.maxPendingMessages ?? 64,
      maxPendingBytes: options.maxPendingBytes ?? 64 * 1024 * 1024,
    };
    for (const [name, value] of Object.entries(result)) {
      if (!Number.isSafeInteger(value) || value < 0) {
        return invalidArgumentError(`${name} must be a nonnegative safe integer.`);
      }
    }
    const native = framingBridge();
    if (native === null) return failedPreconditionError(
      'Initialize A11 WASM before validating byte chunking options.');
    try { native.validateOptions(result); }
    catch (error) { return statusFromBridgeFailure(error,
      'Byte chunking options are invalid.'); }
    return result;
  } catch (error) {
    return invalidArgumentError('Byte chunking options could not be read.', [], error);
  }
}

function normalizeTransientId(value: bigint | number): StatusOr<bigint> {
  if (typeof value === 'bigint') {
    return value >= 0n && value <= UINT64_MAX
      ? value
      : outOfRangeError('transientId must be an unsigned 64-bit integer.');
  }
  if (!Number.isSafeInteger(value) || value < 0) {
    return invalidArgumentError(
      'transientId must be a non-negative safe integer or bigint.',
    );
  }
  return BigInt(value);
}

/** Split bytes using Action Engine's fixed little-endian packet suffixes. */
export function splitBytesIntoPackets(
  source: ByteSource,
  transientId: bigint | number,
  packetSize = 64 * 1024,
): StatusOr<Uint8Array[]> {
  const bytes = toBytes(source);
  if (!isOk(bytes)) return bytes;
  const id = normalizeTransientId(transientId);
  if (!isOk(id)) return id;
  if (!Number.isSafeInteger(packetSize) || packetSize < 0) {
    return invalidArgumentError(
      'packetSize must be a nonnegative safe integer.',
    );
  }
  try {
    const native = framingBridge();
    if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before using A11 protocol codecs.');
    return native.split(bytes, id, packetSize);
  } catch (error) {
    return statusFromBridgeFailure(error, 'Failed to split byte message.');
  }
}

/** Parse one Action Engine byte packet without retaining its input buffer. */
export function parseBytePacket(source: ByteSource): StatusOr<BytePacket> {
  const packet = toBytes(source);
  if (!isOk(packet)) return packet;
  const native = framingBridge();
  if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before using A11 protocol codecs.');
  try { return native.parse(packet) as BytePacket; }
  catch (error) { return statusFromBridgeFailure(error, 'Byte packet parse failed.'); }
}

/** Bounded native byte-message reassembly. */
export class ByteReassembler {
  readonly options: Readonly<NormalizedByteChunkingOptions>;
  private readonly native: ReassemblerBridge;

  private constructor(options: NormalizedByteChunkingOptions,
    native: ReassemblerBridge) {
    this.options = Object.freeze({...options});
    this.native = native;
  }

  static create(options: ByteChunkingOptions = {}): StatusOr<ByteReassembler> {
    const normalized = normalizeByteChunkingOptions(options);
    if (!isOk(normalized)) return normalized;
    const bridge = framingBridge();
    if (bridge === null) return failedPreconditionError(
      'Initialize A11 WASM before creating a byte reassembler.');
    try { return new ByteReassembler(normalized, bridge.reassembler(normalized)); }
  catch (error) { return statusFromBridgeFailure(error,
      'Native byte reassembler creation failed.'); }
  }

  get pendingMessageCount(): number { return this.native.pendingMessageCount; }
  get pendingByteCount(): number { return this.native.pendingByteCount; }

  clear(): Status {
    try { this.native.clear(); return okStatus(); }
  catch (error) { return statusFromBridgeFailure(error,
      'Byte reassembly clear failed.'); }
  }

  dispose(): void { this.native.dispose(); }

  feed(source: ByteSource): StatusOr<Uint8Array | null> {
    const serialized = toBytes(source);
    if (!isOk(serialized)) return serialized;
    try { return this.native.feed(serialized); }
  catch (error) { return statusFromBridgeFailure(error,
      'Byte reassembly failed.'); }
  }
}
