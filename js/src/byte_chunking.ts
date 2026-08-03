import { concatBytes, toBytes, type ByteSource } from './bytes.js';
import {
  alreadyExistsError,
  invalidArgumentError,
  isOk,
  okStatus,
  outOfRangeError,
  resourceExhaustedError,
  statusFromUnknown,
  type Status,
  type StatusOr,
} from './status.js';

const COMPLETE_METADATA_SIZE = 9;
const CHUNK_METADATA_SIZE = 13;
const FIRST_CHUNK_METADATA_SIZE = 17;
export const MINIMUM_BYTE_PACKET_SIZE = FIRST_CHUNK_METADATA_SIZE + 1;
const UINT32_MAX = 0xffff_ffff;
const UINT64_MAX = 0xffff_ffff_ffff_ffffn;

export enum BytePacketType {
  COMPLETE_BYTES = 0x00,
  BYTE_CHUNK = 0x01,
  LENGTH_SUFFIXED_BYTE_CHUNK = 0x02,
}

export interface BytePacket {
  type: BytePacketType;
  payload: Uint8Array;
  transientId: bigint;
  sequence: number;
  packetCount: number;
}

export interface ByteChunkingOptions {
  packetSize?: number;
  maxMessageSize?: number;
  maxPendingMessages?: number;
  maxPendingBytes?: number;
}

export interface NormalizedByteChunkingOptions {
  packetSize: number;
  maxMessageSize: number;
  maxPendingMessages: number;
  maxPendingBytes: number;
}

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
      if (!Number.isSafeInteger(value) || value <= 0) {
        return invalidArgumentError(`${name} must be a positive safe integer.`);
      }
    }
    if (result.packetSize < MINIMUM_BYTE_PACKET_SIZE) {
      return invalidArgumentError(
        `packetSize must be at least ${MINIMUM_BYTE_PACKET_SIZE}.`,
      );
    }
    if (result.packetSize > result.maxMessageSize + COMPLETE_METADATA_SIZE) {
      return invalidArgumentError(
        'packetSize must not exceed maxMessageSize plus complete-packet metadata.',
      );
    }
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

function setUint64LittleEndian(
  view: DataView,
  offset: number,
  value: bigint,
): void {
  view.setUint32(offset, Number(value & 0xffff_ffffn), true);
  view.setUint32(offset + 4, Number(value >> 32n), true);
}

function getUint64LittleEndian(view: DataView, offset: number): bigint {
  const low = BigInt(view.getUint32(offset, true));
  const high = BigInt(view.getUint32(offset + 4, true));
  return low | (high << 32n);
}

function completePacket(payload: Uint8Array, transientId: bigint): Uint8Array {
  const result = new Uint8Array(payload.byteLength + COMPLETE_METADATA_SIZE);
  result.set(payload);
  const view = new DataView(result.buffer);
  setUint64LittleEndian(view, payload.byteLength, transientId);
  result[result.length - 1] = BytePacketType.COMPLETE_BYTES;
  return result;
}

function chunkPacket(
  payload: Uint8Array,
  transientId: bigint,
  sequence: number,
  packetCount: number | null,
): Uint8Array {
  const metadataSize = packetCount === null
    ? CHUNK_METADATA_SIZE
    : FIRST_CHUNK_METADATA_SIZE;
  const result = new Uint8Array(payload.byteLength + metadataSize);
  result.set(payload);
  const view = new DataView(result.buffer);
  let offset = payload.byteLength;
  if (packetCount !== null) {
    view.setUint32(offset, packetCount, true);
    offset += 4;
  }
  view.setUint32(offset, sequence, true);
  setUint64LittleEndian(view, offset + 4, transientId);
  result[result.length - 1] = packetCount === null
    ? BytePacketType.BYTE_CHUNK
    : BytePacketType.LENGTH_SUFFIXED_BYTE_CHUNK;
  return result;
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
  if (!Number.isSafeInteger(packetSize) || packetSize < MINIMUM_BYTE_PACKET_SIZE) {
    return invalidArgumentError(
      `packetSize must be an integer of at least ${MINIMUM_BYTE_PACKET_SIZE}.`,
    );
  }
  try {
    if (bytes.byteLength <= packetSize - COMPLETE_METADATA_SIZE) {
      return [completePacket(bytes, id)];
    }

    const firstPayloadSize = packetSize - FIRST_CHUNK_METADATA_SIZE;
    const laterPayloadSize = packetSize - CHUNK_METADATA_SIZE;
    const laterCount = Math.ceil(
      (bytes.byteLength - firstPayloadSize) / laterPayloadSize,
    );
    if (!Number.isSafeInteger(laterCount) || laterCount >= UINT32_MAX) {
      return outOfRangeError('Byte message requires too many packets.');
    }
    const packetCount = laterCount + 1;
    const packets: Uint8Array[] = [
      chunkPacket(bytes.subarray(0, firstPayloadSize), id, 0, packetCount),
    ];
    let offset = firstPayloadSize;
    for (let sequence = 1; offset < bytes.byteLength; ++sequence) {
      const end = Math.min(bytes.byteLength, offset + laterPayloadSize);
      packets.push(chunkPacket(bytes.subarray(offset, end), id, sequence, null));
      offset = end;
    }
    return packets;
  } catch (error) {
    return statusFromUnknown(error, 'Failed to split byte message.');
  }
}

