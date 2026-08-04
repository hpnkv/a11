import {
  randomId,
  toBytesAsync,
  type ByteMap,
  type ByteMapInput,
} from './bytes.js';
import { Deferred } from './concurrency.js';
import { WireMessage, validateName } from './data.js';
import {
  cancelledError,
  failedPreconditionError,
  internalError,
  invalidArgumentError,
  isOk,
  isStatus,
  okStatus,
  resourceExhaustedError,
  statusFromUnknown,
  unavailableError,
  unimplementedError,
  type NonOkStatus,
  type Status,
  type StatusOr,
} from './status.js';
import {
  SignallingMessage,
  SignallingMessageType,
  type SignallingTransport,
} from './signalling.js';
import {
  ChannelEndpointRole,
  ChannelWireStream,
  type BinaryChannel,
  type BinaryChannelCallbacks,
} from './channel_wire_stream.js';
import {
  type OnWireDone,
  type OnWireMessage,
  type WireDeadline,
  type WireStream,
  type WireStreamOptions,
} from './wire_stream.js';

/** Transport used between a peer and a TURN relay. */
export enum TurnRelayType {
  /** Prefer low-latency UDP relay traffic. */
  UDP = 'udp',
  /** Carry relay traffic over TCP. */
  TCP = 'tcp',
  /** Carry relay traffic over TLS/TCP. */
  TLS = 'tls',
}

/** Structured TURN endpoint and optional long-term credentials. */
export interface TurnServerOptions {
  /** DNS name or IP address without URI brackets. */
  hostname: string;
  /** Relay port; defaults to 3478. */
  port?: number;
  /** TURN username, when authentication is required. */
  username?: string;
  /** TURN credential paired with {@link username}. */
  password?: string;
  /** Relay transport; defaults to UDP. */
  relayType?: TurnRelayType;
}

/** Validated TURN configuration that can be passed to WebRTC ICE setup. */
export class TurnServer {
  /** DNS name or IP address of the relay. */
  hostname: string;
  /** Relay port. */
  port: number;
  /** TURN username. */
  username: string;
  /** TURN password/credential. */
  password: string;
  /** Network transport used to reach the relay. */
  relayType: TurnRelayType;

  constructor(options: TurnServerOptions) {
    this.hostname = options.hostname;
    this.port = options.port ?? 3478;
    this.username = options.username ?? '';
    this.password = options.password ?? '';
    this.relayType = options.relayType ?? TurnRelayType.UDP;
  }

  /** Parse a `turn:` or `turns:` URI into a structured server. */
  static fromString(value: string): StatusOr<TurnServer> {
    if (typeof value !== 'string' || value.length === 0) {
      return invalidArgumentError('TURN server must be a non-empty string.');
    }
    try {
      let remainder = value;
      let secure = false;
      if (remainder.startsWith('turns:')) {
        secure = true;
        remainder = remainder.slice(6);
      } else if (remainder.startsWith('turn:')) {
        remainder = remainder.slice(5);
      }
      const queryIndex = remainder.indexOf('?');
      const query = queryIndex < 0 ? '' : remainder.slice(queryIndex + 1);
      if (queryIndex >= 0) remainder = remainder.slice(0, queryIndex);
      let credentials = '';
      const at = remainder.lastIndexOf('@');
      if (at >= 0) {
        credentials = remainder.slice(0, at);
        remainder = remainder.slice(at + 1);
      }
      let hostname = '';
      let port = 3478;
      if (remainder.startsWith('[')) {
        const closing = remainder.indexOf(']');
        if (closing < 0) return invalidArgumentError('TURN server has an invalid IPv6 host.');
        hostname = remainder.slice(1, closing);
        if (closing + 1 < remainder.length) {
          if (remainder[closing + 1] !== ':') return invalidArgumentError('TURN server address is invalid.');
          port = Number(remainder.slice(closing + 2));
        }
      } else {
        const colon = remainder.lastIndexOf(':');
        if (colon >= 0) {
          hostname = remainder.slice(0, colon);
          port = Number(remainder.slice(colon + 1));
        } else {
          hostname = remainder;
        }
      }
      if (hostname.length === 0) return invalidArgumentError('TURN server requires a hostname.');
      if (!Number.isInteger(port) || port <= 0 || port > 65535) {
        return invalidArgumentError('TURN server has an invalid port.');
      }
      let relayType = secure ? TurnRelayType.TLS : TurnRelayType.UDP;
      if (query === 'transport=tcp') relayType = TurnRelayType.TCP;
      else if (query === 'transport=tls') relayType = TurnRelayType.TLS;
      else if (query !== '' && query !== 'transport=udp') {
        return invalidArgumentError('TURN server has an invalid transport.');
      }
      const separator = credentials.indexOf(':');
      return new TurnServer({
        hostname,
        port,
        username: separator < 0 ? credentials : credentials.slice(0, separator),
        password: separator < 0 ? '' : credentials.slice(separator + 1),
        relayType,
      });
    } catch (error) {
      return invalidArgumentError('Could not parse TURN server.', [], error);
    }
  }

  /** Validate host, port, credentials, and relay transport. */
  validate(): Status {
    try {
      if (typeof this.hostname !== 'string' || this.hostname.length === 0) {
        return invalidArgumentError('TURN server hostname must not be empty.');
      }
      if (!Number.isInteger(this.port) || this.port <= 0 || this.port > 65535) {
        return invalidArgumentError('TURN server port must be in [1, 65535].');
      }
      if (typeof this.username !== 'string' || typeof this.password !== 'string') {
        return invalidArgumentError('TURN server credentials must be strings.');
      }
      if (!Object.values(TurnRelayType).includes(this.relayType)) {
        return invalidArgumentError('TURN relay type is invalid.');
      }
      return okStatus();
    } catch (error) {
      return invalidArgumentError('TURN server fields could not be read.', [], error);
    }
  }

