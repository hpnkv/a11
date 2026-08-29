/**
 * WebRTC server (answerer) for the P2P chat host.
 *
 * The A11 JS library only provides a WebRTC client (offer) role via
 * {@link WebRtcWireStream.createClient}. The host peer needs the
 * opposite: it must *answer* incoming offers from joining peers.
 *
 * This module implements the answerer side entirely in user-space,
 * reusing the library's {@link BinaryChannel} / {@link ChannelWireStream}
 * adapter seam so the resulting stream plugs directly into
 * {@link Session.addStream}.
 *
 * Signalling flow:
 * 1. The host opens one signalling WebSocket as its identity.
 * 2. A joining peer sends an SDP offer (DESCRIPTION type "offer").
 * 3. This module creates an RTCPeerConnection, sets the remote
 *    description, creates an answer, and sends it back.
 * 4. ICE candidates are exchanged through the same signalling path.
 * 5. The peer's data channel arrives via `ondatachannel`.
 * 6. The data channel is wrapped as a {@link BinaryChannel} and fed
 *    to {@link ChannelWireStream.create} with SERVER role.
 */

import {
  ChannelEndpointRole,
  ChannelWireStream,
  SignallingMessage,
  SignallingMessageType,
  WebSocketSignallingClient,
  isOk,
  okStatus,
  failedPreconditionError,
  invalidArgumentError,
  statusFromUnknown,
  unavailableError,
  randomId,
  type BinaryChannel,
  type BinaryChannelCallbacks,
  type SignallingTransport,
  type Status,
  type StatusOr,
  type NonOkStatus,
  type WireStream,
} from '../../src/index.js';

// -------------------------------------------- sequence framing

/**
 * 8-byte little-endian sequence suffix matching
 * {@link MultiplexedRtcChannel}'s wire format.
 *
 * The client side wraps every packet with a monotonic sequence
 * number.  The server adapter must strip it on receive and add it
 * on send so the two sides stay byte-compatible.
 */
const SEQUENCE_SUFFIX = 8;

function encodeSequence(sequence: number, payload: Uint8Array): Uint8Array {
  const framed = new Uint8Array(payload.byteLength + SEQUENCE_SUFFIX);
  framed.set(payload, 0);
  const view = new DataView(framed.buffer);
  view.setUint32(payload.byteLength, sequence >>> 0, true);
  view.setUint32(payload.byteLength + 4, Math.floor(sequence / 0x100000000), true);
  return framed;
}

function decodeSequence(framed: Uint8Array): Uint8Array {
  return framed.subarray(0, framed.byteLength - SEQUENCE_SUFFIX);
}

// -------------------------------------------- single-channel adapter

/**
 * Wraps one {@link RTCDataChannel} as a {@link BinaryChannel}.
 *
 * This is a simplified, single-channel version of the library's
 * internal {@link MultiplexedRtcChannel}.  It adds and strips the
 * 8-byte sequence suffix that the client's multiplexed channel
 * uses, so the two sides stay wire-compatible.
 */
class SingleDataChannelAdapter implements BinaryChannel {
  private callbacks: BinaryChannelCallbacks | null = null;
  private closed = false;
  private failure: NonOkStatus | null = null;
  private openResolve: ((status: Status) => void) | null = null;
  private nextSendSequence = 0;

  constructor(
    private readonly dataChannel: RTCDataChannel,
    private readonly connection: RTCPeerConnection,
  ) {
    dataChannel.binaryType = 'arraybuffer';
    dataChannel.bufferedAmountLowThreshold = 0;

    dataChannel.addEventListener('open', () => {
      try {
        this.callbacks?.onOpen();
        this.openResolve?.(okStatus());
        this.openResolve = null;
      } catch (error) {
        this.fail(statusFromUnknown(error, 'Data channel open callback raised.'));
      }
    });

    dataChannel.addEventListener('message', (event) => {
      void this.handleMessage(event.data);
    });

    dataChannel.addEventListener('error', () => {
      this.fail(unavailableError('WebRTC data channel reported an error.'));
    });

    dataChannel.addEventListener('close', () => {
      if (this.closed) return;
      this.closed = true;
      try { this.callbacks?.onClosed(); } catch { /* terminal */ }
    });

    dataChannel.addEventListener('bufferedamountlow', () => {
      try { this.callbacks?.onBufferedAmountLow(); } catch { /* observable */ }
    });
  }

