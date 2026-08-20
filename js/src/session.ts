import { Action } from './action.js';
import { ActionRegistry } from './action_registry.js';
import {
  ACTION_DISPATCH_STATUS_OUTPUT,
  ACTION_STATUS_OUTPUT,
  CANCEL_ACTION_HEADER,
  CANCEL_ACTION_NAME,
  decodeStatusChunk,
  isCloseStatusChunk,
  isStatusChunk,
  statusToChunk,
} from './action_schema.js';
import { NodeMap } from './async_node.js';
import {
  copyByteMap,
  randomId,
  utf8Decode,
  type ByteMap,
  type ByteMapInput,
} from './bytes.js';
import { Deferred } from './concurrency.js';
import {
  NodeFragment,
  WireMessage,
  validateName,
} from './data.js';
import {
  abortedError,
  alreadyExistsError,
  cancelledError,
  deadlineExceededError,
  failedPreconditionError,
  internalError,
  invalidArgumentError,
  isOk,
  isStatus,
  notFoundError,
  okStatus,
  outOfRangeError,
  statusFromUnknown,
  statusToJson,
  unknownError,
  type NonOkStatus,
  type Status,
  type StatusOr,
} from './status.js';
import { decodeStatus, packStatus } from './status_codec.js';
import {
  ABORT_STATUS_HEADER,
  MAX_SINGLE_WIRE_MESSAGE_SIZE,
  normalizeWireHeaders,
  wireDeadlineMillis,
  type WireDeadline,
  type WireStream,
} from './wire_stream.js';

/** Terminal trailer/header carrying a session's structured completion status. */
export const SESSION_STATUS_HEADER = 'x-a11-session-status';
/** Hard upper bound for one message admitted by a session. */
export const MAX_SINGLE_MESSAGE_SIZE = MAX_SINGLE_WIRE_MESSAGE_SIZE;

const SESSION_STREAM_ABORT_MESSAGE = 'Session has aborted its streams';

/** Which endpoint role a session asks an attached stream to drive. */
export enum StreamMode {
  START = 'start',
  ACCEPT = 'accept',
}

/** Buffer, concurrency, and lifetime bounds for one agent connection. */
export interface SessionOptions {
  /** Messages buffered across every attached stream. */
  maxBufferedMessagesTotal?: number;
  /** Messages buffered for any one attached stream. */
  maxBufferedMessagesPerStream?: number;
  /** Top-level action handlers allowed to run concurrently. */
  maxConcurrentRootActions?: number;
  /** Child action handlers allowed to run concurrently. */
  maxConcurrentNestedActions?: number;
  /** Maximum encoded size of one incoming wire message. */
  maxSingleMessageSize?: number;
  /** Encoded bytes buffered across every attached stream. */
  maxBufferedBytesTotal?: number;
  /** Encoded bytes buffered for any one attached stream. */
  maxBufferedBytesPerStream?: number;
  /** Grace period with no streams before clean half-close; `null` disables it. */
  noStreamTimeoutMs?: number | null;
  /** Absolute connection deadline. */
  deadline?: WireDeadline;
}

/** Validated, default-filled form of {@link SessionOptions}. */
export interface NormalizedSessionOptions {
  maxBufferedMessagesTotal: number;
  maxBufferedMessagesPerStream: number;
  maxConcurrentRootActions: number;
  maxConcurrentNestedActions: number;
  maxSingleMessageSize: number;
  maxBufferedBytesTotal: number;
  maxBufferedBytesPerStream: number;
  noStreamTimeoutMs: number | null;
  deadline: number | null;
}

/**
 * Consumes one inbound message with its transport and owning session.
 * `null` marks that transport's remote half-close.
 */
export type OnSessionStreamMessage = (
  message: WireMessage | null,
  stream: WireStream,
  session: Session,
) => void | Status | Promise<void | Status>;

/** Runs after an attached stream has completely terminated. */
export type OnSessionStreamDone = (
  stream: WireStream,
  session: Session,
) => void | Status | Promise<void | Status>;

/** Identity, callbacks, and shared registries used to create a Session. */
export interface SessionCreateOptions extends SessionOptions {
  id?: string;
  onStreamMessage?: OnSessionStreamMessage;
  onStreamDone?: OnSessionStreamDone;
  headers?: ByteMapInput;
  nodeMap?: NodeMap;
  actionRegistry?: ActionRegistry | null;
}

interface InitializedSession {
  id: string;
  headers: ByteMap;
  options: NormalizedSessionOptions;
  nodeMap: NodeMap;
  actionRegistry: ActionRegistry | null;
  onStreamMessage?: OnSessionStreamMessage;
  onStreamDone?: OnSessionStreamDone;
}

interface BufferedMessage {
  message: WireMessage;
  size: number;
}

interface SessionStreamState {
  stream: WireStream;
  id: string;
  outstandingMessages: number;
  outstandingBytes: number;
  pendingMessages: BufferedMessage[];
  messagePumpRunning: boolean;
  acceptingMessages: boolean;
  remoteHalfClosed: boolean;
  halfCloseDelivered: boolean;
  doneStarted: boolean;
  done: boolean;
}

interface DispatchFailure {
  elementType: 'action_message' | 'node_fragment';
  elementIndex: number;
  status: NonOkStatus;
}

interface ActionWaiter {
  deferred: Deferred<Status>;
  signal?: AbortSignal;
  onAbort?: () => void;
}

class ActionLimiter {
  private active = 0;
  private readonly waiters: ActionWaiter[] = [];
  private terminalStatus: NonOkStatus | null = null;

  constructor(private readonly maximum: number) {}

  acquire(signal?: AbortSignal): Promise<Status> {
    if (this.terminalStatus !== null) return Promise.resolve(this.terminalStatus);
    if (signal?.aborted) {
      return Promise.resolve(cancelledError('Action was cancelled while waiting to run.'));
    }
    if (this.active < this.maximum) {
      ++this.active;
      return Promise.resolve(okStatus());
    }
    const waiter: ActionWaiter = { deferred: new Deferred<Status>(), signal };
    if (signal !== undefined) {
      waiter.onAbort = () => {
        const index = this.waiters.indexOf(waiter);
        if (index < 0) return;
        this.waiters.splice(index, 1);
        waiter.deferred.resolve(
          cancelledError('Action was cancelled while waiting to run.'),
        );
      };
      try {
        signal.addEventListener('abort', waiter.onAbort, { once: true });
      } catch {
        // AbortSignal implementations in supported runtimes provide this API.
      }
    }
    this.waiters.push(waiter);
    return waiter.deferred.promise;
  }

  release(): void {
    if (this.active > 0) --this.active;
    while (this.waiters.length > 0 && this.terminalStatus === null) {
      const waiter = this.waiters.shift()!;
      this.removeAbortListener(waiter);
      if (waiter.signal?.aborted) {
        waiter.deferred.resolve(
          cancelledError('Action was cancelled while waiting to run.'),
        );
        continue;
      }
      ++this.active;
      waiter.deferred.resolve(okStatus());
      break;
    }
  }

  cancel(status: NonOkStatus): void {
    if (this.terminalStatus !== null) return;
    this.terminalStatus = status;
    for (const waiter of this.waiters.splice(0)) {
      this.removeAbortListener(waiter);
      waiter.deferred.resolve(status);
    }
  }

  private removeAbortListener(waiter: ActionWaiter): void {
    if (waiter.signal === undefined || waiter.onAbort === undefined) return;
    try {
      waiter.signal.removeEventListener('abort', waiter.onAbort);
    } catch {
      // Removing an already-removed listener is harmless.
    }
  }
}

