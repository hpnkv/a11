/**
 * Peer client for the P2P chat room.
 *
 * Non-host peers connect to the host via WebRTC, call actions to send
 * messages and set names, and receive the replicated event log through
 * the `replicate` action's streaming output. When the host disconnects,
 * the peer computes the new host via deterministic election and either
 * becomes the new host or reconnects to the elected one.
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
} from '../../src/index.js';

import { ConnectionMonitor } from './connection_monitor.js';
import { shouldBecomeHost, electHost } from './election.js';
import { ChatHost, type OnStateChange } from './host.js';
import { deriveState } from './room_state.js';
import {
  ConnectionType,
  type PeerInfo,
  type RoomEvent,
  type AnonymousClaimResult,
} from './types.js';

// Client-side copies of the host's action schemas (no handlers).
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

/** Delay before the new host starts accepting after failover. */
const FAILOVER_DELAY_MS = 1_000;

/** Callback when the peer transitions to host role. */
export type OnBecomeHost = (host: ChatHost) => void;

/** Callback when connection type changes for a peer. */
export type OnConnectionTypeChange = (
  peerId: string,
  type: ConnectionType,
) => void;

export class ChatPeer {
  private events: RoomEvent[] = [];
  private session: Session | null = null;
  private stream: WebRtcWireStream | null = null;
  private signalling: WebSocketSignallingClient | null = null;
  private connectionMonitor: ConnectionMonitor | null = null;
  private replicating = false;
  private hostId: string;

  private readonly registry = new ActionRegistry();
  private readonly onStateChange: OnStateChange;
  private readonly onBecomeHost: OnBecomeHost;
  private readonly onConnectionTypeChange: OnConnectionTypeChange;
  readonly myId: string;
  private claimResult: AnonymousClaimResult | null = null;

  constructor(options: {
    myId: string;
    hostId: string;
    onStateChange: OnStateChange;
    onBecomeHost: OnBecomeHost;
    onConnectionTypeChange: OnConnectionTypeChange;
  }) {
    this.myId = options.myId;
    this.hostId = options.hostId;
    this.onStateChange = options.onStateChange;
    this.onBecomeHost = options.onBecomeHost;
    this.onConnectionTypeChange = options.onConnectionTypeChange;

    // Register schemas client-side (no handlers).
    this.registry.register('send_message', sendMessageSchema);
    this.registry.register('set_name', setNameSchema);
    this.registry.register('replicate', replicateSchema);
    this.registry.register('get_peers', getPeersSchema);
  }

  /** Return the local event log for failover or UI use. */
  getEventLog(): RoomEvent[] {
    return [...this.events];
  }

  /** Build peer info list from local state. */
  getPeerInfos(): PeerInfo[] {
    const state = deriveState(this.events);
    const result: PeerInfo[] = [];
    for (const [peerId, name] of state.participants) {
      result.push({
        peerId,
        name,
        connectionType: ConnectionType.UNKNOWN,
        isHost: peerId === this.hostId,
      });
    }
    return result;
  }

  /**
   * Connect to the host peer via WebRTC through a11x signalling.
   *
   * The host ID comes from the share URL (client-side), not from a11x.
   * a11x only provides signalling transport — it never learns which
   * room this peer belongs to or who the host is.
   */
  async connectToHost(claim: AnonymousClaimResult): Promise<void> {
    this.claimResult = claim;

    // Build the signalling URL with `{id}` placeholder so that
    // WebSocketSignallingClient.connect() inserts the identity into
    // the path before the query string.  The claim token goes as a
    // query parameter because browser WebSockets cannot send custom
    // HTTP headers.
    const base = claim.signallingUrl.endsWith('/')
      ? claim.signallingUrl.slice(0, -1) : claim.signallingUrl;
    const urlWithClaim = `${base}/{id}?claim=${encodeURIComponent(claim.claimToken)}`;
    const signalling = await WebSocketSignallingClient.connect(
      urlWithClaim,
      this.myId,
    );
    if (!isOk(signalling)) {
      throw new Error(`Signalling connect failed: ${JSON.stringify(signalling)}`);
    }
    this.signalling = signalling;

    const webrtcStream = WebRtcWireStream.createClient(
      this.hostId,
      signalling,
      { stunServers: [], rtcConfiguration: { iceServers: claim.iceServers } },
    );
    if (!isOk(webrtcStream)) {
      signalling.close();
      throw new Error(`WebRTC stream creation failed: ${JSON.stringify(webrtcStream)}`);
    }
    this.stream = webrtcStream;

    const session = Session.create({
      actionRegistry: this.registry,
      noStreamTimeoutMs: 30_000,
    });
    if (!isOk(session)) {
      signalling.close();
      throw new Error(`Session creation failed: ${JSON.stringify(session)}`);
    }
    this.session = session;

    const addResult = await session.addStream(webrtcStream, StreamMode.START);
    if (!isOk(addResult)) {
      signalling.close();
      throw new Error(`Stream start failed: ${JSON.stringify(addResult)}`);
    }

    // Start connection monitoring.
    this.connectionMonitor = new ConnectionMonitor(
      this.hostId,
      webrtcStream.peerConnection,
      this.onConnectionTypeChange,
    );
    this.connectionMonitor.start();

    // Monitor for host disconnection.
    void session.done().then(() => this.handleHostDisconnect());
  }

