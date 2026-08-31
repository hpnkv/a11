/**
 * WebRTC connection type monitor.
 *
 * Polls `RTCPeerConnection.getStats()` every few seconds to determine
 * whether the active ICE candidate pair uses a direct path (STUN) or
 * a relayed path (TURN). Emits updates via a callback so the UI can
 * display per-peer connection badges.
 */

import { ConnectionType, DISCONNECT_GRACE_MS } from './types.js';

/** How often to poll connection stats (milliseconds). */
const POLL_INTERVAL_MS = 3_000;

/** Callback signature for connection type changes. */
export type OnConnectionTypeChange = (
  peerId: string,
  type: ConnectionType,
) => void;

/**
 * Monitors one `RTCPeerConnection` and classifies its active path.
 *
 * Create one per peer connection. Call {@link start} to begin polling
 * and {@link stop} to cease. The monitor handles `getStats()` returning
 * no selected pair gracefully by reporting {@link ConnectionType.UNKNOWN}.
 */
export class ConnectionMonitor {
  private timer: ReturnType<typeof setInterval> | null = null;
  private lastType: ConnectionType = ConnectionType.UNKNOWN;

  constructor(
    private readonly peerId: string,
    private readonly connection: RTCPeerConnection,
    private readonly onChange: OnConnectionTypeChange,
  ) {}

  /** Begin periodic polling. */
  start(): void {
    if (this.timer !== null) return;
    // Fire once immediately, then on interval.
    void this.poll();
    this.timer = setInterval(() => void this.poll(), POLL_INTERVAL_MS);
  }

  /** Stop polling and release the timer. */
  stop(): void {
    if (this.timer !== null) {
      clearInterval(this.timer);
      this.timer = null;
    }
  }

  private async poll(): Promise<void> {
    try {
      const type = await classifyConnection(this.connection);
      if (type !== this.lastType) {
        this.lastType = type;
        this.onChange(this.peerId, type);
      }
    } catch {
      // getStats() can throw if the connection is closing; ignore.
    }
  }
}

/**
 * Inspect the active candidate pair and classify the connection type.
 *
 * Looks for a `candidate-pair` report in the `succeeded` state, then
 * checks the local candidate's type. `relay` means TURN; anything else
 * (host, srflx, prflx) means a direct path.
 */
async function classifyConnection(
  connection: RTCPeerConnection,
): Promise<ConnectionType> {
  const stats = await connection.getStats();

  // Find the active (succeeded/nominated) candidate pair.
  let localCandidateId: string | null = null;
  stats.forEach((report) => {
    if (
      report.type === 'candidate-pair' &&
      (report.state === 'succeeded' || report.nominated === true) &&
      localCandidateId === null
    ) {
      localCandidateId = report.localCandidateId ?? null;
    }
  });

  if (localCandidateId === null) return ConnectionType.UNKNOWN;

  // Look up the local candidate to check its type.
  const localCandidate = stats.get(localCandidateId);
  if (localCandidate === undefined) return ConnectionType.UNKNOWN;

  const candidateType: string = localCandidate.candidateType ?? '';
  if (candidateType === 'relay') return ConnectionType.TURN;
  if (
    candidateType === 'host' ||
    candidateType === 'srflx' ||
    candidateType === 'prflx'
  ) {
    return ConnectionType.DIRECT;
  }

  return ConnectionType.UNKNOWN;
}

/**
 * Report a peer connection that has gone away.
 *
 * `failed` and `closed` are terminal. `disconnected` is not: ICE recovers
 * from a brief interruption, so it is given {@link DISCONNECT_GRACE_MS}
 * before it counts. A closed tab or a dropped network shows here in under a
 * second; the framing layer's idle timeout is the backstop behind it.
 */
export function watchPeerConnection(
  connection: RTCPeerConnection,
  onGone: () => void,
  setTimer: (timer: ReturnType<typeof setTimeout> | null) => void,
  getTimer: () => ReturnType<typeof setTimeout> | null,
): void {
  const clear = (): void => {
    const timer = getTimer();
    if (timer !== null) clearTimeout(timer);
    setTimer(null);
  };
  const check = (): void => {
    switch (connection.connectionState) {
      case 'failed':
      case 'closed':
        clear();
        onGone();
        break;
      case 'disconnected':
        if (getTimer() !== null) break;
        setTimer(setTimeout(() => {
          setTimer(null);
          if (connection.connectionState !== 'connected') onGone();
        }, DISCONNECT_GRACE_MS));
        break;
      default:
        clear();
        break;
    }
  };
  connection.addEventListener('connectionstatechange', check);
  check();
}