function isPositiveIntegerInRange(
  value: number,
  maximum: number,
): boolean {
  return Number.isSafeInteger(value) && value >= 1 && value <= maximum;
}

/** Validate session limits and apply defaults before timers are scheduled. */
export function normalizeSessionOptions(
  options: SessionOptions = {},
): StatusOr<NormalizedSessionOptions> {
  try {
    const empty = new WireMessage().toMsgpack();
    if (!isOk(empty)) return empty;
    const deadline = wireDeadlineMillis(options.deadline);
    if (!isOk(deadline)) return deadline;
    const result: NormalizedSessionOptions = {
      maxBufferedMessagesTotal: options.maxBufferedMessagesTotal ?? 256,
      maxBufferedMessagesPerStream:
        options.maxBufferedMessagesPerStream ?? 32,
      maxConcurrentRootActions: options.maxConcurrentRootActions ?? 32,
      maxConcurrentNestedActions: options.maxConcurrentNestedActions ?? 128,
      maxSingleMessageSize:
        options.maxSingleMessageSize ?? MAX_SINGLE_MESSAGE_SIZE,
      maxBufferedBytesTotal:
        options.maxBufferedBytesTotal ?? 32 * 1024 * 1024,
      maxBufferedBytesPerStream:
        options.maxBufferedBytesPerStream ?? 4 * 1024 * 1024,
      noStreamTimeoutMs: options.noStreamTimeoutMs === undefined
        ? 30_000
        : options.noStreamTimeoutMs,
      deadline,
    };
    if (
      !isPositiveIntegerInRange(result.maxBufferedMessagesTotal, 1024) ||
      !isPositiveIntegerInRange(result.maxBufferedMessagesPerStream, 1024)
    ) {
      return invalidArgumentError(
        'Session message limits must be integers between 1 and 1024.',
      );
    }
    if (
      !isPositiveIntegerInRange(result.maxConcurrentRootActions, 65_536) ||
      !isPositiveIntegerInRange(result.maxConcurrentNestedActions, 65_536)
    ) {
      return invalidArgumentError(
        'Session Action limits must be integers between 1 and 65536.',
      );
    }
    if (
      !Number.isSafeInteger(result.maxSingleMessageSize) ||
      result.maxSingleMessageSize < empty.byteLength ||
      result.maxSingleMessageSize > MAX_SINGLE_MESSAGE_SIZE
    ) {
      return invalidArgumentError('Invalid maxSingleMessageSize.');
    }
    if (
      !Number.isSafeInteger(result.maxBufferedBytesTotal) ||
      result.maxBufferedBytesTotal < empty.byteLength ||
      !Number.isSafeInteger(result.maxBufferedBytesPerStream) ||
      result.maxBufferedBytesPerStream < empty.byteLength
    ) {
      return invalidArgumentError(
        'Session byte limits are smaller than an empty WireMessage.',
      );
    }
    if (
      result.noStreamTimeoutMs !== null &&
      (!Number.isFinite(result.noStreamTimeoutMs) ||
        result.noStreamTimeoutMs < 0)
    ) {
      return invalidArgumentError(
        'noStreamTimeoutMs must be a non-negative finite number or null.',
      );
    }
    return result;
  } catch (error) {
    return statusFromUnknown(error, 'Validating Session options raised an exception.');
  }
}

/** Validate session metadata, lowercase names, and copy byte values. */
export function normalizeSessionHeaders(
  headers: ByteMapInput | undefined = undefined,
): StatusOr<ByteMap> {
  return normalizeWireHeaders(headers);
}

function initializeSession(
  options: SessionCreateOptions,
): StatusOr<InitializedSession> {
  try {
    const normalizedOptions = normalizeSessionOptions(options);
    if (!isOk(normalizedOptions)) return normalizedOptions;
    const headers = normalizeSessionHeaders(options.headers);
    if (!isOk(headers)) return headers;
    const id = options.id || randomId('session-');
    const validId = validateName(id);
    if (!isOk(validId)) return validId;
    if (
      options.onStreamMessage !== undefined &&
      typeof options.onStreamMessage !== 'function'
    ) {
      return invalidArgumentError('onStreamMessage must be callable.');
    }
    if (
      options.onStreamDone !== undefined &&
      typeof options.onStreamDone !== 'function'
    ) {
      return invalidArgumentError('onStreamDone must be callable.');
    }
    if (options.nodeMap !== undefined && !(options.nodeMap instanceof NodeMap)) {
      return invalidArgumentError('nodeMap must be a NodeMap.');
    }
    if (
      options.actionRegistry !== undefined &&
      options.actionRegistry !== null &&
      !(options.actionRegistry instanceof ActionRegistry)
    ) {
      return invalidArgumentError(
        'actionRegistry must be an ActionRegistry or null.',
      );
    }
    return {
      id,
      headers,
      options: normalizedOptions,
      nodeMap: options.nodeMap ?? new NodeMap(),
      actionRegistry: options.actionRegistry ?? null,
      onStreamMessage: options.onStreamMessage,
      onStreamDone: options.onStreamDone,
    };
  } catch (error) {
    return statusFromUnknown(error, 'Creating Session configuration raised an exception.');
  }
}

function hasWireStreamShape(value: unknown): value is WireStream {
  if (typeof value !== 'object' || value === null) return false;
  try {
    const candidate = value as Record<string, unknown>;
    return [
      'send',
      'start',
      'accept',
      'halfClose',
      'drainOutgoingMessages',
      'abort',
      'setDeadline',
      'getDeadline',
      'getStatus',
      'getTrailers',
      'getId',
      'getImpl',
      'wait',
    ].every((name) => typeof candidate[name] === 'function');
  } catch {
    return false;
  }
}

function firstError(first: Status, candidate: unknown): Status {
  if (!isStatus(candidate)) {
    return isOk(first)
      ? internalError('A Status-returning operation returned an invalid value.')
      : first;
  }
  return isOk(first) && !isOk(candidate) ? candidate : first;
}

function sessionStreamAbortStatus(): NonOkStatus {
  return abortedError(SESSION_STREAM_ABORT_MESSAGE);
}

function isSessionStreamAbortStatus(status: Status): boolean {
  const expected = sessionStreamAbortStatus();
  return status.code === expected.code && status.message === expected.message;
}

function specialActionNode(nodeId: string): [string, string] | null {
  for (const name of [ACTION_DISPATCH_STATUS_OUTPUT, ACTION_STATUS_OUTPUT]) {
    const suffix = `#${name}`;
    if (nodeId.length > suffix.length && nodeId.endsWith(suffix)) {
      return [nodeId.slice(0, -suffix.length), name];
    }
  }
  return null;
}

function aggregateDispatchFailures(
  failures: readonly DispatchFailure[],
  total: number,
): NonOkStatus {
  const first = failures[0]!.status;
  const sameCode = failures.every((failure) => failure.status.code === first.code);
  const base = sameCode ? first : unknownError();
  return {
    ...base,
    message: `Failed to dispatch ${failures.length} of ${total} WireMessage elements.`,
    details: failures.map((failure) => ({
      element_type: failure.elementType,
      element_index: failure.elementIndex,
      status: statusToJson(failure.status),
    })),
  };
}

async function invokeSessionMessageCallback(
  callback: OnSessionStreamMessage,
  message: WireMessage | null,
  stream: WireStream,
  session: Session,
): Promise<Status> {
  try {
    const result = await callback(message, stream, session);
    if (result === undefined) return okStatus();
    return isStatus(result)
      ? result
      : internalError('Session message callback returned a non-Status value.');
  } catch (error) {
    return statusFromUnknown(error, 'Session message callback raised an exception.');
  }
}

