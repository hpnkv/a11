import type {BinaryChannel, ChannelFramingOptions,
  ChannelEndpointRole} from '../../channel_wire_stream.js';
import type {WireStreamOptions} from '../../wire_stream.js';
import type {Status} from '../../status.js';

export interface ChannelHandle {
  readonly __a11NativeChannelStream: true;
  readonly id: string;
  host?: import('../../wire_stream.js').WireStream;
  fallback?: import('./wire_stream_bridge.js').WireStreamBridge;
  send(message: Record<string, unknown>): Promise<unknown>;
  halfClose(message: Record<string, unknown>): Promise<unknown>;
  abort(status: Status): Promise<unknown>;
  drain(): Promise<unknown>;
  setDeadline(deadlineMs: number | null): Promise<unknown>;
  status(): Promise<Record<string, unknown>>;
  receiveStart(mode: 'start' | 'accept',
    onMessage: (message: Record<string, unknown> | null) => Promise<unknown>,
    onDone: (status: Record<string, unknown>) => Promise<unknown>):
    Promise<unknown>;
  receive(timeoutMs?: number): Promise<Record<string, unknown> | null>;
  start(onMessage: (message: Record<string, unknown> | null) => Promise<unknown>,
    onDone: (status: Record<string, unknown>) => Promise<unknown>,
    mode: 'start' | 'accept'): Promise<unknown>;
  release(): Promise<void>;
}

export interface ChannelBridge {
  createChannelStream(channel: BinaryChannel, id: string,
    role: ChannelEndpointRole, options: WireStreamOptions,
    framing: ChannelFramingOptions): Promise<ChannelHandle>;
}

const installed: ChannelBridge[] = [];

export function installChannelBridge(bridge: ChannelBridge): void {
  installed.push(bridge);
}

export function removeChannelBridge(bridge: ChannelBridge): void {
  const index = installed.lastIndexOf(bridge);
  if (index !== -1) installed.splice(index, 1);
}

export function channelBridge(): ChannelBridge | null {
  return installed[0] ?? null;
}
