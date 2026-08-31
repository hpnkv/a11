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
  PING_NAME,
  Session,
  StreamMode,
  WebRtcWireStream,
  WebSocketSignallingClient,
  cancelledError,
  isOk,
  unavailableError,
} from '../../src/index.js';

import { ConnectionMonitor, watchPeerConnection } from './connection_monitor.js';
import { electHost } from './election.js';
import { type ChatHost, type OnStateChange } from './host.js';
import { MAX_BACKOFF_MS, MIN_BACKOFF_MS, retry } from './retry.js';
import { deriveState, eventKey } from './room_state.js';
import { claimAnonymous, iceExpiryMs } from './signalling_client.js';
import {
  CONFIRM_TIMEOUT_MS,
  ConnectionType,
  PEER_TIMEOUT_MS,
  PING_INTERVAL_MS,
  PING_TIMEOUT_MS,
  CLAIM_MARGIN_MS,
  RECONNECT_DEADLINE_MS,
  SIGNALLING_POLL_MS,
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

/** Signalling reconnect attempts before the claim is treated as lapsed. */
const HOPELESS_ATTEMPTS = 5;

/**
 * Data channels the peer opens toward the host.
 *
 * The host answers with {@link WebRtcServer}, whose adapter reads one
 * channel. `WebRtcWireStream.createClient` defaults to 8 and stripes
 * packets round-robin across every open channel.
 */
export const PEER_DESIRED_CHANNELS = 1;

/**
 * Callback when this peer wins the election.
 *
 * It carries the log the new host starts from; the caller builds the
 * {@link ChatHost}, so a promoted host is wired up like the original.
 */
export type OnBecomeHost = (events: RoomEvent[]) => void;

/** Callback when connection type changes for a peer. */
export type OnConnectionTypeChange = (
  peerId: string,
  type: ConnectionType,
) => void;

/** Callback when the room's host changes, so the share URL can follow. */
export type OnHostChanged = (hostId: string) => void;

export class ChatPeer {
  private events: RoomEvent[] = [];
  private session: Session | null = null;
  private stream: WebRtcWireStream | null = null;
  private signalling: WebSocketSignallingClient | null = null;
  private connectionMonitor: ConnectionMonitor | null = null;
  private replicating = false;
  private hostId: string;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private pingInFlight = false;
  private hostGone = false;
  private leaving = false;
  private failingOver = false;
  private disconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private signallingTimer: ReturnType<typeof setInterval> | null = null;
  /** Bumped per connection, so watchers of a replaced one stay quiet. */
  private connectionGeneration = 0;
  /** Keys of events already held, so a replayed snapshot is idempotent. */
  private readonly seen = new Set<string>();
  /** Candidates that did not answer as host, skipped by later elections. */
  private readonly unreachable = new Set<string>();

  private readonly registry = new ActionRegistry();
  private readonly onStateChange: OnStateChange;
  private readonly onBecomeHost: OnBecomeHost;
  private readonly onConnectionTypeChange: OnConnectionTypeChange;
  private readonly onHostChanged: OnHostChanged;
  private readonly onIdentityChanged: OnHostChanged;
  private identity: string;
  private claimResult: AnonymousClaimResult | null = null;

  constructor(options: {
    myId: string;
    hostId: string;
    onStateChange: OnStateChange;
    onBecomeHost: OnBecomeHost;
    onConnectionTypeChange: OnConnectionTypeChange;
    onHostChanged?: OnHostChanged;
    onIdentityChanged?: OnHostChanged;
  }) {
    this.identity = options.myId;
    this.hostId = options.hostId;
    this.onStateChange = options.onStateChange;
    this.onBecomeHost = options.onBecomeHost;
    this.onConnectionTypeChange = options.onConnectionTypeChange;
    this.onHostChanged = options.onHostChanged ?? (() => {});
    this.onIdentityChanged = options.onIdentityChanged ?? (() => {});

    // Register schemas client-side (no handlers).
    this.registry.register('send_message', sendMessageSchema);
    this.registry.register('set_name', setNameSchema);
    this.registry.register('replicate', replicateSchema);
    this.registry.register('get_peers', getPeersSchema);
  }

  /** This peer's current anonymous identity. */
  get myId(): string { return this.identity; }

  /** Use `iceServers` for connections made from now on. */
  setIceServers(iceServers: RTCIceServer[]): void {
    if (this.claimResult !== null) {
      this.claimResult = { ...this.claimResult, iceServers };
    }
  }

  /** Return the local event log for failover or UI use. */
  getEventLog(): RoomEvent[] {
    return [...this.events];
  }

  /**
   * Add events not already held and report the change.
   *
   * A reconnect replays the new host's whole log, so the same event arrives
   * twice; identity is the event's type, peer, timestamp and payload.
   */
  private appendEvents(events: RoomEvent[]): void {
    let added = false;
    for (const event of events) {
      const key = eventKey(event);
      if (this.seen.has(key)) continue;
      this.seen.add(key);
      this.events.push(event);
      added = true;
    }
    if (added) this.onStateChange(this.events);
  }

  /** Record that a peer is gone, so it leaves the lists and elections. */
  private recordDeparture(peerId: string): void {
    this.appendEvents([{ type: 'leave', peerId, timestamp: Date.now() }]);
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

    // One socket per connection: `MultiplexedRtcChannel.close` closes the
    // signalling transport it was handed, so a stream that ends takes the
    // socket with it.
    const signalling = await this.openSignalling(claim);
    this.signalling = signalling;

    const webrtcStream = WebRtcWireStream.createClient(
      this.hostId,
      signalling,
      {
        stunServers: [],
        rtcConfiguration: { iceServers: claim.iceServers },
        desiredChannels: PEER_DESIRED_CHANNELS,
      },
      { messageTimeoutMs: PEER_TIMEOUT_MS },
    );
    if (!isOk(webrtcStream)) {
      // Nothing took the socket over, so this closes it.
      signalling.close();
      this.signalling = null;
      throw new Error(`WebRTC stream creation failed: ${JSON.stringify(webrtcStream)}`);
    }
    const session = Session.create({
      actionRegistry: this.registry,
      noStreamTimeoutMs: PEER_TIMEOUT_MS,
    });
    if (!isOk(session)) {
      this.abandon(webrtcStream);
      throw new Error(`Session creation failed: ${JSON.stringify(session)}`);
    }

    const addResult = await session.addStream(webrtcStream, StreamMode.START);
    if (!isOk(addResult)) {
      this.abandon(webrtcStream);
      throw new Error(`Stream start failed: ${JSON.stringify(addResult)}`);
    }

    this.adoptConnection(session, webrtcStream);

    // Start connection monitoring.
    this.connectionMonitor = new ConnectionMonitor(
      this.hostId,
      webrtcStream.peerConnection,
      this.onConnectionTypeChange,
    );
    this.connectionMonitor.start();
  }

  /**
   * Adopt a started session and stream as the connection to the host.
   *
   * The stream ends on the host's abort, on its channel closing, and on
   * the idle timeout; the session completes on an orderly close.
   */
  private adoptConnection(session: Session, stream: WebRtcWireStream): void {
    this.session = session;
    this.stream = stream;
    this.hostGone = false;
    this.startPinging();
    const generation = ++this.connectionGeneration;
    void Promise.race([
      session.done().then(() => 'session ended'),
      stream.wait().then(() => 'stream ended'),
    ]).then((reason) => {
      // A race armed for an earlier connection outlives it; only the
      // current one reports.
      if (generation !== this.connectionGeneration) return;
      void this.handleHostDisconnect(reason);
    });
    this.startSignallingSupervisor();

    const channel = safeDataChannel(stream);
    if (channel !== null) {
      const generation = this.connectionGeneration;
      const check = (): void => {
        if (generation !== this.connectionGeneration) return;
        void this.confirmHostAlive();
      };
      channel.addEventListener('close', check);
      channel.addEventListener('error', check);
    }

    const connection = stream.peerConnection ?? null;
    if (connection !== null) {
      watchPeerConnection(
        connection,
        () => {
          if (generation !== this.connectionGeneration) return;
          void this.handleHostDisconnect('peer connection state');
        },
        (timer) => { this.disconnectTimer = timer; },
        () => this.disconnectTimer,
      );
    }
  }

  /**
   * Ping the host on an interval so neither side goes idle.
   *
   * The `__ping` builtin answers, so the host registers nothing for it.
   * A ping that fails or goes unanswered within {@link PEER_TIMEOUT_MS}
   * counts as the host being gone.
   */
  private startPinging(): void {
    this.stopPinging();
    this.pingTimer = setInterval(() => {
      if (this.pingInFlight) return;
      this.pingInFlight = true;
      void this.pingHost().finally(() => { this.pingInFlight = false; });
    }, PING_INTERVAL_MS);
  }

  /**
   * Re-make the signalling connection when it drops.
   *
   * The data channel carries the chat, but the multiplexed WebRTC channel
   * negotiates a replacement channel over signalling when one is lost, and
   * a peer with a dead socket cannot do that.
   */
  private startSignallingSupervisor(): void {
    this.stopSignallingSupervisor();
    let backoffMs = MIN_BACKOFF_MS;
    let reconnecting = false;
    let failures = 0;
    this.signallingTimer = setInterval(() => {
      const claim = this.claimResult;
      if (this.leaving || reconnecting || claim === null) return;
      if (this.signalling?.isConnected() ?? false) {
        backoffMs = MIN_BACKOFF_MS;
        return;
      }
      reconnecting = true;
      void (async () => {
        await new Promise((resolve) => setTimeout(resolve, backoffMs));
        backoffMs = Math.min(backoffMs * 2, MAX_BACKOFF_MS);
        if (this.leaving) return;
        const signalling = await WebSocketSignallingClient.connect(
          signallingUrlFor(claim), this.identity,
        );
        if (!isOk(signalling)) {
          // The exchange refuses a connection under a lapsed anonymous
          // claim, and there is no renewal, so this stops rather than
          // asking for ever. The data channel in hand keeps working.
          failures++;
          if (failures >= HOPELESS_ATTEMPTS) {
            console.warn(
              'a11 p2p: signalling is gone for good; this peer can talk to '
              + 'its host but cannot move to another one',
            );
            this.stopSignallingSupervisor();
            return;
          }
          console.warn('a11 p2p: signalling reconnect failed:', signalling.message);
          return;
        }
        failures = 0;
        try { this.signalling?.close(); } catch { /* already gone */ }
        this.signalling = signalling;
        backoffMs = MIN_BACKOFF_MS;
      })().finally(() => { reconnecting = false; });
    }, SIGNALLING_POLL_MS);
  }

  private stopSignallingSupervisor(): void {
    if (this.signallingTimer !== null) clearInterval(this.signallingTimer);
    this.signallingTimer = null;
  }

  private stopPinging(): void {
    if (this.pingTimer !== null) clearInterval(this.pingTimer);
    this.pingTimer = null;
    this.pingInFlight = false;
  }

  private async pingHost(): Promise<void> {
    const generation = this.connectionGeneration;
    if (await this.pingOnce(PING_TIMEOUT_MS)) return;
    if (generation !== this.connectionGeneration) return;
    await this.handleHostDisconnect('ping unanswered');
  }

  /** Whether the host answered a ping within `timeoutMs`. */
  private async pingOnce(timeoutMs: number): Promise<boolean> {
    const session = this.session;
    const stream = this.stream;
    if (session === null || stream === null) return false;

    const action = this.registry.makeAction(PING_NAME, {
      nodeMap: session.getNodeMap(),
      stream,
      session,
    });
    if (!isOk(action)) return false;

    if (!isOk(await action.call())) return false;
    const input = await action.getInput('input');
    if (!isOk(input)) return false;
    await input.finalize('ping');
    const output = await action.getOutput('output', false);
    if (!isOk(output)) return false;
    return isOk(await output.consume({ timeoutMs }));
  }

  /**
   * Check the host after the data channel reports trouble.
   *
   * A closed channel is the first sign a host has gone, arriving long
   * before ICE gives up on the connection. The multiplexed channel can also
   * replace a channel that dropped on its own, so this asks the host: one
   * ping, one short deadline.
   */
  private async confirmHostAlive(): Promise<void> {
    if (this.leaving || this.hostGone || this.failingOver) return;
    const generation = this.connectionGeneration;
    if (await this.pingOnce(CONFIRM_TIMEOUT_MS)) return;
    if (generation !== this.connectionGeneration) return;
    await this.handleHostDisconnect('data channel closed');
  }

  /**
   * Join the room, retrying while the host is not yet reachable.
   *
   * A joiner often arrives during the host's own start-up, or just after a
   * failover, when the listener is still binding.
   */
  async join(claim: AnonymousClaimResult): Promise<void> {
    this.claimResult = claim;
    if (await this.attachToHost()) return;
    throw new Error(`Could not reach host ${this.hostId}.`);
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
   * Reads the full log first, then continuously receives live events. A
   * failure here leaves the peer connected and deaf, so it is raised: the
   * caller's retry makes another connection.
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
      throw new Error(`replicate makeAction failed: ${JSON.stringify(action)}`);
    }

    const callResult = await action.call();
    if (!isOk(callResult)) {
      this.replicating = false;
      throw new Error(`replicate call failed: ${JSON.stringify(callResult)}`);
    }

    const dispatchResult = await action.waitForDispatch(10_000);
    if (!isOk(dispatchResult)) {
      this.replicating = false;
      throw new Error(
        `replicate dispatch failed: ${JSON.stringify(dispatchResult)}`,
      );
    }

    const eventsNode = await action.getOutput('events', false);
    if (!isOk(eventsNode)) {
      this.replicating = false;
      throw new Error(`replicate output failed: ${JSON.stringify(eventsNode)}`);
    }

    // Read events continuously in the background.
    void (async () => {
      try {
        for (;;) {
          const event = await eventsNode.next<RoomEvent>();
          if (!isOk(event) || event === null) break;
          this.appendEvents([event]);
        }
      } catch {
        // Stream ended — host disconnected or session closed.
      } finally {
        this.replicating = false;
      }
    })();
  }

  /** Handle host disconnection: elect a new host and transition. */
  private async handleHostDisconnect(reason = 'unknown'): Promise<void> {
    if (this.leaving || this.hostGone || this.failingOver) return;
    this.hostGone = true;
    this.cleanup();
    console.info(`a11 p2p: host ${this.hostId} went away (${reason})`);
    this.recordDeparture(this.hostId);

    // Give every peer the same view before anyone acts on it.
    await new Promise((resolve) => setTimeout(resolve, FAILOVER_DELAY_MS));
    await this.takeOverOrFollow();
  }

  /**
   * Take the room over, or attach to whoever the log elects.
   *
   * Every peer runs the same rule over the same log, so they agree on the
   * winner. A candidate that does not answer within
   * {@link RECONNECT_DEADLINE_MS} is recorded as departed and the election
   * runs again over what is left.
   */
  private async takeOverOrFollow(): Promise<void> {
    if (this.failingOver) return;
    this.failingOver = true;
    try {
      for (;;) {
        if (this.leaving) return;
        const candidates = [...deriveState(this.events).participants.keys()]
          .filter((id) => !this.unreachable.has(id));
        if (candidates.length === 0) return;

        const elected = electHost(candidates);
        console.info(
          `a11 p2p: electing from [${candidates.join(', ')}] -> ${elected}`,
        );
        if (elected === this.identity) {
          this.onBecomeHost(this.getEventLog());
          return;
        }

        this.hostId = elected;
        if (await this.attachToHost()) {
          console.info(`a11 p2p: attached to host ${elected}`);
          this.onHostChanged(elected);
          return;
        }
        console.warn(`a11 p2p: ${elected} never answered; electing again`);
        this.unreachable.add(elected);
        this.recordDeparture(elected);
      }
    } finally {
      this.failingOver = false;
    }
  }

  /**
   * A claim that can still open a signalling connection.
   *
   * The exchange refuses a connection under a lapsed anonymous claim and
   * offers no renewal, so a peer whose claim has run out takes a fresh
   * identity and rejoins under it. The identity it leaves behind is
   * recorded as departed, so no one elects it or waits on it.
   */
  private async usableClaim(): Promise<AnonymousClaimResult> {
    const claim = this.claimResult;
    if (claim !== null) {
      const expiry = iceExpiryMs(claim.iceServers);
      if (expiry === null || expiry - Date.now() > CLAIM_MARGIN_MS) return claim;
    }
    const fresh = await claimAnonymous();
    const previous = this.identity;
    this.identity = fresh.peerId;
    this.claimResult = fresh;
    console.info(`a11 p2p: claim lapsed; rejoining as ${fresh.peerId}`);
    this.recordDeparture(previous);
    this.onIdentityChanged(fresh.peerId);
    return fresh;
  }

  /**
   * Dial the current host until it answers or the deadline passes.
   *
   * A newly elected host binds its listener while its peers are already
   * dialling, so the first attempts arrive before it is listening.
   */
  private async attachToHost(): Promise<boolean> {
    const claim = this.claimResult;
    if (claim === null) {
      console.warn('a11 p2p: no credentials held, cannot reach a host');
      return false;
    }
    try {
      await retry(async () => {
        await this.connectToHost(await this.usableClaim());
        await this.startReplication();
      }, {
        deadlineMs: RECONNECT_DEADLINE_MS,
        cancelled: () => this.leaving,
        onAttemptFailed: (error, attempt) => console.warn(
          `a11 p2p: attempt ${attempt} to reach host ${this.hostId} failed:`,
          error,
        ),
      });
      return true;
    } catch {
      return false;
    }
  }

  /** Clean up the current connection without triggering failover. */
  private cleanup(): void {
    this.replicating = false;
    this.stopPinging();
    this.stopSignallingSupervisor();
    if (this.disconnectTimer !== null) {
      clearTimeout(this.disconnectTimer);
      this.disconnectTimer = null;
    }
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

  /** Drop a half-built connection; aborting takes its socket with it. */
  private abandon(stream: WebRtcWireStream): void {
    try { stream.abort(unavailableError('Connection attempt abandoned.')); }
    catch { /* best effort */ }
    this.signalling = null;
  }

  /**
   * Open a signalling connection for `claim`.
   *
   * `WebSocketSignallingClient.connect` substitutes the identity into the
   * URL path; the claim travels as a query parameter because a browser
   * WebSocket cannot send headers.
   */
  private async openSignalling(
    claim: AnonymousClaimResult,
  ): Promise<WebSocketSignallingClient> {
    const signalling = await WebSocketSignallingClient.connect(
      signallingUrlFor(claim),
      this.identity,
    );
    if (!isOk(signalling)) {
      throw new Error(`Signalling connect failed: ${JSON.stringify(signalling)}`);
    }
    return signalling;
  }

  /**
   * Leave the room and tell the host.
   *
   * The abort marker travels on the open data channel, so the host ends
   * this peer's session and broadcasts the leave event without waiting
   * for a timeout. Safe to call from a `pagehide` handler.
   */
  disconnect(): void {
    this.leaving = true;
    if (this.signalling) {
      try { this.signalling.close(); } catch { /* already closed */ }
      this.signalling = null;
    }
    if (this.session) {
      try {
        this.session.abort(cancelledError('The peer left the room.'));
      } catch { /* ignore */ }
    }
    this.cleanup();
  }

  /** Return the underlying RTCPeerConnection for connection monitoring. */
  getPeerConnection(): RTCPeerConnection | null {
    return this.stream?.peerConnection ?? null;
  }
}

/**
 * The signalling URL for a claim, with the identity placeholder in place.
 *
 * `WebSocketSignallingClient.connect` substitutes `{id}` in the path, which
 * has to happen before the query string; the claim travels as a query
 * parameter because a browser WebSocket cannot send headers.
 */
export function signallingUrlFor(claim: AnonymousClaimResult): string {
  const base = claim.signallingUrl.endsWith('/')
    ? claim.signallingUrl.slice(0, -1) : claim.signallingUrl;
  return `${base}/{id}?claim=${encodeURIComponent(claim.claimToken)}`;
}

/** The stream's primary data channel, or null when it has none yet. */
function safeDataChannel(stream: WebRtcWireStream): RTCDataChannel | null {
  try { return stream.dataChannel ?? null; }
  catch { return null; }
}