async function invokeSessionDoneCallback(
  callback: OnSessionStreamDone,
  stream: WireStream,
  session: Session,
): Promise<Status> {
  try {
    const result = await callback(stream, session);
    if (result === undefined) return okStatus();
    return isStatus(result)
      ? result
      : internalError('Session done callback returned a non-Status value.');
  } catch (error) {
    return statusFromUnknown(error, 'Session done callback raised an exception.');
  }
}

/**
 * Connection-scoped runtime that turns wire traffic into agent work.
 *
 * A session owns a shared {@link NodeMap}, optional {@link ActionRegistry}, one
 * or more {@link WireStream}s, and every action running across them. Incoming
 * WireMessages are split into action calls and node fragments; bounded,
 * per-stream pumps preserve backpressure while handlers run asynchronously.
 * Outgoing messages may target a stream explicitly or use round-robin routing.
 *
 * The session starts open. {@link halfClose} stops new sends/actions and asks
 * every transport to drain normally. {@link abort} cancels actions and carries
 * a structured failure to peers. The promise returned by {@link done} resolves
 * only after all attached stream state has been removed; `isClosed()` can
 * therefore become true before `isDone()`.
 */
export class Session {
  private readonly id: string;
  private readonly headers: ByteMap;
  private readonly options: NormalizedSessionOptions;
  private nodeMap: NodeMap;
  private actionRegistry: ActionRegistry | null;
  private onStreamMessage: OnSessionStreamMessage;
  private onStreamDone: OnSessionStreamDone;
  private readonly streamStates = new Map<WireStream, SessionStreamState>();
  private readonly streamsById = new Map<string, SessionStreamState>();
  private readonly streamOrder: WireStream[] = [];
  private roundRobinIndex = 0;
  private bufferedMessages = 0;
  private bufferedBytes = 0;
  private readonly activeActions = new Map<string, Action>();
  private readonly rootLimiter: ActionLimiter;
  private readonly nestedLimiter: ActionLimiter;
  private phase: 'open' | 'closing' | 'aborted' = 'open';
  private sessionStatus: Status = okStatus();
  private remoteClosed = false;
  private destroyed = false;
  private readonly doneDeferred = new Deferred<Status>();
  private stateWaiters: Array<Deferred<void>> = [];
  private noStreamTimer: ReturnType<typeof setTimeout> | null = null;
  private deadlineTimer: ReturnType<typeof setTimeout> | null = null;

  protected constructor(initialized: InitializedSession) {
    this.id = initialized.id;
    this.headers = initialized.headers;
    this.options = initialized.options;
    this.nodeMap = initialized.nodeMap;
    this.actionRegistry = initialized.actionRegistry;
    this.rootLimiter = new ActionLimiter(
      initialized.options.maxConcurrentRootActions,
    );
    this.nestedLimiter = new ActionLimiter(
      initialized.options.maxConcurrentNestedActions,
    );
    this.onStreamMessage = initialized.onStreamMessage ??
      (async (message, stream) =>
        message === null
          ? okStatus()
          : this.dispatchWireMessage(message, stream));
    this.onStreamDone = initialized.onStreamDone ?? (() => okStatus());
    this.scheduleDeadlineTimer();
    this.scheduleNoStreamTimer();
  }

  /** Create an open session and start deadline/no-stream supervision. */
  static create(options: SessionCreateOptions = {}): StatusOr<Session> {
    try {
      const initialized = initializeSession(options);
      return isOk(initialized) ? new Session(initialized) : initialized;
    } catch (error) {
      return statusFromUnknown(error, 'Constructing Session raised an exception.');
    }
  }

  protected setStreamCallbacks(
    onMessage: OnSessionStreamMessage,
    onDone: OnSessionStreamDone,
  ): Status {
    if (typeof onMessage !== 'function' || typeof onDone !== 'function') {
      return invalidArgumentError('Session stream callbacks must be callable.');
    }
    this.onStreamMessage = onMessage;
    this.onStreamDone = onDone;
    return okStatus();
  }

  getId(): string { return this.id; }
  getHeaders(): ByteMap { return copyByteMap(this.headers); }
  getOptions(): NormalizedSessionOptions { return { ...this.options }; }
  getNodeMap(): NodeMap { return this.nodeMap; }
  getActionRegistry(): ActionRegistry | null { return this.actionRegistry; }

  /**
   * Replace the node map and rebind active actions.
   * Existing fragments remain in the old map, so prefer configuring this
   * before traffic starts rather than splitting a live action's state.
   */
  setNodeMap(nodeMap: NodeMap): Status {
    if (!(nodeMap instanceof NodeMap)) {
      return invalidArgumentError('nodeMap must be a NodeMap.');
    }
    this.nodeMap = nodeMap;
    let first: Status = okStatus();
    for (const action of this.activeActions.values()) {
      first = firstError(first, action.bindNodeMap(nodeMap));
    }
    return first;
  }

  /**
   * Replace the registry and rebind active actions for later name resolution.
   * Prefer configuring it before dispatch so one operation does not observe
   * registrations from different registry versions.
   */
  setActionRegistry(registry: ActionRegistry | null): Status {
    if (registry !== null && !(registry instanceof ActionRegistry)) {
      return invalidArgumentError(
        'registry must be an ActionRegistry or null.',
      );
    }
    this.actionRegistry = registry;
    let first: Status = okStatus();
    for (const action of this.activeActions.values()) {
      first = firstError(first, action.bindRegistry(registry));
    }
    return first;
  }

  /** Snapshot the streams currently attached to this session. */
  streams(): StatusOr<Array<[string, WireStream]>> {
    try {
      const result: Array<[string, WireStream]> = [];
      for (const stream of this.streamOrder) {
        const state = this.streamStates.get(stream);
        if (state !== undefined) result.push([state.id, state.stream]);
      }
      return result;
    } catch (error) {
      return statusFromUnknown(error, 'Listing Session streams raised an exception.');
    }
  }

  getStream(streamId: string): StatusOr<WireStream> {
    try {
      const state = this.streamsById.get(streamId);
      return state?.stream ?? notFoundError(
        `Stream '${streamId}' is not attached to the Session.`,
      );
    } catch (error) {
      return statusFromUnknown(error, 'Looking up Session stream raised an exception.');
    }
  }

  /** Snapshot actions currently tracked as in-flight. */
  actions(): Array<[string, Action]> {
    try { return [...this.activeActions]; }
    catch { return []; }
  }

  getAction(actionId: string): StatusOr<Action> {
    try {
      return this.activeActions.get(actionId) ?? notFoundError(
        `Action '${actionId}' is not active in the Session.`,
      );
    } catch (error) {
      return statusFromUnknown(error, 'Looking up Session Action raised an exception.');
    }
  }

  /** Request cooperative cancellation of one active action. */
  cancelAction(actionId: string): Status {
    const action = this.getAction(actionId);
    return isOk(action) ? action.cancel() : action;
  }

  /** Request cancellation of every action without waiting for teardown. */
  cancelAllActions(): Status {
    let first: Status = okStatus();
    for (const action of this.activeActions.values()) {
      first = firstError(first, action.cancel());
    }
    return first;
  }

