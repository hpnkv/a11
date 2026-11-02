/** Codec bridge installed after the C++ WASM module has loaded. */
export interface DataCodecBridge {
  validateName(value: string): Uint8Array;
  encode(kind: string, value: Record<string, unknown>): Uint8Array;
  decode(kind: string, bytes: Uint8Array): Record<string, unknown>;
}

let initialized: DataCodecBridge | null = null;

export function installDataCodecBridge(bridge: DataCodecBridge): void {
  initialized = bridge;
}

export function dataCodecBridge(): DataCodecBridge | null {
  return initialized;
}
