import type {BinaryChannel, ChannelFramingOptions,
  ChannelEndpointRole} from '../../channel_wire_stream.js';
import type {WireStreamOptions} from '../../wire_stream.js';
import type {Status} from '../../status.js';

export interface ChannelHandle {
  readonly __a11NativeChannelStream: true;
  readonly id: string;
  host?: import('../../wire_stream.js').WireStream;
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

let installed: ChannelBridge | null = null;

export function installChannelBridge(bridge: ChannelBridge): void {
  installed = bridge;
}

export function channelBridge(): ChannelBridge | null {
  return installed;
}