  /** Await all actions observed during the wait and aggregate their failures. */
  async awaitAllActions(timeoutMs?: number): Promise<Status> {
    try {
      if (
        timeoutMs !== undefined &&
        (!Number.isFinite(timeoutMs) || timeoutMs < 0)
      ) {
        return invalidArgumentError(
          'timeoutMs must be a non-negative finite number.',
        );
      }
      const deadline = timeoutMs === undefined ? null : Date.now() + timeoutMs;
      const observed = new Set<Action>();
      const failures: NonOkStatus[] = [];
      while (true) {
        const pending = [...this.activeActions.values()].filter(
          (action) => !observed.has(action),
        );
        if (pending.length === 0) break;
        for (const action of pending) {
          observed.add(action);
          const remaining = deadline === null
            ? undefined
            : Math.max(0, deadline - Date.now());
          const result = await action.wait(remaining);
          if (!isOk(result)) {
            if (!action.isDone() && result.code === deadlineExceededError().code) {
              return result;
            }
            failures.push(result);
          }
        }
      }
      if (failures.length === 0) return okStatus();
      const code = failures.every((failure) => failure.code === failures[0]!.code)
        ? failures[0]!.code
        : unknownError().code;
      const base = code === failures[0]!.code ? failures[0]! : unknownError();
      return {
        ...base,
        message: `${failures.length} Actions completed with errors.`,
        details: failures.map((status) => ({ status: statusToJson(status) })),
      };
    } catch (error) {
      return statusFromUnknown(error, 'Waiting for Session Actions raised an exception.');
    }
  }

  /** Track an action so concurrency, cancellation, and shutdown include it. */
  trackAction(action: Action): Status {
    if (!(action instanceof Action)) {
      return invalidArgumentError('action must be an Action.');
    }
    if (this.phase !== 'open' || this.remoteClosed) {
      return failedPreconditionError(
        'Session is no longer accepting Actions.',
      );
    }
    const id = action.getId();
    const found = this.activeActions.get(id);
    if (found !== undefined && found !== action) {
      return alreadyExistsError(
        `Action '${id}' already exists in the Session.`,
      );
    }
    this.activeActions.set(id, action);
    this.notifyStateChanged();
    return okStatus();
  }

  untrackAction(action: Action): void {
    try {
      const id = action.getId();
      if (this.activeActions.get(id) === action) {
        this.activeActions.delete(id);
        this.notifyStateChanged();
      }
    } catch {
      // Session tracking must never surface cleanup exceptions.
    }
  }

  acquireActionSlot(nested: boolean, signal?: AbortSignal): Promise<Status> {
    try {
      return (nested ? this.nestedLimiter : this.rootLimiter).acquire(signal);
    } catch (error) {
      return Promise.resolve(
        statusFromUnknown(error, 'Acquiring a Session Action slot raised.'),
      );
    }
  }

  releaseActionSlot(nested: boolean): void {
    try { (nested ? this.nestedLimiter : this.rootLimiter).release(); }
    catch { /* A limiter release is best-effort cleanup. */ }
  }

  /** Apply a received fragment, including reserved action-status nodes. */
  async dispatchNodeFragment(
    fragment: NodeFragment,
  ): Promise<StatusOr<number>> {
    try {
      if (!(fragment instanceof NodeFragment)) {
        return invalidArgumentError('fragment must be a NodeFragment.');
      }
      const validation = fragment.validate();
      if (!isOk(validation)) return validation;
      const special = specialActionNode(fragment.id);
      const chunk = fragment.getChunk();
      let protocolStatus: Status | null = null;
      let action: Action | null = null;

      // A closure marker reports that the peer drained the node and closed its
      // write half; it carries no value, so it is applied to the local mirror
      // rather than stored. Checked before the reserved status nodes so that
      // closing an Action's status node is not read as a second status value.
      if (isOk(chunk) && isCloseStatusChunk(chunk)) {
        const decoded = decodeStatusChunk(chunk);
        if (!isOk(decoded)) return decoded;
        const seq = fragment.seq ?? 0;
        // Dropping a marker for a released node loses nothing, whereas creating
        // one would resurrect it.
        const mirror = this.nodeMap.getIfExists(fragment.id);
        if (!isOk(mirror)) return mirror;
        if (mirror === null) return seq;
        const writable = await mirror.isWritable();
        if (!isOk(writable)) return writable;
        if (!writable) return seq;
        const applied = isOk(decoded.status)
          ? await mirror.close()
          : await mirror.abortWithStatus(decoded.status);
        if (!isOk(applied)) return applied;
        return seq;
      }

      if (special !== null) {
        if (!isOk(chunk) || !isStatusChunk(chunk)) {
          return invalidArgumentError(
            'An Action status node requires a status Chunk.',
          );
        }
        const decoded = decodeStatusChunk(chunk);
        if (!isOk(decoded)) return decoded;
        protocolStatus = decoded.status;
        const found = this.getAction(special[0]);
        if (!isOk(found)) {
          return notFoundError('Received status for an unknown Action.');
        }
        action = found;
      }

      const node = await this.nodeMap.get(fragment.id);
      if (!isOk(node)) return node;
      if (isOk(chunk) && isStatusChunk(chunk) && special === null) {
        const decoded = decodeStatusChunk(chunk);
        if (!isOk(decoded)) return decoded;
        if (isOk(decoded.status)) {
          return invalidArgumentError(
            'An ordinary node cannot be aborted with an OK status.',
          );
        }
        const writable = await node.isWritable();
        if (!isOk(writable)) return writable;
        if (writable) {
          const closed = await node.abortWithStatus(decoded.status);
          if (!isOk(closed)) return closed;
        }
        return fragment.seq ?? 0;
      }
      if (node.getWriterAbortStatus() !== null) return fragment.seq ?? 0;
      const stored = await node.putFragment(fragment);
      if (!isOk(stored)) {
        const abortStatus = node.getWriterAbortStatus();
        if (
          abortStatus !== null &&
          abortStatus.code === stored.code &&
          abortStatus.message === stored.message
        ) {
          return fragment.seq ?? 0;
        }
        return stored;
      }
      if (special !== null && action !== null && protocolStatus !== null) {
        const applied = special[1] === ACTION_DISPATCH_STATUS_OUTPUT
          ? action.setDispatchStatus(protocolStatus)
          : action.setCompletionStatus(protocolStatus);
        if (!isOk(applied)) return applied;
      }
      return stored;
    } catch (error) {
      return statusFromUnknown(error, 'Dispatching NodeFragment raised an exception.');
    }
  }