  /** Convert this server to the browser `RTCIceServer` representation. */
  toIceServer(): StatusOr<RTCIceServer> {
    try {
      const validation = this.validate();
      if (!isOk(validation)) return validation;
      const hostname = this.hostname;
      const port = this.port;
      const username = this.username;
      const password = this.password;
      const relayType = this.relayType;
      const ipv6 = hostname.includes(':') ? `[${hostname}]` : hostname;
      const secure = relayType === TurnRelayType.TLS;
      const transport = relayType === TurnRelayType.UDP ? 'udp' : 'tcp';
      return {
        urls: `${secure ? 'turns' : 'turn'}:${ipv6}:${port}?transport=${transport}`,
        username,
        credential: password,
      };
    } catch (error) {
      return invalidArgumentError('TURN server could not be converted to an ICE server.', [], error);
    }
  }
}

/** Injectable peer-connection constructor for tests and Node runtimes. */
export type PeerConnectionFactory = (
  configuration: RTCConfiguration,
) => RTCPeerConnection;

/** ICE, data-channel, and resource options for one WebRTC connection. */
export interface WebRtcConfiguration {
  /** Maximum complete data-channel message, or `null` to use A11's ceiling. */
  maxMessageSize?: number | null;
  /** A11 packet size; keep this below practical SCTP message limits. */
  channelSplitSize?: number;
  /** STUN URLs used for public candidate discovery. */
  stunServers?: readonly string[];
  /** TURN relays used when a direct path cannot be established. */
  turnServers?: readonly TurnServer[];
  /** Additional native `RTCPeerConnection` configuration. */
  rtcConfiguration?: RTCConfiguration;
  /** Client data-channel options; `ordered` defaults to true but may be overridden. */
  dataChannelOptions?: RTCDataChannelInit;
  /**
   * Data channels the client opens per connection and keeps replenished.
   * Striping A11 packets across several channels lets slow per-channel
   * acknowledgement round-trips overlap; the stream still behaves as one
   * ordered, reliable channel. Defaults to 8.
   */
  desiredChannels?: number;
  /** Abort when the data channel buffers more than this many bytes. */
  maxBufferedAmount?: number;
  /** Custom peer-connection constructor when no browser global is available. */
  peerConnectionFactory?: PeerConnectionFactory;
}

function firstError(
  first: Status,
  candidate: unknown,
  operation = 'WebRTC operation',
): Status {
  if (!isStatus(first)) {
    return internalError('WebRTC accumulated an invalid status.');
  }
  if (!isStatus(candidate)) {
    return isOk(first)
      ? internalError(`${operation} returned a non-Status value.`)
      : first;
  }
  return isOk(first) && !isOk(candidate) ? candidate : first;
}

function hasSignallingTransportShape(
  value: unknown,
): value is SignallingTransport {
  if (typeof value !== 'object' || value === null) return false;
  try {
    const candidate = value as Record<string, unknown>;
    return [
      'send',
      'setOnMessage',
      'close',
      'getIdentity',
      'isConnected',
      'getStatus',
    ].every((name) => typeof candidate[name] === 'function');
  } catch {
    return false;
  }
}

function hasPeerConnectionShape(value: unknown): value is RTCPeerConnection {
  if (typeof value !== 'object' || value === null) return false;
  try {
    const candidate = value as Record<string, unknown>;
    return [
      'createDataChannel',
      'addEventListener',
      'createOffer',
      'setLocalDescription',
      'setRemoteDescription',
      'addIceCandidate',
      'close',
    ].every((name) => typeof candidate[name] === 'function');
  } catch {
    return false;
  }
}

function hasDataChannelShape(value: unknown): value is RTCDataChannel {
  if (typeof value !== 'object' || value === null) return false;
  try {
    const candidate = value as Record<string, unknown>;
    const amount = candidate.bufferedAmount;
    return [
      'addEventListener',
      'send',
      'close',
    ].every((name) => typeof candidate[name] === 'function') &&
      typeof candidate.readyState === 'string' &&
      typeof candidate.label === 'string' &&
      typeof amount === 'number' &&
      Number.isFinite(amount) &&
      amount >= 0;
  } catch {
    return false;
  }
}

interface NormalizedWebRtcConfiguration {
  maxMessageSize: number | null;
  channelSplitSize: number;
  rtcConfiguration: RTCConfiguration;
  dataChannelOptions: RTCDataChannelInit;
  desiredChannels: number;
  maxBufferedAmount: number;
  peerConnectionFactory: PeerConnectionFactory | undefined;
}

function normalizeWebRtcConfiguration(
  options: WebRtcConfiguration = {},
): StatusOr<NormalizedWebRtcConfiguration> {
  try {
    return normalizeWebRtcConfigurationUnchecked(options);
  } catch (error) {
    return statusFromUnknown(
      error,
      'Validating WebRTC configuration raised an exception.',
    );
  }
}

