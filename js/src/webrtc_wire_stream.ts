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
    maxBufferedAmount,
    peerConnectionFactory: options.peerConnectionFactory,
  };
}

class RtcBinaryChannel implements BinaryChannel {
  private callbacks: BinaryChannelCallbacks | null = null;
  private readonly opened = new Deferred<Status>();
  private negotiationStarted = false;
  private closed = false;
  private failure: NonOkStatus | null = null;
  private messageChain: Promise<void> = Promise.resolve();
  private readonly drainWaiters: Array<Deferred<Status>> = [];
  private pollTimer: ReturnType<typeof setTimeout> | null = null;

  private constructor(
    readonly connection: RTCPeerConnection,
    readonly dataChannel: RTCDataChannel,
    readonly signalling: SignallingTransport,
    readonly identity: string,
    readonly peerIdentity: string,
    private readonly maxBufferedAmount: number,
  ) {
    dataChannel.binaryType = 'arraybuffer';
    dataChannel.bufferedAmountLowThreshold = 0;
    dataChannel.addEventListener('open', () => {
      try { this.handleOpen(); }
      catch (error) {
        this.fail(statusFromUnknown(error, 'Handling WebRTC open event raised.'));
      }
    });
    dataChannel.addEventListener('message', (event) => {
      try { this.handleMessage(event.data); }
      catch (error) {
        this.fail(statusFromUnknown(error, 'Handling WebRTC message event raised.'));
      }
    });
    dataChannel.addEventListener('error', () =>
      this.fail(unavailableError('WebRTC data channel reported an error.')),
    );
    dataChannel.addEventListener('close', () => {
      try { this.handleClose(); }
      catch (error) {
        this.fail(statusFromUnknown(error, 'Handling WebRTC close event raised.'));
      }
    });
    dataChannel.addEventListener('bufferedamountlow', () => {
      try { this.checkDrain(); }
      catch (error) {
        this.fail(statusFromUnknown(error, 'Handling WebRTC drain event raised.'));
      }
    });
  }