/** Parse one Action Engine byte packet without retaining its input buffer. */
export function parseBytePacket(source: ByteSource): StatusOr<BytePacket> {
  const packet = toBytes(source);
  if (!isOk(packet)) return packet;
  if (packet.byteLength < COMPLETE_METADATA_SIZE) {
    return invalidArgumentError(
      'Byte packet is shorter than complete-packet metadata.',
    );
  }
  try {
    const rawType = packet[packet.length - 1];
    if (
      rawType !== BytePacketType.COMPLETE_BYTES &&
      rawType !== BytePacketType.BYTE_CHUNK &&
      rawType !== BytePacketType.LENGTH_SUFFIXED_BYTE_CHUNK
    ) {
      return invalidArgumentError('Byte packet has an unknown type.');
    }
    const type = rawType as BytePacketType;
    const metadataSize = type === BytePacketType.COMPLETE_BYTES
      ? COMPLETE_METADATA_SIZE
      : type === BytePacketType.BYTE_CHUNK
        ? CHUNK_METADATA_SIZE
        : FIRST_CHUNK_METADATA_SIZE;
    if (packet.byteLength < metadataSize) {
      return invalidArgumentError(
        'Byte packet is shorter than its declared metadata.',
      );
    }
    const view = new DataView(
      packet.buffer,
      packet.byteOffset,
      packet.byteLength,
    );
    const transientOffset = packet.byteLength - COMPLETE_METADATA_SIZE;
    let sequence = 0;
    let packetCount = 0;
    if (type !== BytePacketType.COMPLETE_BYTES) {
      const sequenceOffset = transientOffset - 4;
      sequence = view.getUint32(sequenceOffset, true);
      if (type === BytePacketType.LENGTH_SUFFIXED_BYTE_CHUNK) {
        packetCount = view.getUint32(sequenceOffset - 4, true);
        if (sequence !== 0 || packetCount === 0) {
          return invalidArgumentError(
            'First byte chunk must have sequence zero and a positive count.',
          );
        }
      }
    }
    return {
      type,
      payload: packet.slice(0, packet.byteLength - metadataSize),
      transientId: getUint64LittleEndian(view, transientOffset),
      sequence,
      packetCount,
    };
  } catch (error) {
    return statusFromUnknown(error, 'Failed to parse byte packet.');
  }
}

interface PendingMessage {
  packetCount: number | null;
  chunks: Map<number, Uint8Array>;
  byteCount: number;
}

/** Bounded out-of-order and interleaved byte-message reassembly. */
export class ByteReassembler {
  readonly options: Readonly<NormalizedByteChunkingOptions>;
  private readonly pending = new Map<bigint, PendingMessage>();
  private pendingBytesInternal = 0;

  private constructor(options: NormalizedByteChunkingOptions) {
    this.options = Object.freeze({ ...options });
  }

  static create(options: ByteChunkingOptions = {}): StatusOr<ByteReassembler> {
    try {
      const normalized = normalizeByteChunkingOptions(options);
      return isOk(normalized) ? new ByteReassembler(normalized) : normalized;
    } catch (error) {
      return statusFromUnknown(error, 'Creating ByteReassembler raised an exception.');
    }
  }

