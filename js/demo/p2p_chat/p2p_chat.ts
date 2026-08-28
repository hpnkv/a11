/**
 * P2P Chat Room — entry point.
 *
 * Orchestrates room lifecycle: checks the URL for a `?host=` parameter
 * (the host's anonymous peer ID), claims an anonymous identity via
 * a11x, and wires the ChatUI to the ChatHost or ChatPeer.
 *
 * Privacy model: a11x only provides anonymous identities, signalling
 * transport, and TURN relay. It never receives room IDs, peer lists,
 * or any data that could correlate peers into rooms. Room coordination
 * is entirely client-side: the share URL encodes the host's peer ID.
 */

import { ChatHost } from './host.js';
import { ChatPeer } from './peer_client.js';
import { deriveState } from './room_state.js';
import { electHost } from './election.js';
import {
  claimAnonymous,
  refreshCredentials,
  CREDENTIAL_REFRESH_MS,
} from './signalling_client.js';
import { ChatUI } from './ui.js';
import {
  ConnectionType,
  MAX_MESSAGE_LENGTH,
  type PeerInfo,
  type RoomEvent,
  type AnonymousClaimResult,
} from './types.js';

// -------------------------------------------------------- state

let ui: ChatUI;
let host: ChatHost | null = null;
let peer: ChatPeer | null = null;
let myId = '';
let myClaim: AnonymousClaimResult | null = null;
let refreshTimer: ReturnType<typeof setInterval> | null = null;
const connectionTypes = new Map<string, ConnectionType>();

// --------------------------------------------------- UI callbacks

function onStateChange(events: RoomEvent[]): void {
  const state = deriveState(events);

  const peerInfos: PeerInfo[] = [];
  const hostId = host ? myId : (peer ? electHost([...state.participants.keys()]) : '');
  for (const [peerId, name] of state.participants) {
    peerInfos.push({
      peerId,
      name,
      connectionType: connectionTypes.get(peerId) ?? ConnectionType.UNKNOWN,
      isHost: peerId === hostId,
    });
  }

  ui.renderMessages(state.messages, state.participants);
  ui.renderParticipants(peerInfos);
}

function onConnectionTypeChange(peerId: string, type: ConnectionType): void {
  connectionTypes.set(peerId, type);
  const events = host?.getEventLog() ?? peer?.getEventLog() ?? [];
  onStateChange(events);
}

async function onSend(text: string): Promise<void> {
  const trimmed = text.slice(0, MAX_MESSAGE_LENGTH);
  if (!trimmed) return;

  try {
    ui.clearError();
    if (host) {
      const event: RoomEvent = {
        type: 'message',
        peerId: myId,
        text: trimmed,
        timestamp: Date.now(),
      };
      host.appendEvent(event);
    } else if (peer) {
      await peer.sendMessage(trimmed);
    }
  } catch (error) {
    ui.showError(error instanceof Error ? error.message : String(error));
  }
}

async function onSetName(name: string): Promise<void> {
  try {
    ui.clearError();
    if (host) {
      const event: RoomEvent = {
        type: 'name_change',
        peerId: myId,
        name: name.slice(0, 100),
        timestamp: Date.now(),
      };
      host.appendEvent(event);
    } else if (peer) {
      await peer.setName(name);
    }
  } catch (error) {
    ui.showError(error instanceof Error ? error.message : String(error));
  }
}

// ---------------------------------------------------- room lifecycle

/**
 * Build the share URL encoding the host's peer ID.
 *
 * The URL uses `?host=<peerId>` — a11x never sees this parameter,
 * it only exists in the URL the user copies and shares.
 */
function getShareUrl(hostPeerId: string): string {
  const url = new URL(window.location.href);
  // Clean any stale parameters.
  url.searchParams.delete('room');
  url.searchParams.set('host', hostPeerId);
  return url.toString();
}

async function createRoom(): Promise<void> {
  try {
    ui.clearError();
    ui.setStatus('Creating room…');

    // Claim an anonymous identity — a11x learns nothing about the room.
    myClaim = await claimAnonymous();
    myId = myClaim.peerId;

    // The share URL encodes our peer ID so joiners can find us.
    const shareUrl = getShareUrl(myId);
    window.history.replaceState(null, '', shareUrl);

    // Become the host immediately.
    host = new ChatHost(myId, [], onStateChange);

    // Add ourselves as a participant.
    host.appendEvent({
      type: 'join',
      peerId: myId,
      timestamp: Date.now(),
    });

    ui.bind({
      onSend,
      onSetName,
      onCreateRoom: createRoom,
      onJoinRoom: joinByHostId,
    }, myId);
    ui.showRoom(myId, shareUrl);
    ui.setStatus('Room created — share the link to invite others');

    startCredentialRefresh();
  } catch (error) {
    ui.showError(error instanceof Error ? error.message : String(error));
    ui.setStatus('Failed to create room');
  }
}

async function joinByHostId(hostPeerId: string): Promise<void> {
  try {
    ui.clearError();
    ui.setStatus('Joining room…');

    // Update the URL without reloading.
    const shareUrl = getShareUrl(hostPeerId);
    window.history.replaceState(null, '', shareUrl);

    // Claim our own anonymous identity — a11x learns nothing about
    // which room we're joining or who the host is.
    myClaim = await claimAnonymous();
    myId = myClaim.peerId;

    // The host ID comes from the URL (client-side), not from a11x.
    peer = new ChatPeer({
      myId,
      hostId: hostPeerId,
      onStateChange,
      onBecomeHost: handleBecomeHost,
      onConnectionTypeChange,
    });

    ui.bind({
      onSend,
      onSetName,
      onCreateRoom: createRoom,
      onJoinRoom: joinByHostId,
    }, myId);
    ui.showRoom(hostPeerId, shareUrl);
    ui.setStatus('Connecting to host…');

    await peer.connectToHost(myClaim);
    await peer.startReplication();

    ui.setStatus('Connected');
    startCredentialRefresh();
  } catch (error) {
    ui.showError(error instanceof Error ? error.message : String(error));
    ui.setStatus('Failed to join room');
  }
}

function handleBecomeHost(newHost: ChatHost): void {
  peer = null;
  host = newHost;
  // Update share URL with new host's ID.
  const shareUrl = getShareUrl(myId);
  window.history.replaceState(null, '', shareUrl);
  ui.setStatus('You are now the host (previous host left)');
}

function startCredentialRefresh(): void {
  if (refreshTimer !== null) clearInterval(refreshTimer);
  refreshTimer = setInterval(async () => {
    try {
      myClaim = await refreshCredentials();
    } catch {
      // Refresh failures are non-fatal; the next attempt will retry.
    }
  }, CREDENTIAL_REFRESH_MS);
}

// -------------------------------------------------------- init

function init(): void {
  const root = document.querySelector('#p2p-chat');
  if (!root) return;

  ui = new ChatUI();

  // Check URL for host parameter (the host's anonymous peer ID).
  const params = new URLSearchParams(window.location.search);
  const hostPeerId = params.get('host');

  if (hostPeerId) {
    void joinByHostId(hostPeerId);
  } else {
    ui.bind({
      onSend,
      onSetName,
      onCreateRoom: createRoom,
      onJoinRoom: joinByHostId,
    }, '');
    ui.showLobby();
  }
}

init();
