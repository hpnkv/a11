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
  cancelledError,
  isOk,
  type Action,
  type AsyncNode,
  type WireStream,
} from '../../src/index.js';

import { WebRtcServer } from './webrtc_server.js';

import { deriveState, pruneEvents } from './room_state.js';
import {
  ConnectionType,
  MAX_MESSAGE_HISTORY,
  MAX_MESSAGE_LENGTH,
  PEER_TIMEOUT_MS,
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
  /** Serializes writes so events reach the peer in log order. */
  replicationWrites: Promise<void>;
  /** Resolves when the peer leaves; the `replicate` handler awaits it. */
  replicationDone: Promise<void>;
  /** Resolver for {@link replicationDone}. */
  endReplication: () => void;
  peerConnection: RTCPeerConnection | null;
}

/** Callback for state changes the UI should reflect. */
export type OnStateChange = (events: RoomEvent[]) => void;

/** Resolve once {@link signal} aborts. */
function aborted(signal: AbortSignal): Promise<void> {
  if (signal.aborted) return Promise.resolve();
  return new Promise((resolve) => {
    signal.addEventListener('abort', () => resolve(), { once: true });
  });
}

// ----------------------------------------------------------- ChatHost

export class ChatHost {
  private readonly registry = new ActionRegistry();
  private readonly peers = new Map<string, ConnectedPeer>();
  private events: RoomEvent[];
  private readonly onStateChange: OnStateChange;
  private readonly myId: string;
  private readonly connectionTypes = new Map<string, ConnectionType>();
  private server: WebRtcServer | null = null;

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
    const peer = peerId === null ? undefined : this.peers.get(peerId);
    if (peer === undefined) {
      // A caller with no peer record receives the log as it stands.
      for (const event of this.events) await eventsNode.put(event);
      return;
    }

    // Registration and the snapshot are taken in the same turn, so each
    // event is carried either by the snapshot or by a later broadcast.
    peer.replicationNode = eventsNode;
    this.queueReplication(peer, [...this.events]);

    // Returning closes the `events` output, so the handler stays on the
    // peer's lifetime and unwinds on cancellation.
    await Promise.race([peer.replicationDone, aborted(action.signal)]);
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
      this.queueReplication(peer, [event]);
    }
  }

  /**
   * Append events to one peer's replication stream.
   *
   * Writes run on a per-peer chain because `AsyncNode.put` serializes
   * the value before it enqueues, and overlapping calls can enqueue out
   * of order. A disconnected peer fails its writes; its session
   * completion removes it.
   */
  private queueReplication(peer: ConnectedPeer, events: RoomEvent[]): void {
    const node = peer.replicationNode;
    if (node === null) return;
    peer.replicationWrites = peer.replicationWrites
      .then(async () => {
        for (const event of events) await node.put(event);
      })
      .catch(() => {
        // Write failures end with the peer's session.
      });
  }

  // ----------------------------------------------- peer connections

  /**
   * Start listening for incoming WebRTC peer connections.
   *
   * Opens a single signalling WebSocket as the host identity and
   * uses {@link WebRtcServer} to answer incoming offers. a11x never
   * learns which room this connection belongs to — it only routes
   * signalling messages between two anonymous identities.
   */
  async startListening(
    signallingUrl: string,
    claimToken: string,
    iceServers: RTCIceServer[],
  ): Promise<void> {
    const base = signallingUrl.endsWith('/')
      ? signallingUrl.slice(0, -1) : signallingUrl;
    const urlWithClaim = `${base}/{id}?claim=${encodeURIComponent(claimToken)}`;

    const server = await WebRtcServer.create(
      urlWithClaim,
      this.myId,
      iceServers,
      (peerId, wireStream, peerConnection) =>
        this.onPeerConnected(peerId, wireStream, peerConnection),
      (peerId, error) =>
        console.error(`WebRTC server error for ${peerId}:`, error),
      { messageTimeoutMs: PEER_TIMEOUT_MS },
    );
    if (!isOk(server)) {
      console.error('Failed to start WebRTC server:', server);
      return;
    }
    this.server = server;
  }

  /**
   * Handle a new peer whose WebRTC data channel is ready.
   *
   * Called by the {@link WebRtcServer} when an incoming peer's data
   * channel opens and is wrapped as a {@link ChannelWireStream}.
   */
  private async onPeerConnected(
    peerId: string,
    wireStream: WireStream,
    peerConnection: RTCPeerConnection,
  ): Promise<void> {
    const session = Session.create({
      actionRegistry: this.registry,
      noStreamTimeoutMs: PEER_TIMEOUT_MS,
    });
    if (!isOk(session)) {
      console.error(`Session creation failed for peer ${peerId}:`, session);
      return;
    }

    const addResult = await session.addStream(wireStream, StreamMode.ACCEPT);
    if (!isOk(addResult)) {
      console.error(`Stream accept failed for peer ${peerId}:`, addResult);
      return;
    }

    let endReplication = (): void => {};
    const replicationDone = new Promise<void>((resolve) => {
      endReplication = () => resolve();
    });
    const connectedPeer: ConnectedPeer = {
      peerId,
      session,
      stream: wireStream,
      replicationNode: null,
      replicationWrites: Promise.resolve(),
      replicationDone,
      endReplication,
      peerConnection,
    };
    this.peers.set(peerId, connectedPeer);

    this.appendEvent({
      type: 'join',
      peerId,
      timestamp: Date.now(),
    });

    // The stream ends on a peer's abort, on its channel closing, and on
    // the idle timeout; the session completes on an orderly close.
    void Promise.race([session.done(), wireStream.wait()])
      .then(() => this.removePeer(peerId));
  }

  /** Remove a peer and broadcast the leave event. */
  removePeer(peerId: string): void {
    const peer = this.peers.get(peerId);
    if (!peer) return;

    this.peers.delete(peerId);
    this.connectionTypes.delete(peerId);
    peer.endReplication();

    this.appendEvent({
      type: 'leave',
      peerId,
      timestamp: Date.now(),
    });
  }

  /** Shut down all peer sessions and the host service. */
  shutdown(): void {
    if (this.server) {
      this.server.close();
      this.server = null;
    }
    for (const peer of this.peers.values()) {
      peer.endReplication();
      try {
        // An abort carries its status to the peer, which starts electing a
        // new host as soon as the marker arrives.
        peer.session.abort(cancelledError('The host left the room.'));
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
    return peer.peerConnection;
  }
}
