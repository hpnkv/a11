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

import { retry } from './retry.js';
import { ICE_REFRESH_MARGIN_MS, type AnonymousClaimResult } from './types.js';

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
export async function claimAnonymous(
  options: { deadlineMs?: number } = {},
): Promise<AnonymousClaimResult> {
  return retry(async () => {
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
  }, { deadlineMs: options.deadlineMs ?? 20_000 });
}

/**
 * Fetch replacement TURN credentials, keeping the caller's identity.
 *
 * The anonymous endpoint mints a fresh identity on every call and offers no
 * renewal, so this claims one and keeps only its `ice_servers`. TURN
 * credentials are bearer credentials -- the relay checks the HMAC in
 * `credential` against the expiry in `username` and knows nothing about a11x
 * identities -- so they serve the identity already in use, whose signalling
 * connection and share URL stay as they are.
 */
export async function refreshIceServers(): Promise<RTCIceServer[]> {
  const claim = await claimAnonymous();
  return claim.iceServers;
}

/**
 * When the TURN credentials in `iceServers` stop working, in epoch ms.
 *
 * The exchange issues `username` as `<expiry-epoch-seconds>:<id>`. Returns
 * null when no server carries one.
 */
export function iceExpiryMs(iceServers: RTCIceServer[]): number | null {
  let earliest: number | null = null;
  for (const server of iceServers) {
    const username = typeof server.username === 'string' ? server.username : '';
    const seconds = Number.parseInt(username.split(':')[0] ?? '', 10);
    if (!Number.isFinite(seconds) || seconds <= 0) continue;
    const at = seconds * 1_000;
    if (earliest === null || at < earliest) earliest = at;
  }
  return earliest;
}

/** How long to wait before replacing the credentials in `iceServers`. */
export function refreshDelayMs(iceServers: RTCIceServer[]): number {
  const expiry = iceExpiryMs(iceServers);
  if (expiry === null) return ICE_REFRESH_MARGIN_MS;
  return Math.max(5_000, expiry - Date.now() - ICE_REFRESH_MARGIN_MS);
}