  /** Resolve, acknowledge, and start one inbound registered action call. */
  async dispatchActionMessage(
    message: import('./data.js').ActionMessage,
    originStream: WireStream | null = null,
  ): Promise<Status> {
    try {
      const validation = message.validate();
      if (!isOk(validation)) return validation;
      if (message.name === CANCEL_ACTION_NAME) {
        let encodedId: Uint8Array | null = null;
        for (const [name, value] of message.headers) {
          if (name.toLowerCase() === CANCEL_ACTION_HEADER) {
            encodedId = value;
            break;
          }
        }
        if (encodedId === null) {
          return invalidArgumentError(
            'Cancel Action requires the __action header.',
          );
        }
        const actionId = utf8Decode(encodedId);
        if (!isOk(actionId)) return actionId;
        const validId = validateName(actionId);
        if (!isOk(validId)) return validId;
        const action = this.getAction(actionId);
        if (isOk(action)) return action.cancel();
        return action.code === notFoundError().code ? okStatus() : action;
      }

      let dispatchStatus: Status = okStatus();
      let action: Action | null = null;
      if (this.phase !== 'open' || this.remoteClosed) {
        dispatchStatus = failedPreconditionError(
          'Session is no longer accepting Actions.',
        );
      } else if (this.activeActions.has(message.id)) {
        dispatchStatus = alreadyExistsError(
          'Action already exists in the Session.',
        );
      } else if (this.actionRegistry === null) {
        dispatchStatus = failedPreconditionError(
          'Session has no ActionRegistry.',
        );
      } else {
        const created = this.actionRegistry.makeAction(message.name, {
          id: message.id,
          nodeMap: this.nodeMap,
          stream: originStream,
          session: this,
        });
        if (!isOk(created)) dispatchStatus = created;
        else action = created;
      }
      if (isOk(dispatchStatus) && action !== null) {
        dispatchStatus = action.mapPortsFromMessage(message);
        for (const [name, value] of message.headers) {
          if (!isOk(dispatchStatus)) break;
          dispatchStatus = action.setHeader(name, value);
        }
        if (isOk(dispatchStatus)) dispatchStatus = action.clearInputsAfterRun();
        if (isOk(dispatchStatus)) dispatchStatus = action.clearOutputsAfterRun();
        if (isOk(dispatchStatus)) {
          dispatchStatus = await action.applyInputAutofills();
        }
        if (isOk(dispatchStatus)) {
          const started = action.run();
          dispatchStatus = isOk(started) ? okStatus() : started;
        }
      }

      if (originStream !== null) {
        const chunk = statusToChunk(dispatchStatus);
        if (!isOk(chunk)) return chunk;
        const dispatchId = Action.makeNodeId(
          message.id,
          ACTION_DISPATCH_STATUS_OUTPUT,
        );
        if (!isOk(dispatchId)) return dispatchId;
        const fragments = [new NodeFragment({
          id: dispatchId,
          data: chunk,
          seq: 0,
          continued: false,
        })];
        if (!isOk(dispatchStatus)) {
          const statusId = Action.makeNodeId(message.id, ACTION_STATUS_OUTPUT);
          if (!isOk(statusId)) return statusId;
          fragments.push(new NodeFragment({
            id: statusId,
            data: chunk,
            seq: 0,
            continued: false,
          }));
        }
        let sent: Status;
        try { sent = originStream.send(new WireMessage({ nodeFragments: fragments })); }
        catch (error) {
          sent = statusFromUnknown(error, 'Sending Action dispatch status raised.');
        }
        return isStatus(sent)
          ? sent
          : internalError('WireStream.send() returned an invalid Status.');
      }
      return dispatchStatus;
    } catch (error) {
      return statusFromUnknown(error, 'Dispatching ActionMessage raised an exception.');
    }
  }

  async dispatchAction(action: Action): Promise<Status> {
    try {
      if (!(action instanceof Action)) {
        return invalidArgumentError('action must be an Action.');
      }
      if (action.getRegistry() === null) {
        const boundRegistry = action.bindRegistry(this.actionRegistry);
        if (!isOk(boundRegistry)) return boundRegistry;
      }
      const bound = action.bindSession(this);
      if (!isOk(bound)) return bound;
      const started = action.run();
      return isOk(started) ? okStatus() : started;
    } catch (error) {
      return statusFromUnknown(error, 'Dispatching Action raised an exception.');
    }
  }

  /** Dispatch every action and fragment in one validated inbound message. */
  async dispatchWireMessage(
    message: WireMessage,
    originStream: WireStream | null = null,
  ): Promise<Status> {
    try {
      if (!(message instanceof WireMessage)) {
        return invalidArgumentError('message must be a WireMessage.');
      }
      const validation = message.validate();
      if (!isOk(validation)) return validation;
      const failures: DispatchFailure[] = [];
      for (let index = 0; index < message.actions.length; ++index) {
        const status = await this.dispatchActionMessage(
          message.actions[index]!,
          originStream,
        );
        if (!isOk(status)) {
          failures.push({
            elementType: 'action_message',
            elementIndex: index,
            status,
          });
        }
      }
      for (let index = 0; index < message.nodeFragments.length; ++index) {
        const fragment = message.nodeFragments[index]!;
        const status = await this.dispatchNodeFragment(fragment);
        if (!isOk(status)) {
          const separator = fragment.id.indexOf('#');
          if (separator >= 0) {
            this.cancelAction(fragment.id.slice(0, separator));
          }
          failures.push({
            elementType: 'node_fragment',
            elementIndex: index,
            status,
          });
        }
      }
      return failures.length === 0
        ? okStatus()
        : aggregateDispatchFailures(
            failures,
            message.actions.length + message.nodeFragments.length,
          );
    } catch (error) {
      return statusFromUnknown(error, 'Dispatching WireMessage raised an exception.');
    }
  }

  /**
   * Attach and drive one transport endpoint.
   *
   * The returned status covers the stream startup handshake. The session keeps
   * pumping it afterwards; await {@link done} for connection-wide completion.
   */
  async addStream(
    stream: WireStream,
    mode: StreamMode = StreamMode.START,
  ): Promise<Status> {
    let state: SessionStreamState | null = null;
    try {
      if (!hasWireStreamShape(stream)) {
        return invalidArgumentError('stream must implement WireStream.');
      }
      if (mode !== StreamMode.START && mode !== StreamMode.ACCEPT) {
        return invalidArgumentError('mode must be StreamMode.START or ACCEPT.');
      }
      if (this.deadlineExpired()) {
        this.abort(deadlineExceededError('The Session deadline has been exceeded.'));
      }
      let streamId: string;
      try { streamId = stream.getId(); }
      catch (error) {
        return statusFromUnknown(error, 'WireStream.getId() raised an exception.');
      }
      const validId = validateName(streamId);
      if (!isOk(validId)) return validId;
      if (this.phase !== 'open' || this.remoteClosed) {
        return failedPreconditionError(
          'No streams can be attached after the Session ends.',
        );
      }
      if (this.streamsById.has(streamId) || this.streamStates.has(stream)) {
        return alreadyExistsError('Stream is already attached to the Session.');
      }
      state = {
        stream,
        id: streamId,
        outstandingMessages: 0,
        outstandingBytes: 0,
        pendingMessages: [],
        messagePumpRunning: false,
        acceptingMessages: true,
        remoteHalfClosed: false,
        halfCloseDelivered: false,
        doneStarted: false,
        done: false,
      };
      this.streamsById.set(streamId, state);
      this.streamStates.set(stream, state);
      this.streamOrder.push(stream);
      this.clearNoStreamTimer();
      this.notifyStateChanged();

      const onMessage = async (message: WireMessage | null): Promise<Status> =>
        this.handleStreamMessage(state!, message);
      const onDone = async (): Promise<Status> => this.handleStreamDone(state!);
      let startup: Status;
      try {
        startup = mode === StreamMode.START
          ? await stream.start(onMessage, onDone)
          : await stream.accept(onMessage, onDone);
      } catch (error) {
        startup = statusFromUnknown(error, 'WireStream startup raised an exception.');
      }
      if (!isStatus(startup)) {
        const invalid = internalError('WireStream startup returned an invalid Status.');
        this.removeStream(state);
        return invalid;
      }
      if (!isOk(startup)) this.removeStream(state);
      return startup;
    } catch (error) {
      if (state !== null) this.removeStream(state);
      return statusFromUnknown(error, 'Attaching Session stream raised an exception.');
    }
  }