function normalizeWebRtcConfigurationUnchecked(
  options: WebRtcConfiguration = {},
): StatusOr<NormalizedWebRtcConfiguration> {
  if (typeof options !== 'object' || options === null || Array.isArray(options)) {
    return invalidArgumentError('WebRTC configuration must be an object.');
  }
  const maxMessageSize = options.maxMessageSize === undefined
    ? 64 * 1024
    : options.maxMessageSize;
  const channelSplitSize = options.channelSplitSize ?? 48 * 1024;
  if (
    maxMessageSize !== null &&
    (!Number.isSafeInteger(maxMessageSize) || maxMessageSize <= 0)
  ) {
    return invalidArgumentError('maxMessageSize must be positive or null.');
  }
  if (
    !Number.isSafeInteger(channelSplitSize) ||
    channelSplitSize < 18 ||
    channelSplitSize > 1024 * 1024
  ) {
    return invalidArgumentError('channelSplitSize must be in [18, 1048576].');
  }
  if (maxMessageSize !== null && channelSplitSize > maxMessageSize) {
    return invalidArgumentError('channelSplitSize exceeds maxMessageSize.');
  }
  const maxBufferedAmount = options.maxBufferedAmount ?? 16 * 1024 * 1024;
  if (!Number.isSafeInteger(maxBufferedAmount) || maxBufferedAmount <= 0) {
    return invalidArgumentError('maxBufferedAmount must be a positive integer.');
  }
  const desiredChannels = options.desiredChannels ?? 8;
  if (!Number.isSafeInteger(desiredChannels) || desiredChannels < 1) {
    return invalidArgumentError('desiredChannels must be an integer >= 1.');
  }
  if (
    options.peerConnectionFactory !== undefined &&
    typeof options.peerConnectionFactory !== 'function'
  ) {
    return invalidArgumentError('peerConnectionFactory must be callable.');
  }
  const sourceRtcConfiguration = options.rtcConfiguration ?? {};
  if (
    typeof sourceRtcConfiguration !== 'object' ||
    sourceRtcConfiguration === null ||
    Array.isArray(sourceRtcConfiguration)
  ) {
    return invalidArgumentError('rtcConfiguration must be an object.');
  }
  const sourceDataChannelOptions = options.dataChannelOptions ?? {};
  if (
    typeof sourceDataChannelOptions !== 'object' ||
    sourceDataChannelOptions === null ||
    Array.isArray(sourceDataChannelOptions)
  ) {
    return invalidArgumentError('dataChannelOptions must be an object.');
  }
  const stunServers = options.stunServers ?? [];
  if (!Array.isArray(stunServers)) {
    return invalidArgumentError('stunServers must be an array.');
  }
  const turnServers = options.turnServers ?? [];
  if (!Array.isArray(turnServers)) {
    return invalidArgumentError('turnServers must be an array.');
  }
  const rtcConfiguration: RTCConfiguration = { ...sourceRtcConfiguration };
  const configuredIceServers = rtcConfiguration.iceServers ?? [];
  if (!Array.isArray(configuredIceServers)) {
    return invalidArgumentError('rtcConfiguration.iceServers must be an array.');
  }
  const iceServers = [...configuredIceServers];
  for (const url of stunServers) {
    if (typeof url !== 'string' || url.length === 0) {
      return invalidArgumentError('Each STUN server must be a non-empty string.');
    }
    iceServers.push({ urls: url });
  }
  for (const turn of turnServers) {
    if (!(turn instanceof TurnServer)) {
      return invalidArgumentError('Each TURN server must be a TurnServer.');
    }
    const value = turn.toIceServer();
    if (!isOk(value)) return value;
    iceServers.push(value);
  }
  rtcConfiguration.iceServers = iceServers;
  return {
    maxMessageSize,
    channelSplitSize,
    rtcConfiguration,
    dataChannelOptions: {
      ordered: true,
      ...sourceDataChannelOptions,
    },
    desiredChannels,
    maxBufferedAmount,
    peerConnectionFactory: options.peerConnectionFactory,
  };
}


/** Fixed 8-byte little-endian frame prefix carrying the aggregate send order. */
const SEQUENCE_PREFIX = 8;

function encodeSequence(sequence: number, payload: Uint8Array): Uint8Array {
  const framed = new Uint8Array(SEQUENCE_PREFIX + payload.byteLength);
  const view = new DataView(framed.buffer);
  // Split into two little-endian 32-bit halves; sequence stays < 2^53 in
  // practice, and this matches the C++ 8-byte LE uint64 framing byte for byte.
  view.setUint32(0, sequence >>> 0, true);
  view.setUint32(4, Math.floor(sequence / 0x100000000), true);
  framed.set(payload, SEQUENCE_PREFIX);
  return framed;
}

function decodeSequence(framed: Uint8Array): { sequence: number; payload: Uint8Array } {
  const view = new DataView(framed.buffer, framed.byteOffset, framed.byteLength);
  const low = view.getUint32(0, true);
  const high = view.getUint32(4, true);
  return {
    sequence: high * 0x100000000 + low,
    payload: framed.subarray(SEQUENCE_PREFIX),
  };
}

/** Callbacks a {@link RtcDataChannelMember} reports to its multiplex owner. */
interface MemberHandlers {
  onOpen: () => void;
  onMessage: (framed: Uint8Array) => void;
  onError: (status: NonOkStatus) => void;
  onClosed: () => void;
  onBufferedAmountLow: () => void;
}

/**
 * Adapts one {@link RTCDataChannel} for {@link MultiplexedRtcChannel}.
 *
 * Unlike the whole-stream adapter, a member owns only its data channel: closing
 * it never touches the shared peer connection or signalling transport, which
 * the multiplex owns and tears down once, when the stream completes.
 */
class RtcDataChannelMember {
  private handlers: MemberHandlers | null = null;
  private closed = false;
  private failure: NonOkStatus | null = null;
  private messageChain: Promise<void> = Promise.resolve();
  private pollTimer: ReturnType<typeof setTimeout> | null = null;

  constructor(
    readonly dataChannel: RTCDataChannel,
    private readonly maxBufferedAmount: number,
  ) {
    dataChannel.binaryType = 'arraybuffer';
    dataChannel.bufferedAmountLowThreshold = 0;
    dataChannel.addEventListener('open', () => {
      try { this.handlers?.onOpen(); }
      catch (error) { this.fail(statusFromUnknown(error, 'WebRTC open callback raised.')); }
    });
    dataChannel.addEventListener('message', (event) => this.handleMessage(event.data));
    dataChannel.addEventListener('error', () =>
      this.fail(unavailableError('WebRTC data channel reported an error.')),
    );
    dataChannel.addEventListener('close', () => {
      if (this.closed) return;
      this.closed = true;
      try { this.handlers?.onClosed(); } catch { /* terminal */ }
    });
    dataChannel.addEventListener('bufferedamountlow', () => {
      try { this.handlers?.onBufferedAmountLow(); } catch { /* observable */ }
    });
  }

