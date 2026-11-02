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

import type {Deadline} from './runtime_options.js';
import {NodeFragment, WireMessage, wireMessageFromJsonValue,
  wireMessageToJsonValue} from './data.js';
import {invalidArgumentError, isOk, type NonOkStatus,
  type StatusOr} from './status.js';
import {statusFromBridgeFailure} from './status_bridge.js';

export function checkedDeadline(deadline: Deadline | undefined): StatusOr<Deadline> {
  if (deadline === undefined || deadline === null) return null;
  const value = deadline instanceof Date ? deadline.getTime() : deadline;
  return typeof value === 'number' && Number.isFinite(value)
    ? deadline : invalidArgumentError('Deadline must be a finite epoch millisecond value, a Date, or null.');
}

export function failure(error: unknown, context: string): NonOkStatus {
  return statusFromBridgeFailure(error, context);
}

export function fromRaw(value: Record<string, unknown>): StatusOr<NodeFragment> {
  const message = wireMessageFromJsonValue({node_fragments: [value]});
  return isOk(message) ? message.nodeFragments[0]! : message;
}

export function toRaw(fragment: NodeFragment): StatusOr<Record<string, unknown> & {id: string}> {
  const message = wireMessageToJsonValue(new WireMessage({nodeFragments: [fragment]}));
  if (!isOk(message)) return message;
  const raw = (message.node_fragments as Record<string, unknown>[])[0]!;
  return {...raw, id: fragment.id};
}
