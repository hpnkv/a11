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

import {Chunk, ChunkMetadata} from './data.js';

export function cloneMetadata(metadata: ChunkMetadata | null):
    ChunkMetadata | null {
  return metadata === null ? null : new ChunkMetadata({
    mimetype: metadata.mimetype,
    timestamp: metadata.timestamp,
    attributes: metadata.attributes,
  });
}

/** Copy a chunk and its mutable metadata and bytes. */
export function cloneChunk(chunk: Chunk): Chunk {
  return new Chunk({metadata: cloneMetadata(chunk.metadata),
    ref: chunk.ref, data: chunk.data});
}
