/**
 * Host service for the P2P chat room.
 *
 * The host peer runs an A11 ActionRegistry with handlers for
 * `send_message`, `set_name`, `replicate`, and `get_peers`. Incoming
 * WebRTC connections from other peers are accepted as sessions. The
 * event log is the single source of truth; each connected peer
 * receives a streaming copy via the `replicate` action.
 */

import {
  ActionPortSchema,
  ActionRegistry,
  ActionSchema,
  Session,
  StreamMode,
  WebRtcWireStream,
  WebSocketSignallingClient,
  isOk,
  type Action,
  type AsyncNode,
  type WireStream,
} from '../../src/index.js';

import { deriveState, pruneEvents } from './room_state.js';
import {
  ConnectionType,
  MAX_MESSAGE_HISTORY,
  MAX_MESSAGE_LENGTH,
  type PeerInfo,
  type RoomEvent,
} from './types.js';

// ------------------------------------------------------------------ schemas

const sendMessageSchema = new ActionSchema({
  name: 'send_message',
  description: 'Send a chat message to the room.',
  inputs: {
    text: new ActionPortSchema({
      name: 'text',
      type: 'text/plain',
      required: true,
      unary: true,
    }),
  },
  outputs: {
    status: new ActionPortSchema({
      name: 'status',
      type: 'application/json',
      required: true,
      unary: true,
    }),
  },
});

const setNameSchema = new ActionSchema({
  name: 'set_name',
  description: 'Set the display name of the calling peer.',
  inputs: {
    name: new ActionPortSchema({
      name: 'name',
      type: 'text/plain',
      required: true,
      unary: true,
    }),
  },
  outputs: {
    status: new ActionPortSchema({
      name: 'status',
      type: 'application/json',
      required: true,
      unary: true,
    }),
  },
});

const replicateSchema = new ActionSchema({
  name: 'replicate',
  description: 'Stream the full event log then live events.',
  inputs: {},
  outputs: {
    events: new ActionPortSchema({
      name: 'events',
      type: 'application/json',
      required: true,
    }),
  },
});

const getPeersSchema = new ActionSchema({
  name: 'get_peers',
  description: 'List current room participants.',
  inputs: {},
  outputs: {
    peers: new ActionPortSchema({
      name: 'peers',
      type: 'application/json',
      required: true,
      unary: true,
    }),
  },
});

// ------------------------------------------------------- peer bookkeeping

interface ConnectedPeer {
  peerId: string;
  session: Session;
  stream: WireStream;
  replicationNode: AsyncNode | null;
  signalling: WebSocketSignallingClient;
}

/** Callback for state changes the UI should reflect. */
export type OnStateChange = (events: RoomEvent[]) => void;

// ----------------------------------------------------------- ChatHost

export class ChatHost {
  private readonly registry = new ActionRegistry();
  private readonly peers = new Map<string, ConnectedPeer>();
  private events: RoomEvent[];
  private readonly onStateChange: OnStateChange;
  private readonly myId: string;
  private readonly connectionTypes = new Map<string, ConnectionType>();

  constructor(
    myId: string,
    initialEvents: RoomEvent[],
    onStateChange: OnStateChange,
  ) {
    this.myId = myId;
    this.events = [...initialEvents];
    this.onStateChange = onStateChange;
    this.registerActions();
  }

  /** Return a snapshot of the event log for failover handoff. */
  getEventLog(): RoomEvent[] {
    return [...this.events];
  }

  /** Update a peer's observed connection type (from ConnectionMonitor). */
  setConnectionType(peerId: string, type: ConnectionType): void {
    this.connectionTypes.set(peerId, type);
  }

  /** Build peer info list for the UI or get_peers action. */
  getPeerInfos(): PeerInfo[] {
    const state = deriveState(this.events);
    const hostId = this.myId;
    const result: PeerInfo[] = [];
    for (const [peerId, name] of state.participants) {
      result.push({
        peerId,
        name,
        connectionType: this.connectionTypes.get(peerId) ?? ConnectionType.UNKNOWN,
        isHost: peerId === hostId,
      });
    }
    return result;
  }

