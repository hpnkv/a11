// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import { AsyncNode, NodeMap } from './async_node.js';
import { ChunkStoreReader } from './chunk_store_reader.js';
import { type WritableWireStream } from './chunk_store_writer.js';
import { NodeFragment, NodeRef, WireMessage, makeNullChunk } from './data.js';
import { statusToChunk } from './action_schema.js';
import {
  StatusCode, dataLossError, failedPreconditionError, isOk, isStatus, okStatus,
  resourceExhaustedError, statusFromUnknown, type Status, type StatusOr,
} from './status.js';

export const REFERENCE_MIRROR_PREFIX = 'a11_mirror_';

type Phase = 'starting' | 'replaying' | 'closed' | 'failed';
interface Mirror {
  id: string;
  phase: Phase;
  status: Status;
}

/** Receiver-scoped replay cursors for references the peer cannot read. */
export class ReferenceMirrorRegistry {
  private readonly mirrors = new WeakMap<NodeMap, Map<string, Mirror>>();
  private readonly direct: Set<string>;
  private active = 0;

  constructor(private readonly stream: WritableWireStream, directStorageIds: readonly string[]) {
    this.direct = new Set(directStorageIds);
  }

  async rewrite(reference: NodeRef, sourceMap: NodeMap | null): Promise<StatusOr<NodeRef>> {
    const valid = reference.validate();
    if (!isOk(valid)) return valid;
    if (reference.length === 0) {
      return new NodeRef({id: reference.id, offset: reference.offset, length: 0});
    }
    if (reference.storage !== null && this.direct.has(reference.storage.id)) return reference;
    if (sourceMap === null) return failedPreconditionError('Reference source map is unavailable.');
    const key = `${reference.storage?.id.length ?? 0}:${reference.storage?.id ?? ''}${reference.id}`;
    const source = await sourceMap.resolveForRead(reference);
    if (!isOk(source)) return source;
    const group = this.mirrors.get(sourceMap) ?? new Map();
    const raced = group.get(key);
    if (raced) {
      if (raced.phase === 'failed' && !isOk(raced.status)) return raced.status;
      return new NodeRef({id: raced.id, offset: reference.offset, length: reference.length});
    }
    if (this.active >= 128) return resourceExhaustedError('Active mirror limit reached.');
    if (!globalThis.crypto?.getRandomValues) {
      return failedPreconditionError('Secure random mirror ids are unavailable.');
    }
    let suffix: string;
    try {
      const bytes = globalThis.crypto.getRandomValues(new Uint8Array(16));
      suffix = [...bytes].map(byte => byte.toString(16).padStart(2, '0')).join('');
    } catch {
      return failedPreconditionError('Secure random mirror ids are unavailable.');
    }
    const mirror: Mirror = {
      id: REFERENCE_MIRROR_PREFIX + suffix,
      phase: 'starting', status: okStatus(),
    };
    group.set(key, mirror);
    this.mirrors.set(sourceMap, group);
    ++this.active;
    void this.pump(mirror, source, source.nodeMap ?? sourceMap).then((status) => {
      mirror.status = status;
      mirror.phase = isOk(status) ? 'closed' : 'failed';
      --this.active;
      if (!isOk(status)) this.sendFailure(mirror.id, status);
    });
    return new NodeRef({id: mirror.id, offset: reference.offset, length: reference.length});
  }

  private async pump(mirror: Mirror, source: AsyncNode, sourceMap: NodeMap): Promise<Status> {
    const cursor = ChunkStoreReader.create(source.chunkStore, {
      followNodeRefs: false, numChunksToBuffer: 8,
    });
    if (!isOk(cursor)) return cursor;
    mirror.phase = 'replaying';
    let nextSeq = 0;
    let continued = true;
    try {
      while (true) {
        const next = await cursor.next(1000);
        if (!isOk(next) && next.code === StatusCode.DEADLINE_EXCEEDED) {
          const wire = this.stream.getStatus?.();
          if (wire !== undefined && !isOk(wire)) return wire;
          continue;
        }
        if (!isOk(next)) return next;
        if (next === null) break;
        let data = next.data;
        if (data instanceof NodeRef) {
          const rewritten = await this.rewrite(data, sourceMap);
          if (!isOk(rewritten)) return rewritten;
          data = rewritten;
        }
        if (next.seq === null) return dataLossError('Mirrored fragment has no sequence.');
        if (next.seq === 0xffffffff) return resourceExhaustedError('Mirror sequence limit reached.');
        nextSeq = next.seq + 1;
        continued = next.continued;
        const sent = this.send(new WireMessage({
          nodeFragments: [new NodeFragment({
            id: mirror.id, data, seq: next.seq, continued: next.continued,
          })],
        }));
        if (!isOk(sent)) return sent;
      }
      if (continued) {
        return this.send(new WireMessage({
          nodeFragments: [new NodeFragment({
            id: mirror.id, data: makeNullChunk(), seq: nextSeq, continued: false,
          })],
        }));
      }
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Reference mirror replay failed.');
    } finally {
      cursor.cancel();
    }
  }

  private sendFailure(id: string, status: Status): void {
    const chunk = statusToChunk(status, true);
    if (!isOk(chunk)) return;
    this.send(new WireMessage({
      nodeFragments: [new NodeFragment({id, data: chunk, seq: 0, continued: false})],
    }));
  }

  private send(message: WireMessage): Status {
    try {
      const result = this.stream.send(message);
      return isStatus(result) ? result : dataLossError('WireStream send returned an invalid status.');
    } catch (error) {
      return statusFromUnknown(error, 'Reference mirror transport failed.');
    }
  }
}
