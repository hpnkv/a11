/**
 * Retry policy shared by the demo's connection flows.
 *
 * Claiming an identity, connecting signalling, and dialling a host all fail
 * for a while and then succeed: a service restarts, a signalling socket is
 * replaced, a newly elected host has not finished binding its listener. The
 * policy here is the one `a11.client.hosting` applies to a hosted agent --
 * a short first delay, doubling to a ceiling, bounded by a deadline.
 */

/** First delay after a failure. */
export const MIN_BACKOFF_MS = 500;

/** Ceiling the delay doubles towards. */
export const MAX_BACKOFF_MS = 8_000;

/** Options for {@link retry}. */
export interface RetryOptions {
  /** Total time the operation is given, across all attempts. */
  deadlineMs: number;
  /** First delay after a failure. */
  minBackoffMs?: number;
  /** Ceiling the delay doubles towards. */
  maxBackoffMs?: number;
  /** Called with every failure, for logging. */
  onAttemptFailed?: (error: unknown, attempt: number) => void;
  /** Abandon the operation when this returns true. */
  cancelled?: () => boolean;
}

/** Sleep for `ms`, resolving early for nobody. */
function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/** Error thrown when the deadline passes with every attempt failed. */
export class RetriesExhausted extends Error {
  constructor(readonly attempts: number, readonly last: unknown) {
    super(`gave up after ${attempts} attempt(s): ${String(last)}`);
    this.name = 'RetriesExhausted';
  }
}

/**
 * Run `operation` until it resolves, the deadline passes, or the caller
 * cancels.
 *
 * The last failure travels in {@link RetriesExhausted.last}. A cancelled
 * retry rejects with the same error, so one `catch` covers both endings.
 */
export async function retry<T>(
  operation: (attempt: number) => Promise<T>,
  options: RetryOptions,
): Promise<T> {
  const minBackoff = options.minBackoffMs ?? MIN_BACKOFF_MS;
  const maxBackoff = options.maxBackoffMs ?? MAX_BACKOFF_MS;
  const deadline = Date.now() + options.deadlineMs;

  let backoff = minBackoff;
  let attempt = 0;
  let last: unknown = new Error('no attempt ran');

  while (!(options.cancelled?.() ?? false)) {
    attempt++;
    try {
      return await operation(attempt);
    } catch (error) {
      last = error;
      options.onAttemptFailed?.(error, attempt);
    }
    const remaining = deadline - Date.now();
    if (remaining <= 0) break;
    await sleep(Math.min(backoff, remaining));
    backoff = Math.min(backoff * 2, maxBackoff);
  }
  throw new RetriesExhausted(attempt, last);
}
