export interface StreamReferenceBridge {
  toChunk(value: Record<string, unknown>): Uint8Array;
  fromChunk(bytes: Uint8Array): Record<string, unknown>;
}

let initialized: StreamReferenceBridge | null = null;

export function installStreamReferenceBridge(bridge: StreamReferenceBridge): void {
  initialized = bridge;
}

export function streamReferenceBridge(): StreamReferenceBridge | null {
  return initialized;
}
