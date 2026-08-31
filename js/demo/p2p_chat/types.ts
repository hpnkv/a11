/** Shared types for the P2P chat room demo. */

/** How a peer is connected to the host. */
export enum ConnectionType {
  /** Direct path via STUN (host/srflx/prflx candidate). */
  DIRECT = 'direct',
  /** Relayed through a TURN server. */
  TURN = 'turn',
  /** Connection type not yet determined. */
  UNKNOWN = 'unknown',
}

/** A single event in the room's replicated log. */
export type RoomEvent =
  | { type: 'join'; peerId: string; timestamp: number }
  | { type: 'leave'; peerId: string; timestamp: number }
  | { type: 'message'; peerId: string; text: string; timestamp: number }
  | { type: 'name_change'; peerId: string; name: string; timestamp: number };

/** A chat message derived from the event log. */
export interface ChatMessage {
  peerId: string;
  text: string;
  timestamp: number;
}

/** Runtime information about one connected peer. */
export interface PeerInfo {
  peerId: string;
  name: string | null;
  connectionType: ConnectionType;
  isHost: boolean;
}

/** Complete room state derived from the event log. */
export interface RoomState {
  /** Active peer IDs mapped to their display names (null = unnamed). */
  participants: Map<string, string | null>;
  /** Chat messages in chronological order, capped at the message limit. */
  messages: ChatMessage[];
}

/**
 * Result returned by the anonymous claim endpoint.
 *
 * Contains only the caller's own identity and signalling credentials.
 * No room information, no peer list — the exchange never learns which
 * room this identity belongs to or who else is in it.
 */
export interface AnonymousClaimResult {
  peerId: string;
  signallingUrl: string;
  claimToken: string;
  iceServers: RTCIceServer[];
}

/**
 * Idle time on a peer connection after which the peer counts as gone.
 *
 * Passed to the wire stream as `messageTimeoutMs`, so the stream aborts
 * itself once this long passes with no framing activity in either
 * direction, and to the session as `noStreamTimeoutMs`.
 */
export const PEER_TIMEOUT_MS = 10_000;

/**
 * Interval between liveness pings from a peer to its host.
 *
 * Three pings fit inside {@link PEER_TIMEOUT_MS}. The `__ping` builtin
 * answers them, so neither side registers an action for it.
 */
export const PING_INTERVAL_MS = 3_000;

/**
 * Grace given to a peer connection reporting `disconnected`.
 *
 * ICE recovers from a brief interruption on its own; a connection still
 * disconnected after this counts as gone.
 */
export const DISCONNECT_GRACE_MS = 2_000;

/** How long a host keeps trying to bind its signalling listener. */
export const LISTEN_DEADLINE_MS = 20_000;

/**
 * How much claim life a peer needs before it dials with the one it holds.
 *
 * The exchange refuses a signalling connection under a lapsed claim and has
 * no anonymous renewal, so a peer with less than this left takes a fresh
 * identity rather than spending its attempts on a rejection.
 */
export const CLAIM_MARGIN_MS = 30_000;

/** How long a peer keeps dialling one host before electing another. */
export const RECONNECT_DEADLINE_MS = 15_000;

/**
 * How long a liveness ping may take before its host counts as gone.
 *
 * Short enough that a wedged host is noticed within a couple of intervals,
 * long enough to survive one slow relay round trip.
 */
export const PING_TIMEOUT_MS = 4_000;

/**
 * How long the check after a data channel closes waits for an answer.
 *
 * A closed channel is already strong evidence, so this only has to outlast
 * one round trip on a channel the multiplex may have just replaced.
 */
export const CONFIRM_TIMEOUT_MS = 1_500;

/** How often a signalling connection is checked and, if down, re-made. */
export const SIGNALLING_POLL_MS = 3_000;

/**
 * How long before TURN credentials lapse to fetch replacements.
 *
 * Anonymous TURN credentials last ten minutes and carry their expiry in the
 * username. Once it passes, the endpoint holding them gathers no relay
 * candidates at all, so a room that outlives them loses every path that
 * needs a relay.
 */
export const ICE_REFRESH_MARGIN_MS = 120_000;

/** Maximum characters per chat message. */
export const MAX_MESSAGE_LENGTH = 2000;

/** Maximum number of chat messages retained in the event log. */
export const MAX_MESSAGE_HISTORY = 300;
