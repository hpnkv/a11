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

/**
 * Deterministic host election for the P2P chat room.
 *
 * The peer with the lexicographically smallest ID among remaining
 * participants becomes the new host. Every peer computes the same
 * answer from its participant list, so no negotiation is needed.
 */

import { electHost } from './room_state.js';

/** Return whether {@link myId} should become the host given the peer set. */
export function shouldBecomeHost(
  myId: string,
  peerIds: string[],
): boolean {
  return electHost(peerIds) === myId;
}

export { electHost };