  setHandlers(handlers: MemberHandlers): void {
    this.handlers = handlers;
  }

  isOpen(): boolean {
    try { return !this.closed && this.dataChannel.readyState === 'open'; }
    catch { return false; }
  }

  send(framed: Uint8Array): Status {
    try {
      if (!this.isOpen()) {
        return this.failure ?? failedPreconditionError('WebRTC data channel is not open.');
      }
      const buffered = this.bufferedAmount();
      if (!isOk(buffered)) return buffered;
      if (buffered + framed.byteLength > this.maxBufferedAmount) {
        return resourceExhaustedError('WebRTC buffered amount would exceed maxBufferedAmount.');
      }
      this.dataChannel.send(framed.slice().buffer);
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'WebRTC data channel send raised an exception.');
    }
  }

  bufferedAmount(): StatusOr<number> {
    try {
      const amount = this.dataChannel.bufferedAmount;
      return typeof amount === 'number' && Number.isFinite(amount) && amount >= 0
        ? amount
        : internalError('RTCDataChannel.bufferedAmount returned an invalid value.');
    } catch (error) {
      return statusFromUnknown(error, 'Reading WebRTC buffered amount failed.');
    }
  }

  /** Close only this data channel; the connection and signalling are shared. */
  closeChannel(): void {
    if (this.closed) this.closed = true;
    this.closed = true;
    if (this.pollTimer !== null) { clearTimeout(this.pollTimer); this.pollTimer = null; }
    try { this.dataChannel.close(); } catch { /* best effort */ }
  }

  private handleMessage(data: unknown): void {
    this.messageChain = this.messageChain.then(async () => {
      if (
        !(data instanceof ArrayBuffer) &&
        !ArrayBuffer.isView(data) &&
        !(typeof Blob !== 'undefined' && data instanceof Blob)
      ) {
        this.fail(invalidArgumentError('A11 WebRTC messages must be binary.'));
        return;
      }
      const bytes = await toBytesAsync(data);
      if (!isOk(bytes)) { this.fail(bytes); return; }
      try { this.handlers?.onMessage(bytes); }
      catch (error) { this.fail(statusFromUnknown(error, 'WebRTC message callback raised.')); }
    }).catch((error: unknown) => {
      this.fail(statusFromUnknown(error, 'WebRTC message processing failed.'));
    });
  }

  private fail(status: NonOkStatus): void {
    if (this.failure !== null) return;
    this.failure = status;
    try { this.handlers?.onError(status); } catch { /* avoid escape */ }
  }
}

interface Member {
  channel: RtcDataChannelMember;
  open: boolean;
}

/**
 * A {@link BinaryChannel} that stripes packets across several data channels.
 *
 * Each packet is tagged with a monotonic sequence number and sent over one live
 * member channel chosen round-robin, so slow per-channel acknowledgement
 * round-trips overlap. Inbound member packets are reordered by sequence and
 * delivered in the original order, so the framing layer sees one ordered,
 * reliable channel. A background task replenishes lost members toward the
 * desired count, giving up after four consecutive failures; losses and
 * replenishment are logged at debug level. This owns the peer connection and
 * signalling transport and tears them down once, on {@link close}.
 */
class MultiplexedRtcChannel implements BinaryChannel {
  private callbacks: BinaryChannelCallbacks | null = null;
  private readonly members: Member[] = [];
  private roundRobin = 0;
  private nextSendSequence = 0;
  private nextDeliverSequence = 0;
  private readonly pendingOut: Uint8Array[] = [];
  private readonly reorder = new Map<number, Uint8Array>();
  private delivering = false;
  private flushing = false;
  private anyOpen = false;
  private closed = false;
  private negotiationStarted = false;
  private replenishing = false;
  private replenishFailures = 0;
  private replenishGaveUp = false;
  private readonly opened = new Deferred<Status>();
  private failure: NonOkStatus | null = null;
  private readonly drainWaiters: Array<Deferred<Status>> = [];
  private pollTimer: ReturnType<typeof setTimeout> | null = null;
  private primaryChannel: RTCDataChannel | null = null;

  private constructor(
    readonly connection: RTCPeerConnection,
    readonly signalling: SignallingTransport,
    readonly identity: string,
    readonly peerIdentity: string,
    private readonly maxBufferedAmount: number,
    private readonly desiredChannels: number,
    private readonly maxReorderPackets = 4096,
  ) {}

  static create(
    connection: RTCPeerConnection,
    initialChannels: readonly RTCDataChannel[],
    signalling: SignallingTransport,
    identity: string,
    peerIdentity: string,
    maxBufferedAmount: number,
    desiredChannels: number,
  ): StatusOr<MultiplexedRtcChannel> {
    try {
      const multiplex = new MultiplexedRtcChannel(
        connection, signalling, identity, peerIdentity,
        maxBufferedAmount, desiredChannels,
      );
      for (const dataChannel of initialChannels) {
        multiplex.adoptChannel(dataChannel);
      }
      return multiplex;
    } catch (error) {
      try { connection.close(); } catch { /* preserve status */ }
      try { signalling.close(); } catch { /* preserve status */ }
      return statusFromUnknown(error, 'Configuring multiplexed WebRTC channel raised.');
    }
  }

  /** The first data channel opened, for the WebRtcWireStream.dataChannel accessor. */
  get dataChannel(): RTCDataChannel {
    // Always set: create() adopts at least one channel before returning.
    return this.primaryChannel as RTCDataChannel;
  }