  static create(
    connection: RTCPeerConnection,
    dataChannel: RTCDataChannel,
    signalling: SignallingTransport,
    identity: string,
    peerIdentity: string,
    maxBufferedAmount: number,
  ): StatusOr<RtcBinaryChannel> {
    try {
      return new RtcBinaryChannel(
        connection,
        dataChannel,
        signalling,
        identity,
        peerIdentity,
        maxBufferedAmount,
      );
    } catch (error) {
      try { dataChannel.close(); } catch { /* preserve primary status */ }
      try { connection.close(); } catch { /* preserve primary status */ }
      try { signalling.close(); } catch { /* preserve primary status */ }
      return statusFromUnknown(
        error,
        'Configuring WebRTC data channel raised an exception.',
      );
    }
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
      if (this.dataChannel.readyState === 'open') return okStatus();
      if (!this.negotiationStarted) {
        this.negotiationStarted = true;
        queueMicrotask(() => void this.negotiate());
      }
      return await this.opened.promise;
    } catch (error) {
      const status = statusFromUnknown(
        error,
        'Opening WebRTC data channel raised an exception.',
      );
      this.fail(status);
      return status;
    }
  }

  isOpen(): boolean {
    try { return !this.closed && this.dataChannel.readyState === 'open'; }
    catch (error) {
      this.fail(statusFromUnknown(
        error,
        'Reading WebRTC data channel state raised an exception.',
      ));
      return false;
    }
  }

  send(packet: Uint8Array): Status {
    try {
      if (!(packet instanceof Uint8Array)) {
        return invalidArgumentError('WebRTC packet must be a Uint8Array.');
      }
      if (!this.isOpen()) {
        return this.failure ??
          failedPreconditionError('WebRTC data channel is not open.');
      }
      const bufferedAmount = this.bufferedAmount();
      if (!isOk(bufferedAmount)) return bufferedAmount;
      if (bufferedAmount + packet.byteLength > this.maxBufferedAmount) {
        return resourceExhaustedError(
          'WebRTC buffered amount would exceed maxBufferedAmount.',
        );
      }
      this.dataChannel.send(packet.slice().buffer);
      this.scheduleDrainPoll();
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
        : internalError(
            'RTCDataChannel.bufferedAmount returned an invalid value.',
          );
    }
    catch (error) { return statusFromUnknown(error, 'Reading WebRTC buffered amount failed.'); }
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
    if (this.pollTimer !== null) clearTimeout(this.pollTimer);
    const stopStatus = this.failure ?? cancelledError('WebRTC channel closed.');
    for (const waiter of this.drainWaiters.splice(0)) waiter.resolve(stopStatus);
    let first: Status = okStatus();
    try { this.dataChannel.close(); }
    catch (error) { first = statusFromUnknown(error, 'Closing WebRTC data channel failed.'); }
    try { this.connection.close(); }
    catch (error) { if (isOk(first)) first = statusFromUnknown(error, 'Closing WebRTC peer connection failed.'); }
    try {
      const signallingStatus = this.signalling.close();
      first = firstError(first, signallingStatus, 'SignallingTransport.close()');
    } catch (error) {
      first = firstError(
        first,
        statusFromUnknown(error, 'Closing WebRTC signalling raised an exception.'),
        'SignallingTransport.close()',
      );
    }
    return first;
  }

  getImpl(): unknown | null { return this.dataChannel; }

  signalFailure(status: NonOkStatus): void { this.fail(status); }

  private async negotiate(): Promise<void> {
    try {
      const connected = this.signalling.isConnected();
      if (typeof connected !== 'boolean') {
        this.fail(
          internalError(
            'SignallingTransport.isConnected() returned a non-boolean value.',
          ),
        );
        return;
      }
      if (!connected) {
        const signallingStatus = this.signalling.getStatus();
        this.fail(
          !isStatus(signallingStatus)
            ? internalError(
                'SignallingTransport.getStatus() returned a non-Status value.',
              )
            : isOk(signallingStatus)
            ? failedPreconditionError('WebRTC signalling is not connected.')
            : signallingStatus,
        );
        return;
      }
      const offer = await this.connection.createOffer();
      await this.connection.setLocalDescription(offer);
      const local = this.connection.localDescription;
      if (
        local === null ||
        typeof local.sdp !== 'string' ||
        local.sdp.length === 0 ||
        local.type !== 'offer'
      ) {
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
      if (!isOk(message)) {
        this.fail(message);
        return;
      }
      const sent = this.signalling.send(message);
      if (!isStatus(sent)) {
        this.fail(
          internalError('SignallingTransport.send() returned a non-Status value.'),
        );
      } else if (!isOk(sent)) {
        this.fail(sent);
      }
    } catch (error) {
      this.fail(statusFromUnknown(error, 'Creating WebRTC offer raised an exception.'));
    }
  }

  private handleOpen(): void {
    if (this.closed) return;
    this.opened.resolve(okStatus());
    try { this.callbacks?.onOpen(); }
    catch (error) { this.fail(statusFromUnknown(error, 'WebRTC open callback raised an exception.')); }
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
      try { this.callbacks?.onMessage(bytes); }
      catch (error) { this.fail(statusFromUnknown(error, 'WebRTC message callback raised an exception.')); }
    }).catch((error: unknown) => {
      this.fail(statusFromUnknown(error, 'WebRTC message processing failed.'));
    });
  }

  private handleClose(): void {
    const expected = this.closed;
    this.closed = true;
    if (!expected && this.failure === null) {
      this.failure = unavailableError('WebRTC data channel closed unexpectedly.');
      this.opened.resolve(this.failure);
    }
    try { this.callbacks?.onClosed(); } catch { /* lifecycle is already terminal */ }
  }

  private fail(status: NonOkStatus): void {
    if (this.failure !== null) return;
    this.failure = status;
    this.opened.resolve(status);
    try { this.callbacks?.onError(status); } catch { /* avoid event escape */ }
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
    try { this.callbacks?.onBufferedAmountLow(); } catch { /* amount is observable */ }
  }
}

interface ClientContext {
  channel: RtcBinaryChannel;
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
    private readonly channel: RtcBinaryChannel,
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
    let dataChannel: RTCDataChannel;
    try {
      dataChannel = connection.createDataChannel(
        randomId('a11-'),
        normalized.dataChannelOptions,
      );
    } catch (error) {
      try { connection.close(); } catch { /* preserve primary status */ }
      return statusFromUnknown(error, 'Creating WebRTC data channel raised an exception.');
    }
    if (!hasDataChannelShape(dataChannel)) {
      try {
        (dataChannel as unknown as { close?: () => void } | null)?.close?.();
      } catch { /* preserve validation status */ }
      try { connection.close(); } catch { /* preserve validation status */ }
      return invalidArgumentError(
        'RTCPeerConnection.createDataChannel() returned an invalid data channel.',
      );
    }
    const createdChannel = RtcBinaryChannel.create(
      connection,
      dataChannel,
      signalling,
      identity,
      peerIdentity,
      normalized.maxBufferedAmount,
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
