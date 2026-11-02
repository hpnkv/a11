/** Framing entry points supplied by the initialized C++ WASM module. */
export interface FramingBridge {
  validateOptions(options: {
    packetSize: number; maxMessageSize: number;
    maxPendingMessages: number; maxPendingBytes: number;
  }): void;
  parse(bytes: Uint8Array): {
    type: number; payload: Uint8Array; transientId: bigint;
    sequence: number; packetCount: number;
  };
  split(bytes: Uint8Array, transientId: bigint | number,
    packetSize: number): Uint8Array[];
  reassembler(options: {
    packetSize: number; maxMessageSize: number;
    maxPendingMessages: number; maxPendingBytes: number;
  }): ReassemblerBridge;
}

export interface ReassemblerBridge {
  readonly pendingMessageCount: number;
  readonly pendingByteCount: number;
  feed(packet: Uint8Array): Uint8Array | null;
  clear(): void;
  dispose(): void;
}

let initialized: FramingBridge | null = null;

export function installFramingBridge(bridge: FramingBridge): void {
  initialized = bridge;
}

export function framingBridge(): FramingBridge | null {
  return initialized;
}
