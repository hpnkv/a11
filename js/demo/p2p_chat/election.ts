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