  private adoptChannel(dataChannel: RTCDataChannel): void {
    if (this.primaryChannel === null) this.primaryChannel = dataChannel;
    const member: Member = {
      channel: new RtcDataChannelMember(dataChannel, this.maxBufferedAmount),
      open: false,
    };
    member.channel.setHandlers({
      onOpen: () => this.onMemberOpen(member),
      onMessage: (framed) => this.onMemberMessage(framed),
      onError: (status) => this.dropMember(member, status.message ?? 'error'),
      onClosed: () => this.dropMember(member, 'channel closed'),
      onBufferedAmountLow: () => { this.flushPending(); this.checkDrain(); },
    });
    this.members.push(member);
    if (member.channel.isOpen()) this.onMemberOpen(member);
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
    try {
      if (this.failure !== null) return this.failure;
      if (this.anyOpen) { void this.startReplenishment(); return okStatus(); }
      if (!this.negotiationStarted) {
        this.negotiationStarted = true;
        queueMicrotask(() => void this.negotiate());
      }
      const status = await this.opened.promise;
      if (isOk(status)) void this.startReplenishment();
      return status;
    } catch (error) {
      const status = statusFromUnknown(error, 'Opening multiplexed WebRTC channel raised.');
      this.fail(status);
      return status;
    }
  }

  isOpen(): boolean {
    return !this.closed && this.anyOpen && this.members.some((m) => m.open);
  }