  // -------------------------------------------------------- actions

  private registerActions(): void {
    const need = (s: ReturnType<ActionRegistry['register']>) => {
      if (!isOk(s)) throw new Error(`Action registration failed: ${s.message}`);
    };

    need(this.registry.register('send_message', sendMessageSchema, (action) =>
      this.handleSendMessage(action),
    ));
    need(this.registry.register('set_name', setNameSchema, (action) =>
      this.handleSetName(action),
    ));
    need(this.registry.register('replicate', replicateSchema, (action) =>
      this.handleReplicate(action),
    ));
    need(this.registry.register('get_peers', getPeersSchema, (action) =>
      this.handleGetPeers(action),
    ));
  }

  private peerIdForAction(action: Action): string | null {
    // Find which connected peer owns this action's session.
    const actionSession = action.getSession();
    for (const [peerId, peer] of this.peers) {
      if (peer.session === actionSession) {
        return peerId;
      }
    }
    return null;
  }

  private async handleSendMessage(action: Action): Promise<void> {
    const inputNode = await action.getInput('text');
    if (!isOk(inputNode)) return;
    const textResult = await inputNode.consume<string>({ timeoutMs: 5_000 });
    if (!isOk(textResult) || textResult === null) {
      const out = await action.getOutput('status');
      if (isOk(out)) await out.finalize({ error: 'invalid input' });
      return;
    }

    const trimmed = String(textResult).slice(0, MAX_MESSAGE_LENGTH);
    const peerId = this.peerIdForAction(action) ?? 'unknown';

    const event: RoomEvent = {
      type: 'message',
      peerId,
      text: trimmed,
      timestamp: Date.now(),
    };
    this.appendEvent(event);

    const out = await action.getOutput('status');
    if (isOk(out)) await out.finalize({ ok: true });
  }

  private async handleSetName(action: Action): Promise<void> {
    const inputNode = await action.getInput('name');
    if (!isOk(inputNode)) return;
    const nameResult = await inputNode.consume<string>({ timeoutMs: 5_000 });
    if (!isOk(nameResult) || nameResult === null) {
      const out = await action.getOutput('status');
      if (isOk(out)) await out.finalize({ error: 'invalid input' });
      return;
    }

    const peerId = this.peerIdForAction(action) ?? 'unknown';
    const event: RoomEvent = {
      type: 'name_change',
      peerId,
      name: String(nameResult).slice(0, 100),
      timestamp: Date.now(),
    };
    this.appendEvent(event);

    const out = await action.getOutput('status');
    if (isOk(out)) await out.finalize({ ok: true });
  }

  private async handleReplicate(action: Action): Promise<void> {
    const eventsNode = await action.getOutput('events');
    if (!isOk(eventsNode)) return;

    const peerId = this.peerIdForAction(action);

    // Send the full event log as the initial batch.
    for (const event of this.events) {
      await eventsNode.put(event);
    }

    // Register this node for live broadcasts. The stream stays open
    // until the peer disconnects.
    if (peerId !== null) {
      const peer = this.peers.get(peerId);
      if (peer) peer.replicationNode = eventsNode;
    }

    // Keep the action alive by never finalizing — the node stays
    // open for live pushes. The session closing will clean it up.
  }

  private async handleGetPeers(action: Action): Promise<void> {
    const out = await action.getOutput('peers');
    if (!isOk(out)) return;
    await out.finalize(this.getPeerInfos());
  }

  // ------------------------------------------------- event management

  /** Append an event, broadcast to peers, and notify the UI. */
  appendEvent(event: RoomEvent): void {
    this.events.push(event);
    this.events = pruneEvents(this.events, MAX_MESSAGE_HISTORY);
    this.broadcastEvent(event);
    this.onStateChange(this.events);
  }

