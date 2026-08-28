/**
 * Anonymous claim client for the P2P chat room.
 *
 * Calls `POST /v1/anonymous/claim` on a11x to obtain an anonymous
 * identity with signalling URL, claim token, and ICE servers (including
 * time-limited TURN credentials). The request body carries no room
 * identifier, no peer list, and no information that could let a11x
 * correlate this identity with a particular room or set of peers.
 *
 * Room coordination is entirely client-side: the share URL encodes the
 * host's peer ID so joiners know who to WebRTC-connect to.
 */

import { type AnonymousClaimResult } from './types.js';

/** Base URL for the a11x exchange control plane. */
const EXCHANGE_BASE = 'https://a11.services';

/**
 * Claim an anonymous identity with signalling and TURN credentials.
 *
 * The exchange:
 * - Generates a random anonymous identity (not tied to any room).
 * - Issues a short-lived claim (10 min) with TURN credentials.
 * - Returns signalling URL and ICE servers.
 * - Does NOT receive or store any room, peer-list, or correlation data.
 * - Rejects the identity if it collides with a registered user.
 */
export async function claimAnonymous(): Promise<AnonymousClaimResult> {
  const url = `${EXCHANGE_BASE}/v1/anonymous/claim`;
  const response = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    // Empty body: no room ID, no peer list, no correlating data.
    body: '{}',
  });
  if (!response.ok) {
    const body = await response.text().catch(() => '');
    throw new Error(
      `Anonymous claim failed (${response.status}): ${body || response.statusText}`,
    );
  }
  const data = await response.json();
  return {
    peerId: data.peer_id,
    signallingUrl: data.signalling_url,
    claimToken: data.claim_token,
    iceServers: data.ice_servers ?? [],
  };
}

/**
 * Refresh credentials by claiming a new anonymous identity.
 *
 * The old identity expires; the caller gets a fresh one with new TURN
 * credentials. The caller is responsible for re-announcing their new
 * identity to the room (updating the host's participant tracking).
 */
export async function refreshCredentials(): Promise<AnonymousClaimResult> {
  return claimAnonymous();
}

/** Credential refresh interval: 8 minutes (well before 10-min expiry). */
export const CREDENTIAL_REFRESH_MS = 8 * 60 * 1000;