  setCallbacks(callbacks: BinaryChannelCallbacks): Status {
    this.callbacks = callbacks;
    return okStatus();
  }

  resetCallbacks(): Status {
    this.callbacks = null;
    return okStatus();
  }

  async open(): Promise<Status> {
    if (this.failure !== null) return this.failure;
    if (this.dataChannel.readyState === 'open') return okStatus();
    return new Promise<Status>((resolve) => {
      this.openResolve = resolve;
      // If the channel closes or errors before opening, resolve with failure.
      const timeout = setTimeout(() => {
        if (this.openResolve) {
          this.openResolve = null;
          resolve(unavailableError('Data channel did not open within 20 seconds.'));
        }
      }, 20_000);
      const origResolve = this.openResolve;
      this.openResolve = (status: Status) => {
        clearTimeout(timeout);
        origResolve?.(status);
      };
    });
  }

  isOpen(): boolean {
    try { return !this.closed && this.dataChannel.readyState === 'open'; }
    catch { return false; }
  }

  send(packet: Uint8Array): Status {
    try {
      if (!this.isOpen()) {
        return this.failure ?? failedPreconditionError('Data channel is not open.');
      }
      const framed = encodeSequence(this.nextSendSequence++, packet);
      this.dataChannel.send(framed.slice().buffer);
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Data channel send raised an exception.');
    }
  }

  bufferedAmount(): StatusOr<number> {
    try {
      const amount = this.dataChannel.bufferedAmount;
      return typeof amount === 'number' && Number.isFinite(amount) && amount >= 0
        ? amount
        : invalidArgumentError('bufferedAmount returned an invalid value.');
    } catch (error) {
      return statusFromUnknown(error, 'Reading buffered amount failed.');
    }
  }

  async waitForBufferedAmountLow(): Promise<Status> {
    if (!this.isOpen()) {
      return this.failure ?? failedPreconditionError('Data channel is not open.');
    }
    const amount = this.bufferedAmount();
    if (!isOk(amount)) return amount;
    if (amount === 0) return okStatus();
    return new Promise<Status>((resolve) => {
      const handler = () => {
        this.dataChannel.removeEventListener('bufferedamountlow', handler);
        resolve(okStatus());
      };
      this.dataChannel.addEventListener('bufferedamountlow', handler);
    });
  }

  close(): Status {
    if (this.closed) return okStatus();
    this.closed = true;
    try { this.dataChannel.close(); } catch { /* best effort */ }
    try { this.connection.close(); } catch { /* best effort */ }
    return okStatus();
  }

  getImpl(): RTCDataChannel { return this.dataChannel; }

  private async handleMessage(data: unknown): Promise<void> {
    try {
      let bytes: Uint8Array;
      if (data instanceof ArrayBuffer) {
        bytes = new Uint8Array(data);
      } else if (ArrayBuffer.isView(data)) {
        bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
      } else if (typeof Blob !== 'undefined' && data instanceof Blob) {
        const buffer = await data.arrayBuffer();
        bytes = new Uint8Array(buffer);
      } else {
        this.fail(invalidArgumentError('WebRTC messages must be binary.'));
        return;
      }
      // Strip the 8-byte sequence suffix the client's multiplexed
      // channel adds.  With a single channel, ordering is inherent.
      if (bytes.byteLength < SEQUENCE_SUFFIX) {
        this.fail(invalidArgumentError('Packet too short for sequence framing.'));
        return;
      }
      const payload = decodeSequence(bytes);
      this.callbacks?.onMessage(payload);
    } catch (error) {
      this.fail(statusFromUnknown(error, 'Message callback raised.'));
    }
  }