  private broadcastEvent(event: RoomEvent): void {
    for (const peer of this.peers.values()) {
      if (peer.replicationNode !== null) {
        void peer.replicationNode.put(event).catch(() => {
          // Peer may have disconnected; ignore write failures.
        });
      }
    }
  }

  // ----------------------------------------------- peer connections

  /**
   * Accept an incoming WebRTC peer connection.
   *
   * Uses the host's own anonymous claim for signalling. a11x never
   * learns which room this connection belongs to — it only routes
   * signalling messages between two anonymous identities.
   */
  async acceptPeer(
    peerId: string,
    hostSignallingUrl: string,
    hostClaimToken: string,
    iceServers: RTCIceServer[],
  ): Promise<void> {
    // Build the signalling URL with `{id}` placeholder so that
    // WebSocketSignallingClient.connect() inserts the identity into
    // the path before the query string.  The claim token goes as a
    // query parameter because browser WebSockets cannot send custom
    // HTTP headers.
    const base = hostSignallingUrl.endsWith('/')
      ? hostSignallingUrl.slice(0, -1) : hostSignallingUrl;
    const urlWithClaim = `${base}/{id}?claim=${encodeURIComponent(hostClaimToken)}`;
    const signalling = await WebSocketSignallingClient.connect(
      urlWithClaim,
      this.myId,
    );
    if (!isOk(signalling)) {
      console.error(`Signalling connect failed for peer ${peerId}:`, signalling);
      return;
    }

    // Create a WebRTC wire stream to this peer.
    const webrtcStream = WebRtcWireStream.createClient(
      peerId,
      signalling,
      { stunServers: [], rtcConfiguration: { iceServers } },
    );
    if (!isOk(webrtcStream)) {
      console.error(`WebRTC stream creation failed for peer ${peerId}:`, webrtcStream);
      signalling.close();
      return;
    }

    // Create a session that accepts the incoming stream.
    const session = Session.create({
      actionRegistry: this.registry,
      noStreamTimeoutMs: 30_000,
    });
    if (!isOk(session)) {
      console.error(`Session creation failed for peer ${peerId}:`, session);
      signalling.close();
      return;
    }

    const addResult = await session.addStream(webrtcStream, StreamMode.ACCEPT);
    if (!isOk(addResult)) {
      console.error(`Stream accept failed for peer ${peerId}:`, addResult);
      signalling.close();
      return;
    }

    const connectedPeer: ConnectedPeer = {
      peerId,
      session,
      stream: webrtcStream,
      replicationNode: null,
      signalling,
    };
    this.peers.set(peerId, connectedPeer);

    // Add a join event.
    this.appendEvent({
      type: 'join',
      peerId,
      timestamp: Date.now(),
    });

    // Monitor for disconnection.
    void session.done().then(() => this.removePeer(peerId));
  }

  /** Remove a peer and broadcast the leave event. */
  removePeer(peerId: string): void {
    const peer = this.peers.get(peerId);
    if (!peer) return;

    this.peers.delete(peerId);
    this.connectionTypes.delete(peerId);

    try {
      peer.signalling.close();
    } catch {
      // Already closed.
    }

    this.appendEvent({
      type: 'leave',
      peerId,
      timestamp: Date.now(),
    });
  }

  /** Shut down all peer sessions and the host service. */
  shutdown(): void {
    for (const peer of this.peers.values()) {
      try {
        peer.session.halfClose();
        peer.signalling.close();
      } catch {
        // Best effort cleanup.
      }
    }
    this.peers.clear();
  }

  /** Return the underlying RTCPeerConnection for a connected peer. */
  getPeerConnection(peerId: string): RTCPeerConnection | null {
    const peer = this.peers.get(peerId);
    if (!peer) return null;
    const stream = peer.stream;
    if (stream instanceof WebRtcWireStream) {
      return stream.peerConnection;
    }
    return null;
  }
}
