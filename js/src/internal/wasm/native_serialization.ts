export interface SerializationBridge {
  jsonToChunk(value: unknown, mimetype?: string): Uint8Array;
  jsonFromChunk(bytes: Uint8Array, patterns?: readonly string[]): unknown;
  bytesToChunk(bytes: Uint8Array, mimetype: string): Uint8Array;
  bytesFromChunk(bytes: Uint8Array): Uint8Array;
}

let initialized: SerializationBridge | null = null;

export function installSerializationBridge(bridge: SerializationBridge): void {
  initialized = bridge;
}

export function serializationBridge(): SerializationBridge | null {
  return initialized;
}