  /** Queue a message on a named stream or round-robin across active streams. */
  send(message: WireMessage, streamId = ''): Status {
    try {
      if (!(message instanceof WireMessage)) {
        return invalidArgumentError('message must be a WireMessage.');
      }
      const validation = message.validate();
      if (!isOk(validation)) return validation;
      if (this.phase !== 'open') {
        return failedPreconditionError(
          'Messages cannot be sent after the Session ends.',
        );
      }
      let stream: WireStream;
      if (streamId !== '') {
        const found = this.streamsById.get(streamId);
        if (found === undefined) {
          return notFoundError('Session stream was not found.');
        }
        stream = found.stream;
      } else {
        const available = this.streamOrder
          .map((candidate) => this.streamStates.get(candidate))
          .filter(
            (candidate): candidate is SessionStreamState =>
              candidate !== undefined &&
              !candidate.done &&
              !candidate.doneStarted,
          );
        if (available.length === 0) {
          return notFoundError('Session has no attached streams.');
        }
        const index = this.roundRobinIndex % available.length;
        this.roundRobinIndex = (index + 1) % available.length;
        stream = available[index]!.stream;
      }
      try {
        const sent = stream.send(message);
        return isStatus(sent)
          ? sent
          : internalError('WireStream.send() returned an invalid Status.');
      }
      catch (error) {
        return statusFromUnknown(error, 'WireStream.send() raised an exception.');
      }
    } catch (error) {
      return statusFromUnknown(error, 'Sending Session message raised an exception.');
    }
  }

  /**
   * Begin clean shutdown and half-close every active stream with OK trailers.
   * Existing inbound work may still arrive and attached streams must still
   * finish before {@link done} resolves.
   */
  halfClose(): Status {
    try {
      if (this.phase !== 'open') return okStatus();
      if (this.deadlineExpired()) {
        return this.abort(
          deadlineExceededError('The Session deadline has been exceeded.'),
        );
      }
      const packed = packStatus(okStatus());
      if (!isOk(packed)) return packed;
      const trailers = copyByteMap(this.headers);
      trailers.set(SESSION_STATUS_HEADER, packed);
      this.phase = 'closing';
      this.sessionStatus = okStatus();
      this.clearTimers();
      this.notifyStateChanged();
      let first: Status = okStatus();
      for (const state of this.streamStates.values()) {
        if (state.done || state.doneStarted) continue;
        try {
          const closed = state.stream.halfClose(trailers);
          first = firstError(first, closed);
        } catch (error) {
          first = firstError(
            first,
            statusFromUnknown(error, 'WireStream.halfClose() raised.'),
          );
        }
      }
      this.finishIfPossible();
      return first;
    } catch (error) {
      return statusFromUnknown(error, 'Half-closing Session raised an exception.');
    }
  }

  /** Cancel actions and end the session with a structured non-OK status. */
  abort(status: Status): Status {
    try {
      if (!isStatus(status) || isOk(status)) {
        return invalidArgumentError('An aborted Session needs a non-OK status.');
      }
      if (this.phase !== 'open') return okStatus();
      if (this.deadlineExpired()) {
        status = deadlineExceededError('The Session deadline has been exceeded.');
      }
      const sessionStatus = packStatus(status);
      if (!isOk(sessionStatus)) return sessionStatus;
      const streamAbort = sessionStreamAbortStatus();
      const packedStreamAbort = packStatus(streamAbort);
      if (!isOk(packedStreamAbort)) return packedStreamAbort;
      const headers = copyByteMap(this.headers);
      headers.set(SESSION_STATUS_HEADER, sessionStatus);
      headers.set(ABORT_STATUS_HEADER, packedStreamAbort);
      const terminal = new WireMessage({ headers });
      this.phase = 'aborted';
      this.sessionStatus = status;
      this.clearTimers();
      const cancellation = cancelledError('Session was aborted.');
      this.rootLimiter.cancel(cancellation);
      this.nestedLimiter.cancel(cancellation);
      for (const state of this.streamStates.values()) {
        state.acceptingMessages = false;
        this.clearPendingMessages(state);
      }
      this.notifyStateChanged();
      // Cancellation failures are local Action cleanup diagnostics; the
      // Session abort itself still proceeds and reports transport failures.
      this.cancelAllActions();
      let first: Status = okStatus();
      for (const state of this.streamStates.values()) {
        if (state.done || state.doneStarted) continue;
        let sent: Status;
        try { sent = state.stream.send(terminal); }
        catch (error) {
          sent = statusFromUnknown(error, 'Sending Session abort raised.');
        }
        if (!isStatus(sent)) {
          sent = internalError('WireStream.send() returned an invalid Status.');
        }
        if (!isOk(sent)) {
          first = firstError(first, sent);
          try {
            const aborted = state.stream.abort(streamAbort);
            first = firstError(first, aborted);
          }
          catch (error) {
            first = firstError(
              first,
              statusFromUnknown(error, 'WireStream.abort() raised.'),
            );
          }
        }
      }
      this.finishIfPossible();
      return first;
    } catch (error) {
      return statusFromUnknown(error, 'Aborting Session raised an exception.');
    }
  }

  /** Whether either endpoint has ended the session for new work. */
  isClosed(): boolean {
    return this.remoteClosed || this.phase !== 'open';
  }

  /** Whether every attached stream has completed and state is fully quiescent. */
  isDone(): boolean { return this.destroyed; }

  /** Await full cleanup and receive the session's terminal status. */
  done(): Promise<Status> { return this.doneDeferred.promise; }

  getStatus(): Status {
    if (this.phase === 'open' && this.deadlineExpired()) {
      this.abort(deadlineExceededError('The Session deadline has been exceeded.'));
    }
    return this.sessionStatus;
  }

  getDeadline(): number | null { return this.options.deadline; }

  setDeadline(deadline: WireDeadline = null): Status {
    const normalized = wireDeadlineMillis(deadline);
    if (!isOk(normalized)) return normalized;
    this.options.deadline = normalized;
    this.scheduleDeadlineTimer();
    this.notifyStateChanged();
    return this.phase === 'open' && this.deadlineExpired()
      ? this.abort(
          deadlineExceededError('The Session deadline has been exceeded.'),
        )
      : okStatus();
  }

  protected streamIdOf(stream: WireStream): string {
    return this.streamStates.get(stream)?.id ?? '';
  }

  private async handleStreamMessage(
    state: SessionStreamState,
    message: WireMessage | null,
  ): Promise<Status> {
    try {
      if (message === null) return this.handleRemoteHalfClose(state);
      if (!(message instanceof WireMessage)) {
        return invalidArgumentError('WireStream delivered a non-WireMessage value.');
      }
      if (state.remoteHalfClosed) {
        return failedPreconditionError(
          'WireStream delivered data after its remote half-close.',
        );
      }
      const encoded = message.toMsgpack();
      if (!isOk(encoded)) return encoded;
      const size = encoded.byteLength;
      if (size > this.options.maxSingleMessageSize) {
        return outOfRangeError(
          'Incoming WireMessage exceeds maxSingleMessageSize.',
        );
      }
      while (true) {
        if (!state.acceptingMessages || this.phase === 'aborted') {
          return okStatus();
        }
        const countsFit =
          this.bufferedMessages < this.options.maxBufferedMessagesTotal &&
          state.outstandingMessages <
            this.options.maxBufferedMessagesPerStream;
        const totalBytesFit =
          this.bufferedBytes === 0 ||
          this.bufferedBytes + size <= this.options.maxBufferedBytesTotal;
        const streamBytesFit =
          state.outstandingBytes === 0 ||
          state.outstandingBytes + size <=
            this.options.maxBufferedBytesPerStream;
        if (countsFit && totalBytesFit && streamBytesFit) break;
        await this.waitForStateChange();
      }
      ++this.bufferedMessages;
      this.bufferedBytes += size;
      ++state.outstandingMessages;
      state.outstandingBytes += size;
      state.pendingMessages.push({ message, size });
      if (!state.messagePumpRunning) {
        state.messagePumpRunning = true;
        queueMicrotask(() => void this.processStreamMessages(state));
      }
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Handling Session stream message raised.');
    }
  }