  send(packet: Uint8Array): Status {
    try {
      if (!(packet instanceof Uint8Array)) {
        return invalidArgumentError('WebRTC packet must be a Uint8Array.');
      }
      if (this.closed) return this.failure ?? cancelledError('WebRTC channel closed.');
      this.pendingOut.push(encodeSequence(this.nextSendSequence++, packet));
      this.flushPending();
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'WebRTC multiplexed send raised an exception.');
    }
  }

  bufferedAmount(): StatusOr<number> {
    let total = 0;
    for (const framed of this.pendingOut) total += framed.byteLength;
    for (const member of this.members) {
      const amount = member.channel.bufferedAmount();
      if (isOk(amount)) total += amount;
    }
    return total;
  }

  waitForBufferedAmountLow(): Promise<Status> {
    const amount = this.bufferedAmount();
    if (!isOk(amount)) return Promise.resolve(amount);
    if (amount === 0) return Promise.resolve(okStatus());
    const waiter = new Deferred<Status>();
    this.drainWaiters.push(waiter);
    this.scheduleDrainPoll();
    return waiter.promise;
  }

  close(): Status {
    if (this.closed) return okStatus();
    this.closed = true;
    if (this.pollTimer !== null) { clearTimeout(this.pollTimer); this.pollTimer = null; }
    const stop = this.failure ?? cancelledError('WebRTC channel closed.');
    for (const waiter of this.drainWaiters.splice(0)) waiter.resolve(stop);
    for (const member of this.members.splice(0)) member.channel.closeChannel();
    let first: Status = okStatus();
    try { this.connection.close(); }
    catch (error) { first = statusFromUnknown(error, 'Closing WebRTC peer connection failed.'); }
    try {
      const signallingStatus = this.signalling.close();
      first = firstError(first, signallingStatus, 'SignallingTransport.close()');
    } catch (error) {
      first = firstError(first, statusFromUnknown(error, 'Closing WebRTC signalling raised.'), 'SignallingTransport.close()');
    }
    return first;
  }

  getImpl(): unknown | null { return this.primaryChannel; }

  signalFailure(status: NonOkStatus): void { this.fail(status); }

  private onMemberOpen(member: Member): void {
    if (this.closed || member.open) return;
    member.open = true;
    let announce = false;
    if (!this.anyOpen) { this.anyOpen = true; announce = true; }
    this.flushPending();
    if (announce) {
      this.opened.resolve(okStatus());
      try { this.callbacks?.onOpen(); }
      catch (error) { this.fail(statusFromUnknown(error, 'WebRTC open callback raised.')); }
    }
  }

  private onMemberMessage(framed: Uint8Array): void {
    if (this.closed) return;
    if (framed.byteLength < SEQUENCE_PREFIX) {
      this.fail(invalidArgumentError('Multiplexed member packet was malformed.'));
      return;
    }
    const { sequence, payload } = decodeSequence(framed);
    if (sequence >= this.nextDeliverSequence && !this.reorder.has(sequence)) {
      if (this.reorder.size >= this.maxReorderPackets) {
        this.fail(resourceExhaustedError('Multiplexed channel reorder buffer overflowed.'));
        return;
      }
      // Copy out of the transport buffer so a later subarray stays valid.
      this.reorder.set(sequence, payload.slice());
    }
    if (this.delivering) return;
    this.delivering = true;
    try {
      for (;;) {
        const next = this.reorder.get(this.nextDeliverSequence);
        if (next === undefined) break;
        this.reorder.delete(this.nextDeliverSequence);
        this.nextDeliverSequence++;
        try { this.callbacks?.onMessage(next); }
        catch (error) { this.fail(statusFromUnknown(error, 'WebRTC message callback raised.')); return; }
      }
    } finally {
      this.delivering = false;
    }
  }

  private flushPending(): void {
    if (this.flushing) return;
    this.flushing = true;
    try {
      while (this.pendingOut.length > 0 && !this.closed) {
        let chosen: Member | null = null;
        const count = this.members.length;
        for (let attempt = 0; attempt < count; attempt++) {
          const candidate = this.members[(this.roundRobin + attempt) % count];
          if (candidate.open) {
            chosen = candidate;
            this.roundRobin = (this.roundRobin + attempt + 1) % count;
            break;
          }
        }
        if (chosen === null) break; // No live channel; keep buffered.
        const framed = this.pendingOut[0];
        const sent = chosen.channel.send(framed);
        if (isOk(sent)) { this.pendingOut.shift(); continue; }
        // Reroute the packet: drop the failed member, retry another.
        chosen.open = false;
        this.dropMember(chosen, 'send failed');
      }
    } finally {
      this.flushing = false;
    }
  }

  private dropMember(member: Member, reason: string): void {
    const index = this.members.indexOf(member);
    if (index < 0) return;
    this.members.splice(index, 1);
    member.open = false;
    member.channel.closeChannel();
    const live = this.members.filter((m) => m.open).length;
    console.debug(`a11 webrtc: lost data channel (${reason}); ${live} of ${this.desiredChannels} remain`);
    void this.startReplenishment();
  }

  private async startReplenishment(): Promise<void> {
    if (this.replenishing || this.closed || this.replenishGaveUp) return;
    this.replenishing = true;
    try {
      while (!this.closed && !this.replenishGaveUp) {
        const live = this.members.length;
        if (live >= this.desiredChannels) break;
        if (this.replenishFailures >= 4) {
          this.replenishGaveUp = true;
          console.debug('a11 webrtc: giving up channel replenishment after 4 consecutive failures');
          break;
        }
        const opened = await this.replenishOne();
        if (opened) {
          this.replenishFailures = 0;
          console.debug(`a11 webrtc: replenished data channel; ${this.members.filter((m) => m.open).length} of ${this.desiredChannels} live`);
        } else {
          this.replenishFailures++;
          console.debug('a11 webrtc: channel replenishment attempt failed');
        }
      }
    } finally {
      this.replenishing = false;
    }
  }

  private async replenishOne(): Promise<boolean> {
    let dataChannel: RTCDataChannel;
    try {
      dataChannel = this.connection.createDataChannel(randomId('a11-'), { ordered: true });
    } catch {
      return false;
    }
    const member: Member = {
      channel: new RtcDataChannelMember(dataChannel, this.maxBufferedAmount),
      open: false,
    };
    const openedDeferred = new Deferred<boolean>();
    let settled = false;
    const settle = (value: boolean) => { if (!settled) { settled = true; openedDeferred.resolve(value); } };
    member.channel.setHandlers({
      onOpen: () => { this.onMemberOpen(member); settle(true); },
      onMessage: (framed) => this.onMemberMessage(framed),
      onError: () => { this.dropMember(member, 'replenishment error'); settle(false); },
      onClosed: () => { this.dropMember(member, 'replenishment closed'); settle(false); },
      onBufferedAmountLow: () => { this.flushPending(); this.checkDrain(); },
    });
    this.members.push(member);
    const timer = setTimeout(() => settle(false), 20000);
    if (member.channel.isOpen()) { this.onMemberOpen(member); settle(true); }
    const opened = await openedDeferred.promise;
    clearTimeout(timer);
    if (!opened) {
      const index = this.members.indexOf(member);
      if (index >= 0) this.members.splice(index, 1);
      member.channel.closeChannel();
    }
    return opened;
  }

  private async negotiate(): Promise<void> {
    try {
      if (typeof this.signalling.isConnected() === 'boolean' && !this.signalling.isConnected()) {
        const status = this.signalling.getStatus();
        this.fail(isOk(status) ? failedPreconditionError('WebRTC signalling is not connected.') : status as NonOkStatus);
        return;
      }
      const offer = await this.connection.createOffer();
      await this.connection.setLocalDescription(offer);
      const local = this.connection.localDescription;
      if (local === null || typeof local.sdp !== 'string' || local.sdp.length === 0 || local.type !== 'offer') {
        this.fail(failedPreconditionError('WebRTC did not create a local description.'));
        return;
      }
      const message = SignallingMessage.create({
        type: SignallingMessageType.DESCRIPTION,
        sender: this.identity,
        recipient: this.peerIdentity,
        description: local.sdp,
        descriptionType: local.type,
      });
      if (!isOk(message)) { this.fail(message); return; }
      const sent = this.signalling.send(message);
      if (isStatus(sent) && !isOk(sent)) this.fail(sent);
    } catch (error) {
      this.fail(statusFromUnknown(error, 'Creating WebRTC offer raised an exception.'));
    }
  }

  private fail(status: NonOkStatus): void {
    if (this.failure !== null) return;
    this.failure = status;
    this.opened.resolve(status);
    for (const waiter of this.drainWaiters.splice(0)) waiter.resolve(status);
    try { this.callbacks?.onError(status); } catch { /* avoid escape */ }
  }

  private scheduleDrainPoll(): void {
    if (this.pollTimer !== null || this.drainWaiters.length === 0) return;
    this.pollTimer = setTimeout(() => {
      this.pollTimer = null;
      this.checkDrain();
      if (this.drainWaiters.length > 0) this.scheduleDrainPoll();
    }, 4);
  }

  private checkDrain(): void {
    const amount = this.bufferedAmount();
    if (!isOk(amount)) {
      for (const waiter of this.drainWaiters.splice(0)) waiter.resolve(amount);
      return;
    }
    if (amount !== 0) return;
    for (const waiter of this.drainWaiters.splice(0)) waiter.resolve(okStatus());
    try { this.callbacks?.onBufferedAmountLow(); } catch { /* observable */ }
  }
}

interface ClientContext {
  channel: MultiplexedRtcChannel;
  connection: RTCPeerConnection;
  identity: string;
  peerIdentity: string;
  remoteDescriptionSet: boolean;
  pendingCandidates: RTCIceCandidateInit[];
}

