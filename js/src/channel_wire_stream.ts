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

import {type ByteMap, type ByteMapInput} from './bytes.js';
import {WireMessage, makeHalfCloseMessage, wireMessageFromJsonValue,
  wireMessageToJsonValue, validateName} from './data.js';
import {channelBridge, type ChannelHandle} from './internal/wasm/native_channel.js';
import {statusFromBridgeFailure} from './internal/wasm/status_bridge.js';
import {invalidArgumentError, isOk, isStatus, okStatus, statusFromUnknown,
  unavailableError, unimplementedError, type NonOkStatus,
  type Status, type StatusOr}
  from './status.js';
import {normalizeWireHeaders, wireDeadlineMillis, type OnWireDone,
  type OnWireMessage, type WireDeadline, type WireStream,
  type WireStreamOptions} from './wire_stream.js';

export enum ChannelEndpointRole {
  CLIENT = 'client',
  SERVER = 'server',
  EITHER = 'either',
}

export interface ChannelFramingOptions {
  splitSize?: number;
  maxPendingMessages?: number;
  maxPendingBytes?: number;
}

export interface NormalizedChannelFramingOptions {
  splitSize: number;
  maxPendingMessages: number;
  maxPendingBytes: number;
}

export interface BinaryChannelCallbacks {
  onOpen: () => void;
  onMessage: (packet: Uint8Array) => void;
  onError: (status: NonOkStatus) => void;
  onClosed: () => void;
  onBufferedAmountLow: () => void;
}

/** Host packet transport used by the native channel stream. */
export interface BinaryChannel {
  setCallbacks(callbacks: BinaryChannelCallbacks): Status;
  resetCallbacks(): Status;
  open(): Promise<Status>;
  isOpen(): boolean;
  send(packet: Uint8Array): Status;
  bufferedAmount(): StatusOr<number>;
  waitForBufferedAmountLow(): Promise<Status>;
  close(): Status;
  abort?(status: NonOkStatus): Status;
  getImpl(): unknown | null;
}

/** Default the framing limits; C++ validates them when the stream starts. */
export function normalizeChannelFramingOptions(
  options: ChannelFramingOptions = {},
  _maxMessageSize = 32 * 1024 * 1024,
): StatusOr<NormalizedChannelFramingOptions> {
  return {
    splitSize: options.splitSize ?? 64 * 1024,
    maxPendingMessages: options.maxPendingMessages ?? 64,
    maxPendingBytes: options.maxPendingBytes ?? 64 * 1024 * 1024,
  };
}

/** A TypeScript facade over C++ ChannelWireStream in the threaded WASM worker. */
export class ChannelWireStream implements WireStream {
  readonly options: Readonly<WireStreamOptions>;
  readonly framing: Readonly<ChannelFramingOptions>;
  private readonly handle: Promise<ChannelHandle>;
  private finalStatus: Status = okStatus();
  private earlyError: NonOkStatus | null = null;
  private finished: Promise<Status>;
  private finish: (status: Status) => void = () => {};

  private constructor(private readonly channel: BinaryChannel,
    private readonly id: string, role: ChannelEndpointRole,
    options: WireStreamOptions, framing: ChannelFramingOptions) {
    this.options = Object.freeze({...options});
    this.framing = Object.freeze({...framing});
    this.finished = new Promise(resolve => { this.finish = resolve; });
    channel.setCallbacks({
      onOpen: () => {},
      onMessage: () => {},
      onError: status => { this.earlyError = status; },
      onClosed: () => {
        this.earlyError = unavailableError('Binary channel closed before startup.');
      },
      onBufferedAmountLow: () => {},
    });
    this.handle = channelBridge()!.createChannelStream(
      channel, id, role, options, framing);
    void this.handle.catch(() => {});
  }

  static create(channel: BinaryChannel, id: string,
    role: ChannelEndpointRole = ChannelEndpointRole.EITHER,
    options: WireStreamOptions = {},
    framing: ChannelFramingOptions = {}): StatusOr<ChannelWireStream> {
    if (channelBridge() === null) {
      return unimplementedError('Initialize A11 WASM before creating a channel stream.');
    }
    if (typeof channel !== 'object' || channel === null ||
        typeof channel.setCallbacks !== 'function' ||
        typeof channel.open !== 'function') {
      return invalidArgumentError('channel must implement BinaryChannel.');
    }
    const name = validateName(id);
    if (!isOk(name)) return name;
    if (!Object.values(ChannelEndpointRole).includes(role)) {
      return invalidArgumentError('role is not a valid ChannelEndpointRole.');
    }
    return new ChannelWireStream(channel, id, role, options, framing);
  }

  private async invoke(operation: (handle: ChannelHandle) => Promise<unknown>,
    context: string): Promise<Status> {
    try {
      await operation(await this.handle);
      return okStatus();
    } catch (error) {
      return statusFromBridgeFailure(error, context);
    }
  }

  async send(message: WireMessage): Promise<Status> {
    if (!(message instanceof WireMessage)) {
      return invalidArgumentError('message must be a WireMessage.');
    }
    const raw = wireMessageToJsonValue(message);
    if (!isOk(raw)) return raw;
    return this.invoke(handle => handle.send(raw), 'Native channel send failed.');
  }