  private async handleRemoteHalfClose(
    state: SessionStreamState,
  ): Promise<Status> {
    state.remoteHalfClosed = true;
    while (
      state.acceptingMessages &&
      this.phase !== 'aborted' &&
      state.outstandingMessages > 0
    ) {
      await this.waitForStateChange();
    }
    if (
      !state.acceptingMessages ||
      this.phase === 'aborted' ||
      state.halfCloseDelivered
    ) {
      return okStatus();
    }
    const trailers = this.safeGetTrailers(state.stream);
    if (trailers !== null) {
      const encoded = trailers.get(SESSION_STATUS_HEADER);
      if (encoded !== undefined) {
        const decoded = decodeStatus(encoded);
        if (!isOk(decoded)) return decoded;
        if (!isOk(decoded.status)) {
          return failedPreconditionError(
            'A peer must abort, not half-close, a failed Session.',
          );
        }
        this.remoteClosed = true;
        this.notifyStateChanged();
      }
    }
    state.halfCloseDelivered = true;
    return invokeSessionMessageCallback(
      this.onStreamMessage,
      null,
      state.stream,
      this,
    );
  }

  private async processStreamMessages(state: SessionStreamState): Promise<void> {
    try {
      while (state.pendingMessages.length > 0) {
        const buffered = state.pendingMessages.shift()!;
        let callbackStatus: Status;
        if (this.phase === 'aborted' || !state.acceptingMessages) {
          callbackStatus = okStatus();
        } else {
          callbackStatus = await invokeSessionMessageCallback(
            this.onStreamMessage,
            buffered.message,
            state.stream,
            this,
          );
        }
        this.releaseBufferedMessage(state, buffered.size);
        if (!isOk(callbackStatus)) {
          state.acceptingMessages = false;
          this.clearPendingMessages(state);
          state.messagePumpRunning = false;
          this.notifyStateChanged();
          if (this.phase !== 'aborted') {
            try { state.stream.abort(callbackStatus); }
            catch { /* The callback status remains the transport failure. */ }
          }
          return;
        }
      }
      state.messagePumpRunning = false;
      this.notifyStateChanged();
    } catch (error) {
      state.acceptingMessages = false;
      this.clearPendingMessages(state);
      state.messagePumpRunning = false;
      const status = statusFromUnknown(error, 'Session message pump raised.');
      try { state.stream.abort(status); }
      catch { /* No exception may escape the scheduled pump. */ }
      this.notifyStateChanged();
    }
  }

  private async handleStreamDone(state: SessionStreamState): Promise<Status> {
    try {
      if (state.done || state.doneStarted) return okStatus();
      state.doneStarted = true;
      const streamStatus = this.safeGetStatus(state.stream);
      if (isSessionStreamAbortStatus(streamStatus) && this.phase !== 'aborted') {
        this.remoteClosed = true;
        this.phase = 'aborted';
        this.sessionStatus = this.sessionStatusFromTrailers(state.stream) ??
          streamStatus;
        this.clearTimers();
        for (const candidate of this.streamStates.values()) {
          candidate.acceptingMessages = false;
          this.clearPendingMessages(candidate);
        }
        const cancellation = cancelledError('Remote Session was aborted.');
        this.rootLimiter.cancel(cancellation);
        this.nestedLimiter.cancel(cancellation);
        this.cancelAllActions();
      } else if (!isOk(streamStatus)) {
        state.acceptingMessages = false;
      }
      this.notifyStateChanged();
      while (state.outstandingMessages > 0) await this.waitForStateChange();
      const callbackStatus = await invokeSessionDoneCallback(
        this.onStreamDone,
        state.stream,
        this,
      );
      this.removeStream(state);
      return callbackStatus;
    } catch (error) {
      this.removeStream(state);
      return statusFromUnknown(error, 'Handling Session stream completion raised.');
    }
  }

  private removeStream(state: SessionStreamState): void {
    if (state.done) return;
    state.done = true;
    state.acceptingMessages = false;
    this.clearPendingMessages(state);
    this.streamStates.delete(state.stream);
    if (this.streamsById.get(state.id) === state) this.streamsById.delete(state.id);
    const index = this.streamOrder.indexOf(state.stream);
    if (index >= 0) this.streamOrder.splice(index, 1);
    if (
      this.streamStates.size === 0 &&
      this.phase === 'open' &&
      !this.remoteClosed
    ) {
      this.scheduleNoStreamTimer();
    }
    this.notifyStateChanged();
    this.finishIfPossible();
  }

  private releaseBufferedMessage(state: SessionStreamState, size: number): void {
    this.bufferedMessages = Math.max(0, this.bufferedMessages - 1);
    this.bufferedBytes = Math.max(0, this.bufferedBytes - size);
    state.outstandingMessages = Math.max(0, state.outstandingMessages - 1);
    state.outstandingBytes = Math.max(0, state.outstandingBytes - size);
    this.notifyStateChanged();
  }

  private clearPendingMessages(state: SessionStreamState): void {
    for (const buffered of state.pendingMessages.splice(0)) {
      this.bufferedMessages = Math.max(0, this.bufferedMessages - 1);
      this.bufferedBytes = Math.max(0, this.bufferedBytes - buffered.size);
      state.outstandingMessages = Math.max(0, state.outstandingMessages - 1);
      state.outstandingBytes = Math.max(0, state.outstandingBytes - buffered.size);
    }
    this.notifyStateChanged();
  }

  private finishIfPossible(): void {
    if (
      (this.phase === 'open' && !this.remoteClosed) ||
      this.streamStates.size !== 0 ||
      this.destroyed
    ) {
      return;
    }
    this.destroyed = true;
    this.clearTimers();
    this.doneDeferred.resolve(this.sessionStatus);
    this.notifyStateChanged();
  }

  private notifyStateChanged(): void {
    const waiters = this.stateWaiters;
    this.stateWaiters = [];
    for (const waiter of waiters) waiter.resolve(undefined);
  }

  private waitForStateChange(): Promise<void> {
    const waiter = new Deferred<void>();
    this.stateWaiters.push(waiter);
    return waiter.promise;
  }

  private safeGetStatus(stream: WireStream): Status {
    try {
      const status = stream.getStatus();
      return isStatus(status)
        ? status
        : internalError('WireStream.getStatus() returned an invalid Status.');
    } catch (error) {
      return statusFromUnknown(error, 'WireStream.getStatus() raised.');
    }
  }

  private safeGetTrailers(stream: WireStream): ByteMap | null {
    try {
      const trailers = stream.getTrailers();
      return trailers === null ? null : copyByteMap(trailers);
    } catch {
      return null;
    }
  }

  private sessionStatusFromTrailers(stream: WireStream): Status | null {
    const encoded = this.safeGetTrailers(stream)?.get(SESSION_STATUS_HEADER);
    if (encoded === undefined) return null;
    const decoded = decodeStatus(encoded);
    return isOk(decoded) ? decoded.status : null;
  }

  private deadlineExpired(): boolean {
    return this.options.deadline !== null && this.options.deadline <= Date.now();
  }

  private scheduleDeadlineTimer(): void {
    if (this.deadlineTimer !== null) clearTimeout(this.deadlineTimer);
    this.deadlineTimer = null;
    if (this.options.deadline === null || this.phase !== 'open') return;
    const delay = Math.max(0, this.options.deadline - Date.now());
    this.deadlineTimer = setTimeout(() => {
      this.deadlineTimer = null;
      if (this.phase === 'open') {
        this.abort(
          deadlineExceededError('The Session deadline has been exceeded.'),
        );
      }
    }, delay);
    this.unrefTimer(this.deadlineTimer);
  }