async function applyClientSignal(
  context: ClientContext,
  message: SignallingMessage,
): Promise<Status> {
  try {
    if (!(message instanceof SignallingMessage)) {
      const status = invalidArgumentError(
        'WebRTC signalling callback received an invalid message.',
      );
      context.channel.signalFailure(status);
      return status;
    }
    const validation = message.validate();
    if (!isStatus(validation)) {
      const status = internalError(
        'SignallingMessage.validate() returned a non-Status value.',
      );
      context.channel.signalFailure(status);
      return status;
    }
    if (!isOk(validation)) {
      context.channel.signalFailure(validation);
      return validation;
    }
    if (message.sender !== '' && message.sender !== context.peerIdentity) {
      const status = invalidArgumentError('WebRTC signalling came from an unexpected peer.');
      context.channel.signalFailure(status);
      return status;
    }
    if (message.recipient !== '' && message.recipient !== context.identity) {
      const status = invalidArgumentError('WebRTC signalling has the wrong recipient.');
      context.channel.signalFailure(status);
      return status;
    }
    if (message.type === SignallingMessageType.ERROR) {
      const status = isOk(message.error)
        ? unavailableError('Peer reported an empty WebRTC signalling error.')
        : message.error;
      context.channel.signalFailure(status);
      return status;
    }
    if (message.type === SignallingMessageType.DESCRIPTION) {
      if (message.descriptionType !== 'answer' && message.descriptionType !== 'pranswer') {
        const status = invalidArgumentError('WebRTC client expected an answer description.');
        context.channel.signalFailure(status);
        return status;
      }
      await context.connection.setRemoteDescription({
        type: message.descriptionType,
        sdp: message.description,
      });
      context.remoteDescriptionSet = true;
      for (const candidate of context.pendingCandidates.splice(0)) {
        await context.connection.addIceCandidate(candidate);
      }
      return okStatus();
    }
    const candidate: RTCIceCandidateInit = {
      candidate: message.candidate,
      sdpMid: message.mid || null,
    };
    if (context.remoteDescriptionSet) await context.connection.addIceCandidate(candidate);
    else context.pendingCandidates.push(candidate);
    return okStatus();
  } catch (error) {
    const status = statusFromUnknown(
      error,
      'Applying WebRTC client signalling raised an exception.',
    );
    context.channel.signalFailure(status);
    return status;
  }
}

/**
 * Client-side WireStream over a browser-compatible WebRTC data channel.
 *
 * WebRTC negotiation runs through a connected {@link SignallingTransport};
 * application messages then use the peer-to-peer data channel. The signalling
 * endpoint must remain alive while ICE candidates and descriptions are being
 * exchanged. A11 packetizes messages below SCTP limits and applies the same
 * half-close, drain, abort, and deadline lifecycle as its WebSocket transport.
 *
 * Create the endpoint, call {@link start}, and await {@link wait} for full
 * completion. The returned stream is client-side; accepting server-side data
 * channels is handled by the native service transport.
 */
export class WebRtcWireStream implements WireStream {
  private constructor(
    private readonly stream: ChannelWireStream,
    private readonly channel: MultiplexedRtcChannel,
  ) {}

  /** Configure a peer connection and register signalling before startup. */
  static createClient(
    peerIdentity: string,
    signalling: SignallingTransport,
    configuration: WebRtcConfiguration = {},
    options: WireStreamOptions = {},
  ): StatusOr<WebRtcWireStream> {
    try {
      return WebRtcWireStream.createClientUnchecked(
        peerIdentity,
        signalling,
        configuration,
        options,
      );
    } catch (error) {
      return statusFromUnknown(
        error,
        'Creating WebRTC WireStream client raised an exception.',
      );
    }
  }

