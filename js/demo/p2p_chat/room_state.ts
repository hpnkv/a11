/**
 * Pure state derivation from the replicated event log.
 *
 * Every peer holds a local copy of the event log and derives the full
 * room state from it. These functions are side-effect-free and
 * deterministic so that all peers agree on the same state.
 */

import {
  MAX_MESSAGE_HISTORY,
  type ChatMessage,
  type RoomEvent,
  type RoomState,
} from './types.js';

/**
 * Derive the current room state by replaying the event log.
 *
 * Participants are those who have joined but not yet left. Messages
 * are extracted in order. Name changes update the participant map.
 */
export function deriveState(events: RoomEvent[]): RoomState {
  const participants = new Map<string, string | null>();
  const messages: ChatMessage[] = [];

  for (const event of events) {
    switch (event.type) {
      case 'join':
        participants.set(event.peerId, null);
        break;
      case 'leave':
        participants.delete(event.peerId);
        break;
      case 'message':
        messages.push({
          peerId: event.peerId,
          text: event.text,
          timestamp: event.timestamp,
        });
        break;
      case 'name_change':
        if (participants.has(event.peerId)) {
          participants.set(event.peerId, event.name);
        }
        break;
    }
  }

  return { participants, messages };
}

/**
 * Prune the event log so that at most {@link maxMessages} message events
 * are retained. Join, leave, and name_change events for currently active
 * participants are always kept; older structural events beyond a
 * reasonable window are dropped.
 *
 * Returns a new array; the input is not mutated.
 */
export function pruneEvents(
  events: RoomEvent[],
  maxMessages: number = MAX_MESSAGE_HISTORY,
): RoomEvent[] {
  // Identify active participants from the full log.
  const active = new Set<string>();
  for (const event of events) {
    if (event.type === 'join') active.add(event.peerId);
    else if (event.type === 'leave') active.delete(event.peerId);
  }

  // Count message events.
  let messageCount = 0;
  for (const event of events) {
    if (event.type === 'message') messageCount++;
  }
  const messagesToDrop = Math.max(0, messageCount - maxMessages);

  // Build pruned log, keeping structural events for active peers and
  // dropping the oldest excess messages.
  const result: RoomEvent[] = [];
  let messagesDropped = 0;
  for (const event of events) {
    if (event.type === 'message') {
      if (messagesDropped < messagesToDrop) {
        messagesDropped++;
        continue;
      }
      result.push(event);
    } else if (
      event.type === 'join' || event.type === 'name_change'
    ) {
      // Keep join/name events for still-active participants.
      if (active.has(event.peerId)) {
        result.push(event);
      }
    } else {
      // Leave events for still-active peers should not exist, but
      // keep any leave event for consistency.
      result.push(event);
    }
  }
  return result;
}

/**
 * Elect the host: the peer with the lexicographically smallest ID.
 *
 * Returns the empty string if the list is empty.
 */
export function electHost(peerIds: string[]): string {
  if (peerIds.length === 0) return '';
  let lowest = peerIds[0]!;
  for (let i = 1; i < peerIds.length; i++) {
    if (peerIds[i]! < lowest) lowest = peerIds[i]!;
  }
  return lowest;
}

/** Identity of an event, so a replayed log is idempotent. */
export function eventKey(event: RoomEvent): string {
  switch (event.type) {
    case 'message':
      return `m:${event.peerId}:${event.timestamp}:${event.text}`;
    case 'name_change':
      return `n:${event.peerId}:${event.timestamp}:${event.name}`;
    default:
      return `${event.type}:${event.peerId}:${event.timestamp}`;
  }
}