  private scheduleNoStreamTimer(): void {
    this.clearNoStreamTimer();
    if (
      this.options.noStreamTimeoutMs === null ||
      this.phase !== 'open' ||
      this.remoteClosed ||
      this.streamStates.size !== 0
    ) {
      return;
    }
    this.noStreamTimer = setTimeout(() => {
      this.noStreamTimer = null;
      if (
        this.phase === 'open' &&
        !this.remoteClosed &&
        this.streamStates.size === 0
      ) {
        this.halfClose();
      }
    }, this.options.noStreamTimeoutMs);
    this.unrefTimer(this.noStreamTimer);
  }

  private clearNoStreamTimer(): void {
    if (this.noStreamTimer !== null) clearTimeout(this.noStreamTimer);
    this.noStreamTimer = null;
  }

  private clearTimers(): void {
    this.clearNoStreamTimer();
    if (this.deadlineTimer !== null) clearTimeout(this.deadlineTimer);
    this.deadlineTimer = null;
  }

  private unrefTimer(timer: ReturnType<typeof setTimeout>): void {
    try { (timer as ReturnType<typeof setTimeout> & { unref?: () => void }).unref?.(); }
    catch { /* Browser timers do not need unref. */ }
  }
}

/** Pull-mode message paired with the transport that delivered it. */
export interface ReceivedSessionMessage {
  message: WireMessage;
  streamId: string;
}

/**
 * Pull-oriented Session with a one-slot inbound backpressure handoff.
 *
 * Use this when an agent owns an explicit `while (await receive())` loop. It
 * replaces stream callbacks with {@link receiveWithStreamId}; action and node
 * auto-dispatch are therefore the caller's choice. A clean session half-close
 * produces `null` once, while an abort remains a structured error.
 */
export class SessionWithRecv extends Session {
  private readonly receiveQueue: Array<ReceivedSessionMessage | null> = [];
  private receiveError: NonOkStatus | null = null;
  private readonly endedStreams = new Set<WireStream>();
  private sessionHalfClosed = false;
  private eofStarted = false;
  private eofDelivered = false;
  private receiveWaiters: Array<Deferred<void>> = [];

  private constructor(initialized: InitializedSession) {
    super({
      ...initialized,
      onStreamMessage: undefined,
      onStreamDone: undefined,
    });
    this.setStreamCallbacks(
      (message, stream) => this.receiveOnMessage(message, stream),
      (stream) => this.receiveOnDone(stream),
    );
  }

  static override create(
    options: Omit<SessionCreateOptions, 'onStreamMessage' | 'onStreamDone'> = {},
  ): StatusOr<SessionWithRecv> {
    try {
      const initialized = initializeSession(options);
      return isOk(initialized)
        ? new SessionWithRecv(initialized)
        : initialized;
    } catch (error) {
      return statusFromUnknown(
        error,
        'Constructing receiving Session raised an exception.',
      );
    }
  }

  /** Await one inbound message and its stream id, or `null` at clean EOF. */
  async receiveWithStreamId(
    timeoutMs?: number,
  ): Promise<StatusOr<ReceivedSessionMessage | null>> {
    try {
      if (
        timeoutMs !== undefined &&
        (!Number.isFinite(timeoutMs) || timeoutMs < 0)
      ) {
        return invalidArgumentError(
          'timeoutMs must be a non-negative finite number.',
        );
      }
      const deadline = timeoutMs === undefined ? null : Date.now() + timeoutMs;
      while (true) {
        if (this.receiveError !== null) return this.receiveError;
        if (this.eofDelivered) {
          return failedPreconditionError(
            'Remote Session half-close was already received.',
          );
        }
        if (this.receiveQueue.length > 0) {
          const result = this.receiveQueue.shift()!;
          if (result === null) this.eofDelivered = true;
          this.notifyReceiveChanged();
          return result;
        }
        const remaining = deadline === null
          ? undefined
          : Math.max(0, deadline - Date.now());
        const changed = await this.waitForReceiveChange(remaining);
        if (!changed) {
          return deadlineExceededError(
            'The Session receive deadline has been exceeded.',
          );
        }
      }
    } catch (error) {
      return statusFromUnknown(error, 'Receiving Session message raised an exception.');
    }
  }

  /** Await one inbound message without exposing its stream id. */
  async receive(timeoutMs?: number): Promise<StatusOr<WireMessage | null>> {
    const received = await this.receiveWithStreamId(timeoutMs);
    return isOk(received) && received !== null ? received.message : received;
  }

  override abort(status: Status): Status {
    const result = super.abort(status);
    const recorded = this.getStatus();
    if (!isOk(recorded)) this.signalReceiveError(recorded);
    return result;
  }

  private async receiveOnMessage(
    message: WireMessage | null,
    stream: WireStream,
  ): Promise<Status> {
    try {
      if (this.receiveError !== null) return okStatus();
      if (message === null) {
        this.endedStreams.add(stream);
        const trailers = (() => {
          try { return stream.getTrailers(); }
          catch { return null; }
        })();
        if (trailers?.has(SESSION_STATUS_HEADER)) this.sessionHalfClosed = true;
        if (!this.sessionHalfClosed || this.eofStarted) return okStatus();
        const attached = this.streams();
        if (!isOk(attached)) return attached;
        if (attached.some(([, candidate]) => !this.endedStreams.has(candidate))) {
          return okStatus();
        }
      }
      while (this.receiveQueue.length > 0 && this.receiveError === null) {
        await this.waitForReceiveChange();
      }
      if (this.receiveError !== null) return okStatus();
      if (message === null) {
        this.receiveQueue.push(null);
        this.eofStarted = true;
      } else {
        this.receiveQueue.push({
          message,
          streamId: this.streamIdOf(stream),
        });
      }
      this.notifyReceiveChanged();
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Buffering received Session message raised.');
    }
  }

  private async receiveOnDone(stream: WireStream): Promise<Status> {
    const status = this.getStatus();
    if (!isOk(status)) {
      this.signalReceiveError(status);
      return okStatus();
    }
    this.endedStreams.add(stream);
    return this.receiveOnMessage(null, stream);
  }

  private signalReceiveError(status: NonOkStatus): void {
    if (this.receiveError !== null) return;
    this.receiveError = status;
    this.receiveQueue.splice(0);
    this.notifyReceiveChanged();
  }

  private notifyReceiveChanged(): void {
    const waiters = this.receiveWaiters;
    this.receiveWaiters = [];
    for (const waiter of waiters) waiter.resolve(undefined);
  }

  private async waitForReceiveChange(timeoutMs?: number): Promise<boolean> {
    const waiter = new Deferred<void>();
    this.receiveWaiters.push(waiter);
    if (timeoutMs === undefined) {
      await waiter.promise;
      return true;
    }
    if (timeoutMs <= 0) {
      const index = this.receiveWaiters.indexOf(waiter);
      if (index >= 0) this.receiveWaiters.splice(index, 1);
      return false;
    }
    let timer: ReturnType<typeof setTimeout> | null = null;
    const timedOut = new Deferred<boolean>();
    timer = setTimeout(() => timedOut.resolve(false), timeoutMs);
    try {
      const changed = await Promise.race([
        waiter.promise.then(() => true),
        timedOut.promise,
      ]);
      if (!changed) {
        const index = this.receiveWaiters.indexOf(waiter);
        if (index >= 0) this.receiveWaiters.splice(index, 1);
      }
      return changed;
    } finally {
      if (timer !== null) clearTimeout(timer);
    }
  }
}