  private static createClientUnchecked(
    peerIdentity: string,
    signalling: SignallingTransport,
    configuration: WebRtcConfiguration,
    options: WireStreamOptions,
  ): StatusOr<WebRtcWireStream> {
    if (!hasSignallingTransportShape(signalling)) {
      return invalidArgumentError('signalling must implement SignallingTransport.');
    }
    let identity: string;
    try { identity = signalling.getIdentity(); }
    catch (error) { return statusFromUnknown(error, 'Reading signalling identity raised an exception.'); }
    const validIdentity = validateName(identity);
    if (!isOk(validIdentity)) return validIdentity;
    const validPeer = validateName(peerIdentity);
    if (!isOk(validPeer)) return validPeer;
    if (identity === peerIdentity) {
      return invalidArgumentError('WebRTC identity and peerIdentity must differ.');
    }
    let connected: boolean;
    try { connected = signalling.isConnected(); }
    catch (error) {
      return statusFromUnknown(
        error,
        'Reading signalling connection state raised an exception.',
      );
    }
    if (typeof connected !== 'boolean') {
      return internalError(
        'SignallingTransport.isConnected() returned a non-boolean value.',
      );
    }
    if (!connected) {
      let status: Status;
      try { status = signalling.getStatus(); }
      catch (error) {
        return statusFromUnknown(
          error,
          'Reading signalling status raised an exception.',
        );
      }
      if (!isStatus(status)) {
        return internalError(
          'SignallingTransport.getStatus() returned a non-Status value.',
        );
      }
      return isOk(status)
        ? failedPreconditionError('WebRTC signalling transport is not connected.')
        : status;
    }
    const normalized = normalizeWebRtcConfiguration(configuration);
    if (!isOk(normalized)) return normalized;
    let connection: RTCPeerConnection;
    try {
      if (normalized.peerConnectionFactory !== undefined) {
        connection = normalized.peerConnectionFactory(normalized.rtcConfiguration);
      } else if (typeof globalThis.RTCPeerConnection === 'function') {
        connection = new globalThis.RTCPeerConnection(normalized.rtcConfiguration);
      } else {
        return unimplementedError(
          'RTCPeerConnection is unavailable; provide peerConnectionFactory in Node.js.',
        );
      }
    } catch (error) {
      return statusFromUnknown(error, 'Creating WebRTC peer connection raised an exception.');
    }
    if (!hasPeerConnectionShape(connection)) {
      return invalidArgumentError(
        'Peer connection factory returned an invalid RTCPeerConnection.',
      );
    }
    // Open the desired number of data channels up front so packets stripe
    // across them from the first send; the multiplex replenishes losses later.
    const dataChannels: RTCDataChannel[] = [];
    for (let index = 0; index < normalized.desiredChannels; index++) {
      let dataChannel: RTCDataChannel;
      try {
        dataChannel = connection.createDataChannel(
          randomId('a11-'),
          normalized.dataChannelOptions,
        );
      } catch (error) {
        for (const opened of dataChannels) {
          try { opened.close(); } catch { /* preserve primary status */ }
        }
        try { connection.close(); } catch { /* preserve primary status */ }
        return statusFromUnknown(error, 'Creating WebRTC data channel raised an exception.');
      }
      if (!hasDataChannelShape(dataChannel)) {
        try {
          (dataChannel as unknown as { close?: () => void } | null)?.close?.();
        } catch { /* preserve validation status */ }
        for (const opened of dataChannels) {
          try { opened.close(); } catch { /* preserve validation status */ }
        }
        try { connection.close(); } catch { /* preserve validation status */ }
        return invalidArgumentError(
          'RTCPeerConnection.createDataChannel() returned an invalid data channel.',
        );
      }
      dataChannels.push(dataChannel);
    }
    const dataChannel = dataChannels[0];
    const createdChannel = MultiplexedRtcChannel.create(
      connection,
      dataChannels,
      signalling,
      identity,
      peerIdentity,
      normalized.maxBufferedAmount,
      normalized.desiredChannels,
    );
    if (!isOk(createdChannel)) return createdChannel;
    const channel = createdChannel;
    const context: ClientContext = {
      channel,
      connection,
      identity,
      peerIdentity,
      remoteDescriptionSet: false,
      pendingCandidates: [],
    };
    let callbackStatus: Status;
    try {
      callbackStatus = signalling.setOnMessage(
        (message) => applyClientSignal(context, message),
      );
    } catch (error) {
      channel.close();
      return statusFromUnknown(
        error,
        'Registering WebRTC signalling callback raised an exception.',
      );
    }
    if (!isStatus(callbackStatus)) {
      channel.close();
      return internalError(
        'SignallingTransport.setOnMessage() returned a non-Status value.',
      );
    }
    if (!isOk(callbackStatus)) {
      channel.close();
      return callbackStatus;
    }
    try {
      connection.addEventListener('icecandidate', (event) => {
        try {
          const candidate = event.candidate;
          if (candidate === null) return;
          const message = SignallingMessage.create({
            type: SignallingMessageType.CANDIDATE,
            sender: identity,
            recipient: peerIdentity,
            candidate: candidate.candidate,
            mid: candidate.sdpMid ?? '',
          });
          if (!isOk(message)) {
            channel.signalFailure(message);
            return;
          }
          const sent = signalling.send(message);
          if (!isStatus(sent)) {
            channel.signalFailure(
              internalError(
                'SignallingTransport.send() returned a non-Status value.',
              ),
            );
          } else if (!isOk(sent)) {
            channel.signalFailure(sent);
          }
        } catch (error) {
          channel.signalFailure(statusFromUnknown(
            error,
            'Sending WebRTC ICE candidate raised an exception.',
          ));
        }
      });
      connection.addEventListener('connectionstatechange', () => {
        try {
          if (connection.connectionState === 'failed') {
            channel.signalFailure(
              unavailableError('WebRTC peer connection failed.'),
            );
          } else if (connection.connectionState === 'closed') {
            channel.signalFailure(
              cancelledError('WebRTC peer connection closed.'),
            );
          }
        } catch (error) {
          channel.signalFailure(statusFromUnknown(
            error,
            'Reading WebRTC peer connection state raised an exception.',
          ));
        }
      });
    } catch (error) {
      channel.close();
      return statusFromUnknown(
        error,
        'Registering WebRTC peer listeners raised an exception.',
      );
    }
    let framed: StatusOr<ChannelWireStream>;
    try {
      framed = ChannelWireStream.create(
        channel,
        dataChannel.label || randomId('a11-'),
        ChannelEndpointRole.CLIENT,
        options,
        { splitSize: normalized.channelSplitSize },
      );
    } catch (error) {
      channel.close();
      return statusFromUnknown(
        error,
        'Creating framed WebRTC stream raised an exception.',
      );
    }
    if (!isOk(framed)) {
      channel.close();
      return framed;
    }
    return new WebRtcWireStream(framed, channel);
  }

  /** Underlying data channel for advanced browser integration. */
  get dataChannel(): RTCDataChannel { return this.channel.dataChannel; }
  /** Underlying peer connection for diagnostics and browser statistics. */
  get peerConnection(): RTCPeerConnection { return this.channel.connection; }
  /** Signalling transport used to negotiate this peer connection. */
  get signallingEndpoint(): SignallingTransport { return this.channel.signalling; }

  send(message: WireMessage): Status { return this.stream.send(message); }
  start(onMessage?: OnWireMessage, onDone?: OnWireDone): Promise<Status> { return this.stream.start(onMessage, onDone); }
  accept(onMessage?: OnWireMessage, onDone?: OnWireDone): Promise<Status> { return this.stream.accept(onMessage, onDone); }
  halfClose(trailers?: ByteMapInput): Status { return this.stream.halfClose(trailers); }
  drainOutgoingMessages(): Promise<Status> { return this.stream.drainOutgoingMessages(); }
  abort(status: Status): Status { return this.stream.abort(status); }
  setDeadline(deadline?: WireDeadline): Status { return this.stream.setDeadline(deadline); }
  getDeadline(): number | null { return this.stream.getDeadline(); }
  getStatus(): Status { return this.stream.getStatus(); }
  getTrailers(): ByteMap | null { return this.stream.getTrailers(); }
  getId(): string { return this.stream.getId(); }
  getImpl(): unknown | null { return this.stream.getImpl(); }
  wait(): Promise<Status> { return this.stream.wait(); }
}
