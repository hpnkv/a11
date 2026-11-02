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

import {Chunk, NodeFragment, WireMessage, wireMessageFromJsonValue,
  wireMessageToJsonValue} from './data.js';
import {A11Runtime} from './runtime_backend.js';
import {invalidArgumentError, isOk, type NonOkStatus,
  type StatusOr} from './status.js';

export interface FlowRunOptions {
  inputs?: Record<string, readonly Chunk[]>;
  timeoutMs?: number;
  maxOutputChunks?: number;
  maxOutputBytes?: number;
  signal?: AbortSignal;
  onOutput?: (port: string, chunk: Chunk) => void;
}

function decodeChunk(port: string, raw: unknown): StatusOr<Chunk> {
  if (typeof raw !== 'object' || raw === null || Array.isArray(raw)) {
    return invalidArgumentError('Native Flow returned a malformed chunk.');
  }
  const message = wireMessageFromJsonValue({node_fragments: [{id: port,
    data: raw}]});
  return isOk(message) ? message.nodeFragments[0]!.getChunk() : message;
}

/** Native Flow language tooling, with no second parser in TypeScript. */
export class FlowTooling {
  constructor(private readonly runtime: A11Runtime) {}

  request(method: string, source: string,
      options: Record<string, unknown> = {}):
      StatusOr<Record<string, unknown>> {
    return this.runtime.flowTool({...options, method, source});
  }

  check(source: string, options: Record<string, unknown> = {}):
      StatusOr<Record<string, unknown>> {
    return this.request('check', source, options);
  }

  format(source: string, options: Record<string, unknown> = {}):
      StatusOr<Record<string, unknown>> {
    return this.request('format', source, options);
  }

  tokens(source: string, options: Record<string, unknown> = {}):
      StatusOr<Record<string, unknown>> {
    return this.request('tokens', source, options);
  }
}

/** One native Flow program with typed chunk inputs and outputs. */
export class FlowProgram {
  constructor(private readonly runtime: A11Runtime,
      readonly source: string, readonly name: string) {}

  async run(options: FlowRunOptions = {}):
      Promise<StatusOr<Record<string, Chunk[]>>> {
    const inputs: Record<string, Array<Record<string, unknown>>> = {};
    for (const [port, chunks] of Object.entries(options.inputs ?? {})) {
      inputs[port] = [];
      for (const chunk of chunks) {
        const message = wireMessageToJsonValue(new WireMessage({nodeFragments: [
          new NodeFragment({id: port, data: chunk})]}));
        if (!isOk(message)) return message;
        const fragments = message.node_fragments;
        if (!Array.isArray(fragments) || fragments.length !== 1) {
          return invalidArgumentError('Flow input chunk encoding failed.');
        }
        const fragment = fragments[0] as Record<string, unknown>;
        inputs[port].push(fragment.data as Record<string, unknown>);
      }
    }
    let callbackFailure: NonOkStatus | null = null;
    const raw = await this.runtime.runFlow({source: this.source, flow: this.name,
      inputs, timeoutMs: options.timeoutMs,
      maxOutputChunks: options.maxOutputChunks,
      maxOutputBytes: options.maxOutputBytes}, {
      signal: options.signal,
      onOutput: options.onOutput === undefined ? undefined : (port, value) => {
        const chunk = decodeChunk(port, value);
        if (!isOk(chunk)) {
          callbackFailure = chunk;
          throw new Error(chunk.message);
        }
        options.onOutput!(port, chunk);
      },
    });
    if (callbackFailure !== null) return callbackFailure;
    if (!isOk(raw)) return raw;
    const result: Record<string, Chunk[]> = {};
    for (const [port, values] of Object.entries(raw)) {
      if (!Array.isArray(values)) {
        return invalidArgumentError('Native Flow returned malformed outputs.');
      }
      result[port] = [];
      for (const value of values) {
        const chunk = decodeChunk(port, value);
        if (!isOk(chunk)) return chunk;
        result[port].push(chunk);
      }
    }
    return result;
  }
}
