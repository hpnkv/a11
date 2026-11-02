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

import type {NodeFragment} from './data.js';
import type {Status, StatusOr} from './status.js';
import type {WireDeadline} from './wire_stream.js';

/** Absolute JavaScript epoch deadline. `null` waits indefinitely. */
export type Deadline = number | Date | null;

/** Store contract implemented by the native-backed LocalChunkStore facade. */
export interface ChunkStore {
  get(seq: number, deadline?: Deadline): Promise<StatusOr<NodeFragment>>;
  getByArrivalOrder(order: number, deadline?: Deadline):
    Promise<StatusOr<NodeFragment>>;
  next(deadline?: Deadline, limit?: number):
    Promise<StatusOr<Array<NodeFragment | null>>>;
  put(fragment: NodeFragment): Promise<StatusOr<number>>;
  putMany(fragments: readonly NodeFragment[]): Promise<StatusOr<number[]>>;
  clearData(seq: number): Promise<StatusOr<NodeFragment>>;
  getSeqForArrivalOrder(order: number): Promise<StatusOr<number>>;
  getFinalSeq(): Promise<StatusOr<number | null>>;
  closeWritesWithStatus(status: Status,
    returnStatusIfAlreadyClosed?: boolean): Promise<Status>;
  size(): Promise<StatusOr<number>>;
  getId(): StatusOr<string>;
}

/** Reader buffering and reference traversal options. */
export interface ChunkStoreReaderOptions {
  ordered?: boolean;
  popChunks?: boolean;
  numChunksToBuffer?: number;
  offset?: number;
  maxChunksToRead?: number | null;
  stickyMimetype?: boolean;
  followNodeRefs?: boolean;
  maxRefDepth?: number;
  maxRefBreadth?: number;
}

/** Writer batching and backpressure options. */
export interface ChunkStoreWriterOptions {
  offset?: number;
  maxChunksToWriteAtOnce?: number;
  numChunksToBuffer?: number | null;
  stickyMimetype?: boolean;
}

/** Buffer, concurrency, and lifetime limits for a native session. */
export interface SessionOptions {
  maxBufferedMessagesTotal?: number;
  maxBufferedMessagesPerStream?: number;
  maxConcurrentRootActions?: number;
  maxConcurrentNestedActions?: number;
  maxSingleMessageSize?: number;
  maxBufferedBytesTotal?: number;
  maxBufferedBytesPerStream?: number;
  noStreamTimeoutMs?: number | null;
  deadline?: WireDeadline;
}