  /** Send a chat message to the host. */
  async sendMessage(text: string): Promise<void> {
    if (!this.session || !this.stream) {
      throw new Error('Not connected to host.');
    }
    const action = this.registry.makeAction('send_message', {
      nodeMap: this.session.getNodeMap(),
      stream: this.stream,
      session: this.session,
    });
    if (!isOk(action)) throw new Error(`makeAction failed: ${JSON.stringify(action)}`);

    const callResult = await action.call();
    if (!isOk(callResult)) throw new Error(`call failed: ${JSON.stringify(callResult)}`);

    const input = await action.getInput('text');
    if (!isOk(input)) throw new Error(`getInput failed: ${JSON.stringify(input)}`);
    await input.finalize(text);

    const dispatchResult = await action.waitForDispatch(10_000);
    if (!isOk(dispatchResult)) throw new Error(`dispatch failed: ${JSON.stringify(dispatchResult)}`);

    const output = await action.getOutput('status', false);
    if (isOk(output)) {
      await output.consume({ timeoutMs: 10_000 });
    }
    await action.wait(30_000);
  }

  /** Set the display name on the host. */
  async setName(name: string): Promise<void> {
    if (!this.session || !this.stream) {
      throw new Error('Not connected to host.');
    }
    const action = this.registry.makeAction('set_name', {
      nodeMap: this.session.getNodeMap(),
      stream: this.stream,
      session: this.session,
    });
    if (!isOk(action)) throw new Error(`makeAction failed: ${JSON.stringify(action)}`);

    const callResult = await action.call();
    if (!isOk(callResult)) throw new Error(`call failed: ${JSON.stringify(callResult)}`);

    const input = await action.getInput('name');
    if (!isOk(input)) throw new Error(`getInput failed: ${JSON.stringify(input)}`);
    await input.finalize(name);

    const dispatchResult = await action.waitForDispatch(10_000);
    if (!isOk(dispatchResult)) throw new Error(`dispatch failed: ${JSON.stringify(dispatchResult)}`);

    const output = await action.getOutput('status', false);
    if (isOk(output)) {
      await output.consume({ timeoutMs: 10_000 });
    }
    await action.wait(30_000);
  }

  /**
   * Start receiving the replicated event log from the host.
   *
   * Reads the full log first, then continuously receives live events.
   */
  async startReplication(): Promise<void> {
    if (!this.session || !this.stream || this.replicating) return;
    this.replicating = true;

    const action = this.registry.makeAction('replicate', {
      nodeMap: this.session.getNodeMap(),
      stream: this.stream,
      session: this.session,
    });
    if (!isOk(action)) {
      this.replicating = false;
      return;
    }

    const callResult = await action.call();
    if (!isOk(callResult)) {
      this.replicating = false;
      return;
    }

    const dispatchResult = await action.waitForDispatch(10_000);
    if (!isOk(dispatchResult)) {
      this.replicating = false;
      return;
    }

    const eventsNode = await action.getOutput('events', false);
    if (!isOk(eventsNode)) {
      this.replicating = false;
      return;
    }

    // Read events continuously in the background.
    void (async () => {
      try {
        for (;;) {
          const event = await eventsNode.next<RoomEvent>();
          if (!isOk(event) || event === null) break;
          this.events.push(event);
          this.onStateChange(this.events);
        }
      } catch {
        // Stream ended — host disconnected or session closed.
      } finally {
        this.replicating = false;
      }
    })();
  }

  /** Handle host disconnection: elect new host and transition. */
  private async handleHostDisconnect(): Promise<void> {
    this.cleanup();

    // Compute remaining peers from the event log.
    const state = deriveState(this.events);
    const remainingPeerIds = [...state.participants.keys()].filter(
      (id) => id !== this.hostId,
    );

    if (remainingPeerIds.length === 0) return;

    // Wait for peers to converge on the same decision.
    await new Promise((resolve) => setTimeout(resolve, FAILOVER_DELAY_MS));

    if (shouldBecomeHost(this.myId, remainingPeerIds)) {
      // This peer becomes the new host.
      const host = new ChatHost(this.myId, this.events, this.onStateChange);
      this.onBecomeHost(host);
    } else {
      // Connect to the new host.
      const newHostId = electHost(remainingPeerIds);
      this.hostId = newHostId;

      if (this.claimResult) {
        try {
          await this.connectToHost(this.claimResult);
          await this.startReplication();
        } catch (error) {
          console.error('Failed to reconnect to new host:', error);
        }
      }
    }
  }

  /** Clean up the current connection without triggering failover. */
  private cleanup(): void {
    this.replicating = false;
    if (this.connectionMonitor) {
      this.connectionMonitor.stop();
      this.connectionMonitor = null;
    }
    if (this.signalling) {
      try { this.signalling.close(); } catch { /* already closed */ }
      this.signalling = null;
    }
    this.session = null;
    this.stream = null;
  }

  /** Fully disconnect from the room. */
  disconnect(): void {
    if (this.session) {
      try { this.session.halfClose(); } catch { /* ignore */ }
    }
    this.cleanup();
  }

  /** Return the underlying RTCPeerConnection for connection monitoring. */
  getPeerConnection(): RTCPeerConnection | null {
    return this.stream?.peerConnection ?? null;
  }
}
