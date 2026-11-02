import type {StatusJson} from '../../status.js';

/** Status codec supplied by the initialized C++ WASM module. */
export interface StatusCodecBridge {
  pack(value: StatusJson): Uint8Array;
  unpack(bytes: Uint8Array): StatusJson;
  chunk(value: StatusJson, closing?: boolean): Uint8Array;
  fromChunk(bytes: Uint8Array): StatusJson;
}

let initialized: StatusCodecBridge | null = null;

export function installStatusCodecBridge(bridge: StatusCodecBridge): void {
  initialized = bridge;
}

export function statusCodecBridge(): StatusCodecBridge | null {
  return initialized;
}
