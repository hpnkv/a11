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

import {NodeRef} from './data.js';
import {NODE_REF_TAG} from './serial_tags.js';
import {invalidArgumentError, isOk, type StatusOr} from './status.js';
import {wireValueCodecByTag} from './wire_values.js';

/** Locate an auxiliary producer independently of its stream payload. */
export interface StreamReference {
  producer_id: string;
  parent_producer_id: string;
  node: NodeRef;
  purpose: string;
  source: Record<string, unknown>;
}

/** Decode a stream descriptor through the existing NodeRef wire codec. */
export function parseStreamReference(value: unknown): StatusOr<StreamReference> {
  if (!value || typeof value !== 'object') return invalidArgumentError('Invalid stream reference.');
  const fields = value as Record<string, unknown>;
  if (typeof fields.producer_id !== 'string' || !fields.producer_id ||
      typeof fields.purpose !== 'string' || !fields.purpose ||
      (fields.parent_producer_id !== undefined && typeof fields.parent_producer_id !== 'string') ||
      !fields.source || typeof fields.source !== 'object' || Array.isArray(fields.source) ||
      !fields.node || typeof fields.node !== 'object' || Array.isArray(fields.node)) {
    return invalidArgumentError('Invalid stream reference fields.');
  }
  const node = fields.node instanceof NodeRef ? fields.node
    : wireValueCodecByTag(NODE_REF_TAG)!.load(fields.node as Record<string, unknown>);
  if (!isOk(node)) return node;
  if (!(node instanceof NodeRef)) return invalidArgumentError('Invalid node reference.');
  const valid = node.validate();
  if (!isOk(valid)) return valid;
  return {
    producer_id: fields.producer_id, parent_producer_id: fields.parent_producer_id as string ?? '',
    node, purpose: fields.purpose, source: fields.source as Record<string, unknown>,
  };
}
