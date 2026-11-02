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

import {WireMessage, wireMessageFromJsonValue,
  wireMessageToJsonValue} from './data.js';
import {isOk, type Status} from './status.js';
import {type WireStream} from './wire_stream.js';

export interface WireStreamBridge {
  readonly id: string;
  readonly host: WireStream;
  send(message: Record<string, unknown>): Status;
  halfClose(message: Record<string, unknown>): Status;
  abort(status: Status): Status;
  getStatus(): Status;
  start(onMessage: (message: Record<string, unknown> | null,
      trailers?: Record<string, unknown>) =>
      Promise<Status>, onDone: () => Promise<Status>): Promise<Status>;
  accept(onMessage: (message: Record<string, unknown> | null,
      trailers?: Record<string, unknown>) =>
      Promise<Status>, onDone: () => Promise<Status>): Promise<Status>;
}

/** Connect a host WireStream to the worker's C++ in-process endpoint. */
export function bridgeWireStream(stream: WireStream): WireStreamBridge {
  const incoming = (onMessage: (message: Record<string, unknown> | null,
      trailers?: Record<string, unknown>) =>
      Promise<Status>) => async (message: WireMessage | null): Promise<Status> => {
    if (message === null) {
      const trailers = wireMessageToJsonValue(new WireMessage({
        headers: stream.getTrailers() ?? new Map(),
      }));
      return isOk(trailers) ? onMessage(null, trailers) : trailers;
    }
    const encoded = wireMessageToJsonValue(message);
    return isOk(encoded) ? onMessage(encoded) : encoded;
  };
  return {
    id: `${stream.getId()}-${globalThis.crypto.randomUUID()}`,
    host: stream,
    send(value) {
      const decoded = wireMessageFromJsonValue(value);
      return isOk(decoded) ? stream.send(decoded) : decoded;
    },
    halfClose(value) {
      const decoded = wireMessageFromJsonValue(value);
      return isOk(decoded) ? stream.halfClose(decoded.headers) : decoded;
    },
    abort: status => stream.abort(status),
    getStatus: () => stream.getStatus(),
    start: (onMessage, onDone) => stream.start(incoming(onMessage), onDone),
    accept: (onMessage, onDone) => stream.accept(incoming(onMessage), onDone),
  };
}