  start(onMessage?: OnWireMessage, onDone?: OnWireDone): Promise<Status> {
    return this.startEndpoint('start', onMessage, onDone);
  }

  accept(onMessage?: OnWireMessage, onDone?: OnWireDone): Promise<Status> {
    return this.startEndpoint('accept', onMessage, onDone);
  }

  private async startEndpoint(mode: 'start' | 'accept',
    onMessage?: OnWireMessage, onDone?: OnWireDone): Promise<Status> {
    if (onMessage !== undefined && typeof onMessage !== 'function') {
      return invalidArgumentError('onMessage must be callable.');
    }
    if (onDone !== undefined && typeof onDone !== 'function') {
      return invalidArgumentError('onDone must be callable.');
    }
    if (this.earlyError !== null) {
      this.finish(this.earlyError);
      return this.earlyError;
    }
    const started = await this.invoke(handle => handle.start(async value => {
      const message = value === null ? null : wireMessageFromJsonValue(value);
      if (message !== null && !isOk(message)) return message;
      try {
        const status = await onMessage?.(message);
        return status === undefined ? okStatus() :
          isStatus(status) ? status : invalidArgumentError(
            'onMessage returned an invalid status.');
      } catch (error) {
        return statusFromUnknown(error, 'Channel message callback failed.');
      }
    }, async value => {
      this.finalStatus = isStatus(value) ? value : invalidArgumentError(
        'Native channel completion status is invalid.');
      try {
        const status = await onDone?.();
        const result = status === undefined ? okStatus() :
          isStatus(status) ? status : invalidArgumentError(
            'onDone returned an invalid status.');
        this.finish(this.finalStatus);
        return result;
      } catch (error) {
        const result = statusFromUnknown(error, 'Channel done callback failed.');
        this.finish(result);
        return result;
      }
    }, mode), 'Native channel startup failed.');
    if (!isOk(started)) this.finish(started);
    return started;
  }

  async halfClose(trailers?: ByteMapInput): Promise<Status> {
    const normalized = normalizeWireHeaders(trailers);
    if (!isOk(normalized)) return normalized;
    const raw = wireMessageToJsonValue(makeHalfCloseMessage(normalized));
    if (!isOk(raw)) return raw;
    return this.invoke(handle => handle.halfClose(raw),
      'Native channel half-close failed.');
  }

  drainOutgoingMessages(): Promise<Status> {
    return this.invoke(handle => handle.drain(), 'Native channel drain failed.');
  }

  abort(status: Status): Promise<Status> {
    if (!isStatus(status) || isOk(status)) {
      return Promise.resolve(invalidArgumentError('Abort status must be non-OK.'));
    }
    return this.invoke(handle => handle.abort(status), 'Native channel abort failed.');
  }

  async setDeadline(deadline?: WireDeadline): Promise<Status> {
    const value = wireDeadlineMillis(deadline);
    if (!isOk(value)) return value;
    return this.invoke(handle => handle.setDeadline(value),
      'Native channel deadline failed.');
  }

  async getDeadline(): Promise<number | null> {
    try {
      const value = await (await this.handle).status();
      return typeof value.deadlineMs === 'number' ? value.deadlineMs : null;
    } catch { return null; }
  }

  async getStatus(): Promise<Status> {
    if (!isOk(this.finalStatus)) return this.finalStatus;
    try {
      const value = await (await this.handle).status();
      return isStatus(value.status) ? value.status : this.finalStatus;
    } catch (error) {
      return statusFromBridgeFailure(error, 'Native channel status failed.');
    }
  }

  async getTrailers(): Promise<ByteMap | null> {
    try {
      const value = await (await this.handle).status();
      if (value.trailers === undefined) return null;
      const message = wireMessageFromJsonValue(
        value.trailers as Record<string, unknown>);
      return isOk(message) ? message.headers : null;
    } catch { return null; }
  }

  getId(): string { return this.id; }
  getImpl(): unknown | null { return this.channel.getImpl(); }
  wait(): Promise<Status> { return this.finished; }
  async release(): Promise<void> { await (await this.handle).release(); }

  async nativeHandle(): Promise<ChannelHandle> { return this.handle; }

  async nativeReceiveStart(mode: 'start' | 'accept',
    onMessage?: OnWireMessage, onDone?: OnWireDone): Promise<Status> {
    const started = await this.invoke(handle => handle.receiveStart(mode, async value => {
      const message = value === null ? null : wireMessageFromJsonValue(value);
      if (message !== null && !isOk(message)) return message;
      try {
        const result = await onMessage?.(message);
        return result === undefined ? okStatus() : isStatus(result) ? result :
          invalidArgumentError('Channel observer returned an invalid status.');
      } catch (error) {
        return statusFromUnknown(error, 'Channel observer failed.');
      }
    }, async value => {
      this.finalStatus = isStatus(value) ? value : invalidArgumentError(
        'Native channel completion status is invalid.');
      try {
        const result = await onDone?.();
        return result === undefined ? okStatus() : isStatus(result) ? result :
          invalidArgumentError('Channel completion observer returned an invalid status.');
      } catch (error) {
        return statusFromUnknown(error, 'Channel completion observer failed.');
      } finally {
        this.finish(this.finalStatus);
      }
    }), 'Native channel receive startup failed.');
    if (!isOk(started)) this.finish(started);
    return started;
  }
}