  get pendingMessageCount(): number {
    return this.pending.size;
  }

  get pendingByteCount(): number {
    return this.pendingBytesInternal;
  }

  clear(): Status {
    this.pending.clear();
    this.pendingBytesInternal = 0;
    return okStatus();
  }

  feed(source: ByteSource): StatusOr<Uint8Array | null> {
    const serialized = toBytes(source);
    if (!isOk(serialized)) return serialized;
    if (serialized.byteLength > this.options.packetSize) {
      return outOfRangeError('Incoming byte packet exceeds packetSize.');
    }
    const packet = parseBytePacket(serialized);
    if (!isOk(packet)) return packet;
    if (packet.payload.byteLength > this.options.maxMessageSize) {
      return outOfRangeError('Incoming byte message exceeds its limit.');
    }
    if (packet.type === BytePacketType.COMPLETE_BYTES) {
      if (this.pending.has(packet.transientId)) {
        return alreadyExistsError(
          'Complete byte packet collides with pending chunks.',
        );
      }
      return packet.payload;
    }
    if (packet.sequence > this.options.maxMessageSize) {
      return outOfRangeError(
        'Byte chunk sequence exceeds the configured message bound.',
      );
    }
    if (
      packet.type === BytePacketType.LENGTH_SUFFIXED_BYTE_CHUNK &&
      packet.packetCount > this.options.maxMessageSize + 1
    ) {
      return outOfRangeError(
        'Byte message declares an unreasonable packet count.',
      );
    }
    if (
      this.pendingBytesInternal + packet.payload.byteLength >
      this.options.maxPendingBytes
    ) {
      return resourceExhaustedError(
        'Pending byte chunks exceed maxPendingBytes.',
      );
    }

    let pending = this.pending.get(packet.transientId);
    if (pending === undefined) {
      if (this.pending.size >= this.options.maxPendingMessages) {
        return resourceExhaustedError(
          'Too many byte messages are pending reassembly.',
        );
      }
      pending = { packetCount: null, chunks: new Map(), byteCount: 0 };
      this.pending.set(packet.transientId, pending);
    }

    if (packet.type === BytePacketType.LENGTH_SUFFIXED_BYTE_CHUNK) {
      if (
        pending.packetCount !== null &&
        pending.packetCount !== packet.packetCount
      ) {
        return invalidArgumentError(
          'Byte message has conflicting packet counts.',
        );
      }
      for (const sequence of pending.chunks.keys()) {
        if (sequence >= packet.packetCount) {
          this.dropPending(packet.transientId, pending);
          return outOfRangeError(
            'Byte chunk sequence exceeds the declared packet count.',
          );
        }
      }
      pending.packetCount = packet.packetCount;
    }
    if (
      pending.packetCount !== null &&
      packet.sequence >= pending.packetCount
    ) {
      return outOfRangeError(
        'Byte chunk sequence exceeds the declared packet count.',
      );
    }
    if (pending.chunks.has(packet.sequence)) {
      return alreadyExistsError('Duplicate byte chunk sequence.');
    }
    if (
      pending.byteCount + packet.payload.byteLength >
      this.options.maxMessageSize
    ) {
      return outOfRangeError(
        'Reassembled byte message exceeds maxMessageSize.',
      );
    }
    pending.chunks.set(packet.sequence, packet.payload);
    pending.byteCount += packet.payload.byteLength;
    this.pendingBytesInternal += packet.payload.byteLength;

    if (
      pending.packetCount === null ||
      pending.chunks.size !== pending.packetCount
    ) {
      return null;
    }
    const parts: Uint8Array[] = [];
    for (let sequence = 0; sequence < pending.packetCount; ++sequence) {
      const part = pending.chunks.get(sequence);
      if (part === undefined) return null;
      parts.push(part);
    }
    const result = concatBytes(parts);
    this.dropPending(packet.transientId, pending);
    return result;
  }

  private dropPending(id: bigint, pending: PendingMessage): void {
    this.pendingBytesInternal -= pending.byteCount;
    this.pending.delete(id);
  }
}
