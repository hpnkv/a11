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

import {Chunk, NodeRef} from './data.js';
import {streamReferenceBridge} from './internal/wasm/native_stream_reference.js';
import {statusFromBridgeFailure} from './internal/wasm/status_bridge.js';
import {NODE_REF_TAG} from './serial_tags.js';
import {failedPreconditionError, invalidArgumentError, isOk,
  type StatusOr} from './status.js';
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
  const native = streamReferenceBridge();
  if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before using A11 protocol codecs.');
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
      return invalidArgumentError('Invalid stream reference.');
    }
    try {
      const encoded = native.toChunk(value as Record<string, unknown>);
      return hydrateStreamReference(native.fromChunk(encoded));
    } catch (error) {
      return statusFromBridgeFailure(error, 'Stream reference is invalid.');
    }
}

function hydrateStreamReference(fields: Record<string, unknown>):
    StatusOr<StreamReference> {
  const node = wireValueCodecByTag(NODE_REF_TAG)!.load(
    fields.node as Record<string, unknown>);
  if (!isOk(node)) return node;
  if (!(node instanceof NodeRef)) {
    return invalidArgumentError('Invalid node reference.');
  }
  return {producer_id: fields.producer_id as string,
    parent_producer_id: fields.parent_producer_id as string,
    node, purpose: fields.purpose as string,
    source: fields.source as Record<string, unknown>};
}

/** Encode a stream descriptor using the native C++ reference protocol. */
export function streamReferenceToChunk(value: StreamReference): StatusOr<Chunk> {
  const native = streamReferenceBridge();
  if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before encoding stream references.');
  try { return Chunk.fromMsgpack(native.toChunk(value as unknown as Record<string, unknown>)); }
  catch (error) { return statusFromBridgeFailure(error,
    'Stream reference encoding failed.'); }
}

/** Decode a reference chunk through the native C++ reference protocol. */
export function streamReferenceFromChunk(chunk: Chunk): StatusOr<StreamReference> {
  const native = streamReferenceBridge();
  if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before decoding stream references.');
  const bytes = chunk.toMsgpack();
  if (!isOk(bytes)) return bytes;
  try { return hydrateStreamReference(native.fromChunk(bytes)); }
  catch (error) { return statusFromBridgeFailure(error,
    'Stream reference decoding failed.'); }
}