  private fail(status: NonOkStatus): void {
    if (this.failure !== null) return;
    this.failure = status;
    if (this.openResolve) {
      this.openResolve(status);
      this.openResolve = null;
    }
    try { this.callbacks?.onError(status); } catch { /* avoid escape */ }
  }
}

// ------------------------------------------- per-peer negotiation state

interface PeerNegotiation {
  connection: RTCPeerConnection;
  remoteDescriptionSet: boolean;
  pendingCandidates: RTCIceCandidateInit[];
  dataChannelReceived: boolean;
}

// ---------------------------------------------- WebRtcServer

/** Callback when a new peer's WebRTC connection is ready. */
export type OnPeerConnected = (
  peerId: string,
  wireStream: WireStream,
  peerConnection: RTCPeerConnection,
) => void;

/**
 * Listens on a signalling connection for incoming WebRTC offers and
 * creates answer-side connections.
 *
 * One instance serves the entire lifetime of a host. Each incoming
 * peer gets its own RTCPeerConnection; the resulting data channel is
 * wrapped as a {@link ChannelWireStream} with SERVER role and handed
 * to the {@link onPeerConnected} callback.
 */
export class WebRtcServer {
  private readonly peers = new Map<string, PeerNegotiation>();
  private closed = false;

  private constructor(
    private readonly signalling: SignallingTransport,
    private readonly iceServers: RTCIceServer[],
    private readonly onPeerConnected: OnPeerConnected,
    private readonly onError: (peerId: string, error: string) => void,
  ) {}

  /**
   * Connect to signalling and start listening for incoming offers.
   */
  static async create(
    signallingUrl: string,
    identity: string,
    iceServers: RTCIceServer[],
    onPeerConnected: OnPeerConnected,
    onError?: (peerId: string, error: string) => void,
  ): Promise<StatusOr<WebRtcServer>> {
    const signalling = await WebSocketSignallingClient.connect(
      signallingUrl,
      identity,
    );
    if (!isOk(signalling)) return signalling;

    const server = new WebRtcServer(
      signalling,
      iceServers,
      onPeerConnected,
      onError ?? (() => {}),
    );

    const cbStatus = signalling.setOnMessage(
      (message) => server.handleSignallingMessage(message),
    );
    if (!isOk(cbStatus)) {
      signalling.close();
      return cbStatus;
    }

    return server;
  }

  /** The signalling transport, for external use (e.g. identity). */
  getSignalling(): SignallingTransport { return this.signalling; }

  /** Shut down signalling and all peer connections. */
  close(): void {
    if (this.closed) return;
    this.closed = true;
    for (const [, peer] of this.peers) {
      try { peer.connection.close(); } catch { /* best effort */ }
    }
    this.peers.clear();
    try { this.signalling.close(); } catch { /* best effort */ }
  }

  /** Return the RTCPeerConnection for a given peer, if it exists. */
  getPeerConnection(peerId: string): RTCPeerConnection | null {
    return this.peers.get(peerId)?.connection ?? null;
  }

  // ----------------------------------------- signalling dispatch

  private async handleSignallingMessage(
    message: SignallingMessage,
  ): Promise<void> {
    if (this.closed) return;

    const sender = message.sender;
    if (!sender) return;

    if (message.type === SignallingMessageType.DESCRIPTION) {
      if (message.descriptionType === 'offer') {
        await this.handleOffer(sender, message);
      }
      // Ignore answers — we are the answerer.
      return;
    }

    if (message.type === SignallingMessageType.CANDIDATE) {
      await this.handleCandidate(sender, message);
      return;
    }

    if (message.type === SignallingMessageType.ERROR) {
      const peer = this.peers.get(sender);
      if (peer) {
        try { peer.connection.close(); } catch { /* best effort */ }
        this.peers.delete(sender);
      }
      this.onError(sender, message.error?.message ?? 'Signalling error');
    }
  }

  private async handleOffer(
    peerId: string,
    message: SignallingMessage,
  ): Promise<void> {
    try {
      // If we already have a negotiation for this peer, close the old one.
      const existing = this.peers.get(peerId);
      if (existing) {
        try { existing.connection.close(); } catch { /* best effort */ }
        this.peers.delete(peerId);
      }

      const connection = new RTCPeerConnection({
        iceServers: this.iceServers,
      });

      const negotiation: PeerNegotiation = {
        connection,
        remoteDescriptionSet: false,
        pendingCandidates: [],
        dataChannelReceived: false,
      };
      this.peers.set(peerId, negotiation);

      // Send ICE candidates back to the peer.
      connection.addEventListener('icecandidate', (event) => {
        if (this.closed || !event.candidate) return;
        const msg = SignallingMessage.create({
          type: SignallingMessageType.CANDIDATE,
          sender: this.signalling.getIdentity(),
          recipient: peerId,
          candidate: event.candidate.candidate,
          mid: event.candidate.sdpMid ?? '',
        });
        if (isOk(msg)) this.signalling.send(msg);
      });

      // Handle connection state changes.
      connection.addEventListener('connectionstatechange', () => {
        if (connection.connectionState === 'failed' ||
            connection.connectionState === 'closed') {
          this.peers.delete(peerId);
        }
      });

      // Listen for the peer's data channel.
      connection.addEventListener('datachannel', (event) => {
        if (negotiation.dataChannelReceived) return;
        negotiation.dataChannelReceived = true;
        this.handleDataChannel(peerId, connection, event.channel);
      });

      // Set remote description (the offer) and create answer.
      await connection.setRemoteDescription({
        type: 'offer',
        sdp: message.description,
      });
      negotiation.remoteDescriptionSet = true;

      // Apply any candidates that arrived before the offer was set.
      for (const candidate of negotiation.pendingCandidates.splice(0)) {
        await connection.addIceCandidate(candidate);
      }

      const answer = await connection.createAnswer();
      await connection.setLocalDescription(answer);

      const local = connection.localDescription;
      if (!local) {
        this.onError(peerId, 'Failed to create local description.');
        return;
      }

      const answerMsg = SignallingMessage.create({
        type: SignallingMessageType.DESCRIPTION,
        sender: this.signalling.getIdentity(),
        recipient: peerId,
        description: local.sdp,
        descriptionType: local.type,
      });
      if (!isOk(answerMsg)) {
        this.onError(peerId, 'Failed to create answer message.');
        return;
      }

      const sent = this.signalling.send(answerMsg);
      if (!isOk(sent)) {
        this.onError(peerId, 'Failed to send answer.');
      }
    } catch (error) {
      this.onError(
        peerId,
        error instanceof Error ? error.message : String(error),
      );
    }
  }

  private async handleCandidate(
    peerId: string,
    message: SignallingMessage,
  ): Promise<void> {
    const negotiation = this.peers.get(peerId);
    if (!negotiation) return;

    const candidate: RTCIceCandidateInit = {
      candidate: message.candidate,
      sdpMid: message.mid || null,
    };

    try {
      if (negotiation.remoteDescriptionSet) {
        await negotiation.connection.addIceCandidate(candidate);
      } else {
        negotiation.pendingCandidates.push(candidate);
      }
    } catch (error) {
      this.onError(
        peerId,
        `ICE candidate error: ${error instanceof Error ? error.message : String(error)}`,
      );
    }
  }

  private handleDataChannel(
    peerId: string,
    connection: RTCPeerConnection,
    dataChannel: RTCDataChannel,
  ): void {
    try {
      const adapter = new SingleDataChannelAdapter(dataChannel, connection);
      const streamId = dataChannel.label || randomId('a11-');

      const wireStream = ChannelWireStream.create(
        adapter,
        streamId,
        ChannelEndpointRole.SERVER,
        {},
        { splitSize: 16 * 1024 },
      );
      if (!isOk(wireStream)) {
        this.onError(peerId, `Wire stream creation failed: ${wireStream.message}`);
        return;
      }

      this.onPeerConnected(peerId, wireStream, connection);
    } catch (error) {
      this.onError(
        peerId,
        error instanceof Error ? error.message : String(error),
      );
    }
  }
}
