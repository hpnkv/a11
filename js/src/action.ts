import {
  copyByteMap,
  normalizeByteMap,
  randomId,
  type ByteMap,
  type ByteMapInput,
  type ByteSource,
} from './bytes.js';
import { Deferred } from './concurrency.js';

/// One encoder for the log attributes, which are written on every log line.
const encoder = new TextEncoder();
import {
  LOG_CHANNEL_ATTRIBUTE,
  LOG_FILE_ATTRIBUTE,
  LOG_INTERNAL_ATTRIBUTE,
  LOG_INTERNAL_FALSE,
  LOG_INTERNAL_TRUE,
  LOG_LEVEL_ATTRIBUTE,
  LOG_LEVELS,
  LOG_LINENO_ATTRIBUTE,
  logRecordFromChunk,
  parseLogLevel,
  reportLog,
  type LogOptions,
} from './action_log.js';
import { AsyncNode, NodeMap } from './async_node.js';
import {
  ActionMessage,
  Chunk,
  ChunkMetadata,
  NodeFragment,
  Port,
  WireMessage,
  makeNullChunk,
  validateName,
} from './data.js';
import { OCTET_STREAM_MIMETYPE, toChunk } from './serialization.js';
import {
  ACTION_DISPATCH_STATUS_OUTPUT,
  ACTION_LOG_OUTPUT,
  ACTION_HEADER_PREFIX,
  ACTION_STATUS_OUTPUT,
  CANCEL_ACTION_HEADER,
  CANCEL_ACTION_NAME,
  ActionSchema,
  type ActionSettings,
  isStatusChunk,
  statusToChunk,
} from './action_schema.js';
import {
  cancelledError,
  deadlineExceededError,
  failedPreconditionError,
  internalError,
  invalidArgumentError,
  isOk,
  isStatus,
  notFoundError,
  okStatus,
  statusFromUnknown,
  type NonOkStatus,
  type Status,
  type StatusOr,
} from './status.js';
import {
  normalizeWireHeaders,
  type WireStream,
} from './wire_stream.js';

/** Asynchronous application work invoked by {@link Action.run}. */
export type ActionHandler = (
  action: Action,
) => void | Status | Promise<void | Status>;

/** Synchronous hook run when cancellation is first requested. */
export type OnActionCancelled = (action: Action) => void | Status;

/** Minimal registry contract used to resolve a nested action by name. */
export interface ActionRegistryLike {
  /** Return the registered callable interface. */
  getSchema(actionName: string): StatusOr<ActionSchema>;
  /** Return its local handler, if this process can execute it. */
  getHandler(actionName: string): StatusOr<ActionHandler>;
}

/** Session operations an Action needs for dispatch and lifetime tracking. */
export interface ActionSessionContext {
  /** Shared node namespace into which action ports are mapped. */
  getNodeMap(): NodeMap;
  /** Registry used for inbound and nested work. */
  getActionRegistry(): ActionRegistryLike | null;
  /** Route a call/cancellation message over an attached session stream. */
  send(message: WireMessage, streamId?: string): Status;
  /** Add one running action to session shutdown/concurrency accounting. */
  trackAction(action: Action): Status;
  /** Remove a terminal action from session accounting. */
  untrackAction(action: Action): void;
  /** Await a root/nested concurrency slot before invoking a handler. */
  acquireActionSlot?(nested: boolean, signal?: AbortSignal): Promise<Status>;
  /** Release the concurrency slot after the handler unwinds. */
  releaseActionSlot?(nested: boolean): void;
}

/** Collaborators and instance policy supplied when creating an Action. */
export interface ActionCreateOptions {
  /** Stable call id; generated when omitted. */
  id?: string;
  /** Local implementation; may be bound later or absent for remote-only calls. */
  handler?: ActionHandler | null;
  /** Node namespace backing input/output port ids. */
  nodeMap?: NodeMap;
  /** Direct transport used by `call`, when no session routes it. */
  stream?: WireStream | null;
  /** Optional connection runtime that tracks and routes this action. */
  session?: ActionSessionContext | null;
  /** Registry used to resolve nested action names. */
  registry?: ActionRegistryLike | null;
  /** Port binding and post-run retention policy. */
  settings?: ActionSettings;
}

type ActionMode = 'none' | 'run' | 'call' | 'cancelled';

function firstError(first: Status, next: unknown): Status {
  if (!isStatus(next)) {
    return isOk(first)
      ? internalError('A Status-returning Action operation returned an invalid value.')
      : first;
  }
  return isOk(first) && !isOk(next) ? next : first;
}

function hasRegistryShape(value: unknown): value is ActionRegistryLike {
  if (typeof value !== 'object' || value === null) return false;
  try {
    const candidate = value as Record<string, unknown>;
    return typeof candidate.getSchema === 'function' &&
      typeof candidate.getHandler === 'function';
  } catch {
    return false;
  }
}

function hasSessionShape(value: unknown): value is ActionSessionContext {
  if (typeof value !== 'object' || value === null) return false;
  try {
    const candidate = value as Record<string, unknown>;
    return typeof candidate.getNodeMap === 'function' &&
      typeof candidate.getActionRegistry === 'function' &&
      typeof candidate.send === 'function' &&
      typeof candidate.trackAction === 'function' &&
      typeof candidate.untrackAction === 'function' &&
      (candidate.acquireActionSlot === undefined ||
        typeof candidate.acquireActionSlot === 'function') &&
      (candidate.releaseActionSlot === undefined ||
        typeof candidate.releaseActionSlot === 'function');
  } catch {
    return false;
  }
}

function validateActionSettings(settings: unknown): Status {
  try {
    if (typeof settings !== 'object' || settings === null || Array.isArray(settings)) {
      return invalidArgumentError('settings must be an object.');
    }
    const candidate = settings as Record<string, unknown>;
    for (const name of [
      'bindStreamsOnInputsByDefault',
      'bindStreamsOnOutputsByDefault',
      'clearInputsAfterRun',
      'clearOutputsAfterRun',
    ]) {
      if (candidate[name] !== undefined && typeof candidate[name] !== 'boolean') {
        return invalidArgumentError(`Action setting ${name} must be boolean.`);
      }
    }
    return okStatus();
  } catch (error) {
    return invalidArgumentError('Action settings could not be read.', [], error);
  }
}

/**
 * One schema-described unit of local work or remote agent work.
 *
 * An action binds an {@link ActionSchema} to input/output {@link AsyncNode}s
 * and, for a local run, an application handler. It is a one-shot state machine:
 * configure the id, schema, collaborators, headers, and port mappings; then
 * choose {@link run} for local execution or {@link call} for remote dispatch.
 * Configuration that changes identity or port shape is frozen after start.
 *
 * Remote calls have two milestones. {@link waitForDispatch} reports whether
 * the peer accepted the action, while {@link wait} follows its eventual status
 * after handler and output-writer cleanup. Local handlers can create children
 * with {@link makeNested}; a shared session applies nested limits and includes
 * tracked children in session-wide abort, but each child has an independent
 * cancellation signal.
 */
export class Action {
  private schema: ActionSchema;
  private id: string;
  private handler: ActionHandler | null;
  private nodeMap: NodeMap;
  private stream: WireStream | null;
  private session: ActionSessionContext | null;
  private registry: ActionRegistryLike | null;
  private settings: ActionSettings;
  private readonly headers: ByteMap = new Map();
  private readonly inputIds = new Map<string, string>();
  private readonly outputIds = new Map<string, string>();
  private readonly inputNodes = new Set<AsyncNode>();
  private readonly outputNodes = new Set<AsyncNode>();
  private readonly boundNodes = new Set<AsyncNode>();
  private mode: ActionMode = 'none';
  private completionStatus: Status | null = null;
  private dispatchStatus: Status | null = null;
  private cancelRequested = false;
  private finishing = false;
  private inputAutofillsApplied = false;
  private tracked = false;
  // Set once a consumer has taken the log port through getLogNode(). It then owns
  // presentation, so log() stops reporting to the process sink.
  private logClaimed = false;
  private readonly done = new Deferred<Status>();
  private readonly dispatched = new Deferred<Status>();
  private readonly cancelController = new AbortController();
  private readonly cancelCallbacks: OnActionCancelled[] = [];
  private parent: Action | null = null;

  private constructor(schema: ActionSchema, options: ActionCreateOptions, id: string) {
    this.schema = schema;
    this.id = id;
    this.handler = options.handler ?? null;
    this.session = options.session ?? null;
    this.nodeMap = options.nodeMap ?? this.session?.getNodeMap() ?? new NodeMap();
    this.stream = options.stream ?? null;
    this.registry = options.registry ?? this.session?.getActionRegistry() ?? null;
    this.settings = { ...(options.settings ?? {}) };
    for (const [name, header] of schema.headers) {
      if (header.defaultValue !== null) {
        this.headers.set(name.toLowerCase(), new Uint8Array(header.defaultValue));
      }
    }
    this.remapDefaultPorts();
  }

  /** Validate a schema/options bundle and create a configurable action. */
  static create(
    schema: ActionSchema,
    options: ActionCreateOptions = {},
  ): StatusOr<Action> {
    try {
      if (!(schema instanceof ActionSchema)) {
        return invalidArgumentError('schema must be an ActionSchema.');
      }
      if (typeof options !== 'object' || options === null) {
        return invalidArgumentError('Action options must be an object.');
      }
      const validation = schema.validate();
      if (!isOk(validation)) return validation;
      if (options.handler !== undefined && options.handler !== null && typeof options.handler !== 'function') {
        return invalidArgumentError('handler must be callable or null.');
      }
      if (options.nodeMap !== undefined && !(options.nodeMap instanceof NodeMap)) {
        return invalidArgumentError('nodeMap must be a NodeMap.');
      }
      const settings = validateActionSettings(options.settings ?? {});
      if (!isOk(settings)) return settings;
      const id = options.id || randomId('action-');
      const validId = validateName(id);
      if (!isOk(validId)) return validId;
      const action = new Action(schema, options, id);
      const remapped = action.remapDefaultPorts();
      return isOk(remapped) ? action : remapped;
    } catch (error) {
      return statusFromUnknown(error, 'Creating Action raised an exception.');
    }
  }

  /** Derive the stable `action-id#port-name` id for one action port node. */
  static makeNodeId(actionId: string, nodeName: string): StatusOr<string> {
    const validAction = validateName(actionId);
    if (!isOk(validAction)) return validAction;
    const validNode = validateName(nodeName);
    if (!isOk(validNode)) return validNode;
    const result = `${actionId}#${nodeName}`;
    const validResult = validateName(result);
    return isOk(validResult) ? result : validResult;
  }

  /** AbortSignal a handler can observe for cooperative cancellation. */
  get signal(): AbortSignal { return this.cancelController.signal; }
  getId(): string { return this.id; }
  getSchema(): ActionSchema { return this.schema; }
  getHandler(): ActionHandler | null { return this.handler; }
  hasHandler(): boolean { return this.handler !== null; }
  getSettings(): ActionSettings { return { ...this.settings }; }
  getNodeMap(): NodeMap { return this.nodeMap; }
  getStream(): WireStream | null { return this.stream; }
  getRegistry(): ActionRegistryLike | null { return this.registry; }
  getSession(): ActionSessionContext | null { return this.session; }
  /** Completion status; OK is provisional until `isDone()` becomes true. */
  getStatus(): Status { return this.completionStatus ?? okStatus(); }
  /** Remote acknowledgement status, or `null` before it arrives. */
  getDispatchStatus(): Status | null { return this.dispatchStatus; }
  isDone(): boolean { return this.completionStatus !== null; }
  hasBeenRun(): boolean { return this.mode === 'run'; }
  hasBeenCalled(): boolean { return this.mode === 'call'; }
  isCancelled(): boolean {
    return this.cancelRequested || this.completionStatus?.code === cancelledError().code;
  }

  /** Replace the call id and remap default port ids before starting. */
  setId(id: string): Status {
    const validation = validateName(id);
    if (!isOk(validation)) return validation;
    if (this.mode !== 'none') {
      return failedPreconditionError('Cannot change Action id after it has started.');
    }
    const previous = this.id;
    this.id = id;
    const remapped = this.remapDefaultPorts();
    if (!isOk(remapped)) {
      this.id = previous;
      this.remapDefaultPorts();
    }
    return remapped;
  }

  /** Replace the callable contract and remap ports before starting. */
  setSchema(schema: ActionSchema): Status {
    if (!(schema instanceof ActionSchema)) return invalidArgumentError('schema must be an ActionSchema.');
    const validation = schema.validate();
    if (!isOk(validation)) return validation;
    if (this.mode !== 'none') {
      return failedPreconditionError('Cannot change Action schema after it has started.');
    }
    this.schema = schema;
    return this.remapDefaultPorts();
  }

  /** Bind the application implementation invoked by a local run. */
  bindHandler(handler: ActionHandler): Status {
    if (typeof handler !== 'function') return invalidArgumentError('handler must be callable.');
    if (this.mode !== 'none') {
      return failedPreconditionError('Cannot change Action handler after it has started.');
    }
    this.handler = handler;
    return okStatus();
  }

  setSettings(settings: ActionSettings): Status {
    const validation = validateActionSettings(settings);
    if (!isOk(validation)) return validation;
    try {
      this.settings = { ...settings };
      return okStatus();
    } catch (error) {
      return invalidArgumentError('Action settings could not be copied.', [], error);
    }
  }

  bindStreamsOnInputsByDefault(bind: boolean): Status {
    if (typeof bind !== 'boolean') return invalidArgumentError('bind must be boolean.');
    this.settings.bindStreamsOnInputsByDefault = bind;
    return okStatus();
  }

  bindStreamsOnOutputsByDefault(bind: boolean): Status {
    if (typeof bind !== 'boolean') return invalidArgumentError('bind must be boolean.');
    this.settings.bindStreamsOnOutputsByDefault = bind;
    return okStatus();
  }

  clearInputsAfterRun(clear = true): Status {
    if (typeof clear !== 'boolean') return invalidArgumentError('clear must be boolean.');
    this.settings.clearInputsAfterRun = clear;
    return okStatus();
  }

  clearOutputsAfterRun(clear = true): Status {
    if (typeof clear !== 'boolean') return invalidArgumentError('clear must be boolean.');
    this.settings.clearOutputsAfterRun = clear;
    return okStatus();
  }

  bindNodeMap(nodeMap: NodeMap): Status {
    if (!(nodeMap instanceof NodeMap)) return invalidArgumentError('nodeMap must be a NodeMap.');
    this.nodeMap = nodeMap;
    return okStatus();
  }

  bindRegistry(registry: ActionRegistryLike | null): Status {
    if (registry !== null && !hasRegistryShape(registry)) {
      return invalidArgumentError('registry must implement ActionRegistryLike or be null.');
    }
    this.registry = registry;
    return okStatus();
  }

  /** Move lifetime/routing ownership to a session, retaining active tracking. */
  bindSession(session: ActionSessionContext | null): Status {
    if (session !== null && !hasSessionShape(session)) {
      return invalidArgumentError('session must implement ActionSessionContext or be null.');
    }
    if (!this.tracked || this.session === session) {
      this.session = session;
      return okStatus();
    }

    const previous = this.session;
    if (session !== null) {
      let tracked: Status;
      try {
        tracked = session.trackAction(this);
      } catch (error) {
        return statusFromUnknown(error, 'Tracking Action in Session raised an exception.');
      }
      if (!isStatus(tracked)) {
        return internalError('Session.trackAction() returned a non-Status value.');
      }
      if (!isOk(tracked)) return tracked;
    }
    try {
      previous?.untrackAction(this);
    } catch (error) {
      if (session !== null) {
        try { session.untrackAction(this); } catch { /* preserve primary status */ }
      }
      return statusFromUnknown(error, 'Removing Action from Session raised an exception.');
    }
    this.session = session;
    this.tracked = session !== null;
    return okStatus();
  }

  /** Bind remote dispatch and reattach all already stream-bound port nodes. */
  bindStream(stream: WireStream | null): Status {
    if (stream === this.stream) return okStatus();
    const previous = this.stream;
    const rebound: AsyncNode[] = [];
    for (const node of this.boundNodes) {
      let status = previous === null ? okStatus() : node.detachStream(previous);
      if (isOk(status) && stream !== null) status = node.attachStream(stream);
      if (!isOk(status)) {
        for (const completed of rebound) {
          if (stream !== null) completed.detachStream(stream);
          if (previous !== null) completed.attachStream(previous);
        }
        return status;
      }
      rebound.push(node);
    }
    this.stream = stream;
    return okStatus();
  }

  async getNode(nodeId: string): Promise<StatusOr<AsyncNode>> {
    return this.nodeMap.get(nodeId);
  }

  /** Open a named input node and optionally mirror local writes to the peer. */
  async getInput(name: string, bindStream?: boolean): Promise<StatusOr<AsyncNode>> {
    const valid = validateName(name);
    if (!isOk(valid)) return valid;
    const id = this.inputIds.get(name);
    if (id === undefined) return notFoundError(`Action input '${name}' is not mapped.`);
    const node = await this.nodeMap.get(id);
    if (!isOk(node)) return node;
    this.inputNodes.add(node);
    const bind = bindStream ??
      this.settings.bindStreamsOnInputsByDefault ??
      this.mode !== 'run';
    const attached = this.attachStreamIfRequested(node, bind);
    return isOk(attached) ? node : attached;
  }

  /** Open a named output node and optionally mirror local writes to the peer. */
  async getOutput(name: string, bindStream?: boolean): Promise<StatusOr<AsyncNode>> {
    const valid = validateName(name);
    if (!isOk(valid)) return valid;
    const id = this.outputIds.get(name);
    if (id === undefined) return notFoundError(`Action output '${name}' is not mapped.`);
    const node = await this.nodeMap.get(id);
    if (!isOk(node)) return node;
    this.outputNodes.add(node);
    const bind = bindStream ??
      this.settings.bindStreamsOnOutputsByDefault ??
      this.mode === 'run';
    const attached = this.attachStreamIfRequested(
      node,
      bind && name !== ACTION_STATUS_OUTPUT && name !== ACTION_DISPATCH_STATUS_OUTPUT,
    );
    return isOk(attached) ? node : attached;
  }

  async getPort(name: string): Promise<StatusOr<AsyncNode>> {
    const input = this.inputIds.has(name);
    const output = this.outputIds.has(name);
    if (input && output) {
      return failedPreconditionError(
        'Action port is both an input and output; select one explicitly.',
      );
    }
    if (input) return this.getInput(name);
    if (output) return this.getOutput(name);
    return notFoundError('Action port is not mapped.');
  }

  containsPort(name: string): boolean {
    return this.inputIds.has(name) || this.outputIds.has(name);
  }

  /** Snapshot the call id, name, port mappings, and headers for dispatch. */
  getActionMessage(): ActionMessage {
    return new ActionMessage({
      id: this.id,
      name: this.schema.name,
      inputs: [...this.schema.inputs.keys()].map(
        (name) => new Port(name, this.inputIds.get(name) ?? ''),
      ),
      outputs: [...this.schema.outputs.keys()].map(
        (name) => new Port(name, this.outputIds.get(name) ?? ''),
      ),
      headers: copyByteMap(this.headers),
    });
  }

  /** Adopt validated caller-supplied port node ids before local execution. */
  mapPortsFromMessage(message: ActionMessage): Status {
    if (!(message instanceof ActionMessage)) return invalidArgumentError('message must be an ActionMessage.');
    if (this.mode !== 'none') {
      return failedPreconditionError('Cannot remap Action ports after it has started.');
    }
    const validation = message.validate();
    if (!isOk(validation)) return validation;
    const inputStatus = this.validateMessagePorts(message.inputs, this.schema.inputs, 'input');
    if (!isOk(inputStatus)) return inputStatus;
    const outputStatus = this.validateMessagePorts(message.outputs, this.schema.outputs, 'output');
    if (!isOk(outputStatus)) return outputStatus;
    for (const port of message.inputs) this.inputIds.set(port.name, port.id);
    for (const port of message.outputs) this.outputIds.set(port.name, port.id);
    return okStatus();
  }

  getHeaders(): ByteMap { return copyByteMap(this.headers); }

  getHeader(name: string): StatusOr<Uint8Array | null> {
    const valid = validateName(name);
    if (!isOk(valid)) return valid;
    const value = this.headers.get(name.toLowerCase());
    return value === undefined ? null : new Uint8Array(value);
  }

  hasHeader(name: string): boolean {
    return isOk(validateName(name)) && this.headers.has(name.toLowerCase());
  }

  setHeader(name: string, value: ByteSource): Status {
    const valid = validateName(name);
    if (!isOk(valid)) return valid;
    const normalized = normalizeByteMap(new Map([[name, value]]));
    if (!isOk(normalized)) return normalized;
    this.headers.set(name.toLowerCase(), normalized.get(name)!);
    return okStatus();
  }

  removeHeader(name: string): Status {
    const valid = validateName(name);
    if (!isOk(valid)) return valid;
    this.headers.delete(name.toLowerCase());
    return okStatus();
  }

  forwardHeader(target: Action, name: string): Status {
    if (!(target instanceof Action)) return invalidArgumentError('target must be an Action.');
    const value = this.getHeader(name);
    if (!isOk(value)) return value;
    return value === null ? okStatus() : target.setHeader(name, value);
  }

  /** Copy framework-scoped metadata to a nested action. */
  forwardHeadersWithPrefix(target: Action, prefix = ACTION_HEADER_PREFIX): Status {
    if (!(target instanceof Action)) return invalidArgumentError('target must be an Action.');
    const folded = prefix.toLowerCase();
    for (const [name, value] of this.headers) {
      if (name.startsWith(folded)) {
        const status = target.setHeader(name, value);
        if (!isOk(status)) return status;
      }
    }
    return okStatus();
  }

  /**
   * Create a child action from a schema or registered name.
   *
   * With `propagateIo`, the child shares the parent's node map, stream, and
   * session, while retaining its own id, derived port ids, and AbortSignal.
   * The registry is shared in either mode, and framework headers are forwarded
   * when `forwardHeaders` is true. A shared session supplies nested concurrency
   * limits and session-wide abort; cancelling only the parent is not recursive.
   */
  makeNested(
    schemaOrName: ActionSchema | string,
    propagateIo = true,
    forwardHeaders = true,
  ): StatusOr<Action> {
    try {
      let schema: ActionSchema;
      let handler: ActionHandler | null = null;
      if (typeof schemaOrName === 'string') {
        if (this.registry === null) {
          return failedPreconditionError('Cannot resolve a nested Action without a registry.');
        }
        const found = this.registry.getSchema(schemaOrName);
        if (isStatus(found) && !isOk(found)) return found;
        if (!(found instanceof ActionSchema)) {
          return internalError('Action registry returned an invalid schema.');
        }
        schema = found;
        const foundHandler = this.registry.getHandler(schemaOrName);
        if (isStatus(foundHandler) && !isOk(foundHandler)) {
          handler = null;
        } else if (typeof foundHandler === 'function') {
          handler = foundHandler;
        } else {
          return internalError('Action registry returned an invalid handler.');
        }
      } else {
        schema = schemaOrName;
      }
      if (typeof propagateIo !== 'boolean' || typeof forwardHeaders !== 'boolean') {
        return invalidArgumentError('Nested Action propagation options must be boolean.');
      }
      const child = Action.create(schema, {
        handler,
        nodeMap: propagateIo ? this.nodeMap : undefined,
        stream: propagateIo ? this.stream : null,
        session: propagateIo ? this.session : null,
        registry: this.registry,
      });
      if (!isOk(child)) return child;
      child.parent = this;
      if (forwardHeaders) {
        const forwarded = this.forwardHeadersWithPrefix(child);
        if (!isOk(forwarded)) return forwarded;
      }
      return child;
    } catch (error) {
      return statusFromUnknown(error, 'Creating nested Action raised an exception.');
    }
  }

  /** Start the bound handler locally and return immediately. */
  run(): StatusOr<Action> {
    if (this.handler === null) {
      return failedPreconditionError('Action handler has not been set.');
    }
    const begun = this.begin('run');
    if (!isOk(begun)) return begun;
    const tracked = this.trackInSession();
    if (!isOk(tracked)) {
      this.mode = 'none';
      return tracked;
    }
    queueMicrotask(() => void this.runHandler());
    return this;
  }

  /** Queue this action for remote dispatch; use `waitForDispatch` for acceptance. */
  async call(wireHeaders: ByteMapInput = new Map()): Promise<StatusOr<Action>> {
    try {
      return await this.callInternal(wireHeaders);
    } catch (error) {
      this.untrackFromSession();
      if (this.mode === 'call' && this.completionStatus === null) this.mode = 'none';
      return statusFromUnknown(error, 'Calling Action raised an exception.');
    }
  }

  private async callInternal(
    wireHeaders: ByteMapInput,
  ): Promise<StatusOr<Action>> {
    const headers = normalizeWireHeaders(wireHeaders);
    if (!isOk(headers)) return headers;
    const begun = this.begin('call');
    if (!isOk(begun)) return begun;
    const tracked = this.trackInSession();
    if (!isOk(tracked)) {
      this.mode = 'none';
      return tracked;
    }
    const autofills = this.collectAutofillFragments();
    if (!isOk(autofills)) {
      this.untrackFromSession();
      this.mode = 'none';
      return autofills;
    }
    const message = new WireMessage({
      nodeFragments: autofills,
      actions: [this.getActionMessage()],
      headers,
    });
    let sent: Status;
    try {
      if (this.stream !== null) sent = this.stream.send(message);
      else if (this.session !== null) sent = this.session.send(message);
      else sent = failedPreconditionError(
        'Calling an Action requires an attached WireStream or Session.',
      );
    } catch (error) {
      sent = statusFromUnknown(error, 'Sending Action call raised an exception.');
    }
    if (!isStatus(sent)) {
      sent = internalError('Action transport send() returned a non-Status value.');
    }
    if (!isOk(sent)) {
      this.untrackFromSession();
      this.mode = 'none';
      return sent;
    }
    return this;
  }

  /** Await the remote dispatch acknowledgement, not handler completion. */
  async waitForDispatch(timeoutMs?: number): Promise<Status> {
    if (this.mode !== 'call') {
      return failedPreconditionError('Only a called Action has a dispatch status.');
    }
    return this.waitForStatus(this.dispatched.promise, timeoutMs, 'Action dispatch timed out.');
  }

  /** Await terminal local/remote completion after all lifecycle cleanup. */
  async wait(timeoutMs?: number): Promise<StatusOr<Action>> {
    if (this.mode === 'none') return failedPreconditionError('Action has not been run or called.');
    const status = await this.waitForStatus(this.done.promise, timeoutMs, 'Action wait timed out.');
    return isOk(status) ? this : status;
  }

  /**
   * Request cooperative cancellation once.
   *
   * Local handlers observe {@link signal}; remote calls also send the reserved
   * cancellation action. `cancel()` initiates the transition—await `wait()` if
   * teardown and output abort propagation must be complete.
   */
  cancel(): Status {
    if (this.completionStatus !== null || this.finishing || this.cancelRequested) {
      return okStatus();
    }
    this.cancelRequested = true;
    this.cancelController.abort();
    let first: Status = okStatus();
    for (const callback of this.cancelCallbacks) {
      try {
        const result = callback(this);
        if (result !== undefined) first = firstError(first, result);
      } catch (error) {
        first = firstError(first, statusFromUnknown(error, 'Action cancel callback raised.'));
      }
    }
    const cancelled = cancelledError('Action was cancelled.');
    if (this.mode === 'call') {
      first = firstError(first, this.sendRemoteCancel());
      this.completeCall(cancelled, false);
      queueMicrotask(() => void this.abortLocalCallOutputs(cancelled));
    } else if (this.mode === 'run') {
      queueMicrotask(() => void this.finishRun(cancelled));
    } else if (this.mode === 'none') {
      this.mode = 'cancelled';
      this.completionStatus = cancelled;
      this.done.resolve(cancelled);
    }
    return first;
  }

  setOnCancelled(callback: OnActionCancelled): Status {
    if (typeof callback !== 'function') return invalidArgumentError('callback must be callable.');
    this.cancelCallbacks.push(callback);
    return okStatus();
  }

  /** Session protocol hook for the reserved dispatch-status node. */
  setDispatchStatus(status: Status): Status {
    if (!isStatus(status)) return invalidArgumentError('status must be an A11 Status.');
    if (this.mode !== 'call' || this.dispatchStatus !== null) return okStatus();
    this.dispatchStatus = status;
    this.dispatched.resolve(status);
    return okStatus();
  }

  /** Session protocol hook for the reserved completion-status node. */
  setCompletionStatus(status: Status): Status {
    if (!isStatus(status)) return invalidArgumentError('status must be an A11 Status.');
    if (this.mode !== 'call') return failedPreconditionError('Action is not a call.');
    if (this.dispatchStatus === null) {
      this.dispatchStatus = okStatus();
      this.dispatched.resolve(this.dispatchStatus);
    }
    if (this.completionStatus === null) this.completeCall(status, true);
    else if (this.cancelRequested) this.untrackFromSession();
    return okStatus();
  }

  /** Materialize schema autofills into empty input nodes before a handler runs. */
  async applyInputAutofills(): Promise<Status> {
    try {
      return await this.applyInputAutofillsInternal();
    } catch (error) {
      return statusFromUnknown(error, 'Applying Action input autofills raised an exception.');
    }
  }

  private async applyInputAutofillsInternal(): Promise<Status> {
    if (this.inputAutofillsApplied) return okStatus();
    const work: Array<[string, readonly (NodeFragment | null)[]]> = [];
    for (const [name, port] of this.schema.inputs) {
      if (port.autofills.length > 0) {
        const id = this.inputIds.get(name);
        if (id !== undefined) work.push([id, port.autofills]);
      }
    }
    const nodes: AsyncNode[] = [];
    for (const [id] of work) {
      const node = await this.nodeMap.get(id);
      if (!isOk(node)) return node;
      const writable = await node.isWritable();
      if (!isOk(writable)) return writable;
      if (!writable) return failedPreconditionError(`Autofilled input '${id}' is not writable.`);
      const size = await node.chunkStore.size();
      if (!isOk(size)) return size;
      if (size !== 0) return failedPreconditionError(`Autofilled input '${id}' already contains data.`);
      this.inputNodes.add(node);
      nodes.push(node);
    }
    for (let index = 0; index < work.length; ++index) {
      const [id, autofills] = work[index]!;
      const node = nodes[index]!;
      for (const autofill of autofills) {
        const stored = autofill === null
          ? await node.finalize(undefined, { wait: true, close: false })
          : await node.putFragment(new NodeFragment({
              id,
              data: autofill.data,
              seq: autofill.seq,
              continued: autofill.continued,
            }));
        if (!isOk(stored)) return stored;
      }
      const closed = await node.close();
      if (!isOk(closed)) return closed;
    }
    this.inputAutofillsApplied = true;
    return okStatus();
  }

  private begin(mode: 'run' | 'call'): Status {
    if (this.cancelRequested) return cancelledError('Action was cancelled.');
    if (this.mode !== 'none') return failedPreconditionError('Action has already started.');
    this.mode = mode;
    return okStatus();
  }

  /**
   * Log `value` on the reserved {@link ACTION_LOG_OUTPUT} port.
   *
   * The value becomes a chunk the way `node.put(value)` would make one -- a
   * `string` is `text/plain`, a `Uint8Array` is `application/octet-stream` -- and
   * the chunk always carries a timestamp.
   *
   * Only a running handler may log: logging before `run`, or on the calling side
   * of a `call`, is a failed precondition, because the port would have nowhere to
   * go and no reader to close it. Nothing else about logging fails the action --
   * once the chunk is built, a transport or lifecycle problem is reported through
   * the sink rather than returned.
   *
   * Where it goes: always to the process's action log sink, and additionally onto
   * the log port when something could read it -- a peer is attached, or a local
   * consumer claimed the port with {@link getLogNode}. Nobody has to drain it and
   * nobody has to close it.
   */
  async log(value: unknown, options: LogOptions = {}): Promise<Status> {
    let chunk: Chunk;
    if (value instanceof Chunk) {
      if (options.mimetype !== undefined && options.mimetype !== '') {
        return invalidArgumentError(
          'Cannot give a log mimetype for a chunk that already has one.',
        );
      }
      chunk = value;
    } else {
      const made = await toChunk(value, options.mimetype ?? '');
      if (!isOk(made)) return made;
      chunk = made;
    }
    return this.writeLog(chunk, options);
  }

  /**
   * Log a formatted line: `%s` is replaced by each argument in turn.
   *
   * Uses positional `%s` replacements and `%%` for literal percent signs across
   * language runtimes.
   */
  async logf(format: string, ...args: unknown[]): Promise<Status> {
    let index = 0;
    const filled = format.replace(/%[%s]/g, (found) =>
      found === '%%' ? '%' : String(args[index++] ?? ''),
    );
    return this.log(filled);
  }

  /**
   * Log a formatted line with explicit options.
   *
   * A second name rather than an overload, so it matches the C++ surface, where
   * a leading-options overload of `Logf` is ambiguous against the format spec.
   */
  async logfWith(
    options: LogOptions,
    format: string,
    ...args: unknown[]
  ): Promise<Status> {
    let index = 0;
    const filled = format.replace(/%[%s]/g, (found) =>
      found === '%%' ? '%' : String(args[index++] ?? ''),
    );
    return this.log(filled, options);
  }

  /**
   * Return the log port's node, claiming it for this consumer.
   *
   * Claiming suppresses the process sink for this action, so a consumer that
   * presents the logs itself does not also have them reported twice. Claim before
   * the action runs: logs written earlier have already gone to the sink.
   *
   * The stream is not bound: on the calling side, binding an output would echo
   * received fragments back to the peer.
   */
  async getLogNode(): Promise<StatusOr<AsyncNode>> {
    this.logClaimed = true;
    return this.getOutput(ACTION_LOG_OUTPUT, false);
  }

  /** Apply `options` to `chunk`, report it, and write it where anything reads. */
  private async writeLog(chunk: Chunk, options: LogOptions): Promise<Status> {
    const level = parseLogLevel(options.level ?? '');
    if (level === null) {
      return invalidArgumentError(
        `Unknown log level '${options.level}'; expected one of ${LOG_LEVELS.join(', ')}.`,
      );
    }
    if (this.mode !== 'run') {
      return failedPreconditionError(
        'Only a running Action may log; a caller logs on its own action.',
      );
    }
    const metadata = chunk.metadata ?? new ChunkMetadata();
    if (metadata.mimetype === '') metadata.mimetype = OCTET_STREAM_MIMETYPE;
    if (isStatusChunk(chunk)) {
      return invalidArgumentError(
        'Cannot log a status chunk; log its message instead.',
      );
    }
    metadata.timestamp = new Date();
    // The caller's map first, then the named options, so an explicit level wins
    // over a 'level' the same caller also put in the map.
    if (options.metadata !== undefined) {
      const pairs = options.metadata instanceof Map
        ? options.metadata.entries()
        : Object.entries(options.metadata);
      for (const [key, value] of pairs) {
        metadata.attributes.set(
          key,
          typeof value === 'string' ? encoder.encode(value) : value,
        );
      }
    }
    metadata.attributes.set(LOG_LEVEL_ATTRIBUTE, encoder.encode(level));
    metadata.attributes.set(
      LOG_INTERNAL_ATTRIBUTE,
      encoder.encode(options.internal === true ? LOG_INTERNAL_TRUE : LOG_INTERNAL_FALSE),
    );
    if (options.channel !== undefined && options.channel !== '') {
      metadata.attributes.set(LOG_CHANNEL_ATTRIBUTE, encoder.encode(options.channel));
    }
    if (options.file !== undefined && options.file !== '') {
      metadata.attributes.set(LOG_FILE_ATTRIBUTE, encoder.encode(options.file));
    }
    if (options.lineno !== undefined) {
      metadata.attributes.set(LOG_LINENO_ATTRIBUTE, encoder.encode(String(options.lineno)));
    }
    chunk.metadata = metadata;

    if (!this.logClaimed) {
      reportLog(logRecordFromChunk(chunk, this.schema.name, this.id));
    }
    // Nothing reads a local log port nobody claimed, so materialising it would
    // buffer every line of a narrating action for the length of the run and then
    // throw them away. A peer is always a reader: it is mirroring the node.
    const readable = this.logClaimed || this.stream !== null || this.session !== null;
    if (!readable || this.finishing) return okStatus();

    // From here on nothing is returned to the handler: a log that could not be
    // written is a fault in the logging, not in the action.
    const node = await this.getOutput(ACTION_LOG_OUTPUT, this.stream !== null);
    if (!isOk(node)) return okStatus();
    await node.putChunk(chunk);
    return okStatus();
  }

  private remapDefaultPorts(): Status {
    this.inputIds.clear();
    this.outputIds.clear();
    for (const name of this.schema.inputs.keys()) {
      const id = Action.makeNodeId(this.id, name);
      if (!isOk(id)) return id;
      this.inputIds.set(name, id);
    }
    for (const name of this.schema.outputs.keys()) {
      const id = Action.makeNodeId(this.id, name);
      if (!isOk(id)) return id;
      this.outputIds.set(name, id);
    }
    for (const name of [
      ACTION_STATUS_OUTPUT,
      ACTION_DISPATCH_STATUS_OUTPUT,
      ACTION_LOG_OUTPUT,
    ]) {
      const id = Action.makeNodeId(this.id, name);
      if (!isOk(id)) return id;
      this.outputIds.set(name, id);
    }
    return okStatus();
  }

  private attachStreamIfRequested(node: AsyncNode, bind: boolean): Status {
    if (!bind || this.stream === null) return okStatus();
    const attached = node.attachStream(this.stream);
    if (isOk(attached)) this.boundNodes.add(node);
    return attached;
  }

  private validateMessagePorts(
    ports: readonly Port[],
    schemaPorts: ReadonlyMap<string, unknown>,
    kind: string,
  ): Status {
    const seen = new Set<string>();
    for (const port of ports) {
      const valid = port.validate();
      if (!isOk(valid)) return valid;
      if (!schemaPorts.has(port.name)) {
        return failedPreconditionError(`Unknown Action ${kind} port '${port.name}'.`);
      }
      if (seen.has(port.name)) {
        return invalidArgumentError(`Action ${kind} port '${port.name}' is duplicated.`);
      }
      seen.add(port.name);
    }
    return okStatus();
  }

  private collectAutofillFragments(): StatusOr<NodeFragment[]> {
    const fragments: NodeFragment[] = [];
    for (const [name, port] of this.schema.inputs) {
      if (port.autofills.length === 0) continue;
      const id = this.inputIds.get(name);
      if (id === undefined) continue;
      const start = fragments.length;
      for (const autofill of port.autofills) {
        fragments.push(new NodeFragment({
          id,
          data: autofill?.data ?? makeNullChunk(),
          seq: autofill?.seq ?? null,
          continued: autofill?.continued ?? false,
        }));
      }
      if (fragments.length > start) fragments[fragments.length - 1]!.continued = false;
    }
    return fragments;
  }

  private async runHandler(): Promise<void> {
    let status: Status = okStatus();
    const nested = this.parent !== null;
    let acquired = false;
    try {
      if (this.session?.acquireActionSlot !== undefined) {
        const acquiredStatus = await this.session.acquireActionSlot(
          nested,
          this.signal,
        );
        status = isStatus(acquiredStatus)
          ? acquiredStatus
          : internalError(
              'Session.acquireActionSlot() returned a non-Status value.',
            );
        acquired = isOk(status);
      }
      if (isOk(status)) {
        if (this.cancelRequested) {
          status = cancelledError('Action was cancelled.');
        } else {
          status = await this.applyInputAutofills();
          if (isOk(status)) {
            try {
              const result = await this.handler!(this);
              if (result === undefined) status = okStatus();
              else if (isStatus(result)) status = result;
              else status = internalError(
                'Action handler returned a value that is not a Status.',
              );
            } catch (error) {
              status = statusFromUnknown(
                error,
                'Action handler raised an exception.',
              );
            }
          }
        }
      }
    } catch (error) {
      status = statusFromUnknown(error, 'Running Action handler raised an exception.');
    } finally {
      if (acquired) {
        try {
          this.session?.releaseActionSlot?.(nested);
        } catch (error) {
          status = firstError(
            status,
            statusFromUnknown(error, 'Releasing Session Action slot raised.'),
          );
        }
      }
    }
    if (this.cancelRequested) status = cancelledError('Action was cancelled.');
    await this.finishRun(status);
  }

  private async finishRun(initialStatus: Status): Promise<Status> {
    if (this.finishing || this.completionStatus !== null) return okStatus();
    this.finishing = true;
    try {
      let finalStatus = initialStatus;
      const outputStatus = await this.finishOutputNodes(finalStatus);
      if (isOk(finalStatus) && !isOk(outputStatus)) finalStatus = outputStatus;
      let communicated = await this.communicateStatus(finalStatus);
      if (isOk(finalStatus) && !isOk(communicated)) {
        finalStatus = communicated;
        await this.finishOutputNodes(finalStatus);
        communicated = await this.communicateStatus(finalStatus);
      }
      await this.releaseNodesAfterRun();
      this.completionStatus = finalStatus;
      this.done.resolve(finalStatus);
      this.untrackFromSession();
      return communicated;
    } catch (error) {
      const failure = firstError(
        initialStatus,
        statusFromUnknown(error, 'Finishing Action run raised an exception.'),
      );
      this.completionStatus = failure;
      this.done.resolve(failure);
      this.untrackFromSession();
      return failure;
    }
  }

  private async finishOutputNodes(status: Status): Promise<Status> {
    // The log port is closed with the ordinary outputs -- that is what makes it
    // something a handler never has to close and a reader never waits forever on.
    const ids = [...this.schema.outputs.keys(), ACTION_LOG_OUTPUT]
      .map((name) => this.outputIds.get(name))
      .filter((id): id is string => id !== undefined);
    let first: Status = okStatus();
    if (!isOk(status) && this.stream !== null && ids.length > 0) {
      const chunk = statusToChunk(status);
      if (isOk(chunk)) {
        try {
          const sent = this.stream.send(new WireMessage({
            nodeFragments: ids.map((id) => new NodeFragment({
              id,
              data: chunk,
              seq: 0,
              continued: false,
            })),
          }));
          first = firstError(
            first,
            isStatus(sent)
              ? sent
              : internalError('WireStream.send() returned a non-Status value.'),
          );
        } catch (error) {
          first = firstError(
            first,
            statusFromUnknown(error, 'Sending Action output status raised.'),
          );
        }
      } else first = firstError(first, chunk);
    }
    for (const id of ids) {
      const node = await this.nodeMap.get(id);
      if (!isOk(node)) { first = firstError(first, node); continue; }
      this.outputNodes.add(node);
      const writable = await node.isWritable();
      if (!isOk(writable)) { first = firstError(first, writable); continue; }
      if (!writable) continue;
      const closed = isOk(status)
        ? await node.close()
        : await node.abortWithStatus(status);
      first = firstError(first, closed);
    }
    return first;
  }

  private async communicateStatus(status: Status): Promise<Status> {
    const chunk = statusToChunk(status);
    if (!isOk(chunk)) return chunk;
    const id = this.outputIds.get(ACTION_STATUS_OUTPUT);
    if (id === undefined) return internalError('Action status output is not mapped.');
    const node = await this.nodeMap.get(id);
    if (!isOk(node)) return node;
    const writable = await node.isWritable();
    if (!isOk(writable)) return writable;
    if (!writable) return failedPreconditionError('Action status node was already finalized.');
    if (this.stream !== null) {
      const attached = node.attachStream(this.stream);
      if (!isOk(attached)) return attached;
      this.boundNodes.add(node);
    }
    const stored = await node.putFragment(new NodeFragment({
      id,
      data: chunk,
      seq: 0,
      continued: false,
    }));
    if (!isOk(stored)) return stored;
    return node.close();
  }

  private async releaseNodesAfterRun(): Promise<Status> {
    let first = this.detachBoundNodes();
    if (this.settings.clearInputsAfterRun) {
      for (const id of this.inputIds.values()) {
        const removed = this.nodeMap.discard(id);
        if (!isOk(removed)) first = firstError(first, removed);
        else removed?.cancelReader();
      }
    }
    if (this.settings.clearOutputsAfterRun) {
      for (const id of this.outputIds.values()) {
        const removed = this.nodeMap.discard(id);
        if (!isOk(removed)) first = firstError(first, removed);
      }
    }
    this.inputNodes.clear();
    this.outputNodes.clear();
    return first;
  }

  private detachBoundNodes(): Status {
    let first: Status = okStatus();
    if (this.stream !== null) {
      for (const node of this.boundNodes) {
        first = firstError(first, node.detachStream(this.stream));
      }
    }
    this.boundNodes.clear();
    return first;
  }

  private trackInSession(): Status {
    if (this.session === null || this.tracked) return okStatus();
    let status: Status;
    try {
      status = this.session.trackAction(this);
    } catch (error) {
      return statusFromUnknown(error, 'Tracking Action in Session raised an exception.');
    }
    if (!isStatus(status)) {
      return internalError('Session.trackAction() returned a non-Status value.');
    }
    if (isOk(status)) this.tracked = true;
    return status;
  }

  private untrackFromSession(): void {
    if (!this.tracked) return;
    this.tracked = false;
    try { this.session?.untrackAction(this); }
    catch { /* Session cleanup must not escape an Action lifecycle. */ }
  }

  private sendRemoteCancel(): Status {
    try {
      const cancel = new ActionMessage({
        id: randomId('action-'),
        name: CANCEL_ACTION_NAME,
        headers: new Map([[CANCEL_ACTION_HEADER, new TextEncoder().encode(this.id)]]),
      });
      const message = new WireMessage({ actions: [cancel] });
      const sent = this.stream !== null
        ? this.stream.send(message)
        : this.session !== null
          ? this.session.send(message)
          : failedPreconditionError(
              'Cancelling a called Action requires a WireStream or Session.',
            );
      return isStatus(sent)
        ? sent
        : internalError('Action transport send() returned a non-Status value.');
    } catch (error) {
      return statusFromUnknown(error, 'Sending remote Action cancellation raised.');
    }
  }

  private completeCall(status: Status, removeFromSession: boolean): void {
    if (this.completionStatus === null) {
      this.completionStatus = status;
      this.detachBoundNodes();
      this.done.resolve(status);
    }
    if (removeFromSession) this.untrackFromSession();
  }

  private async abortLocalCallOutputs(status: NonOkStatus): Promise<void> {
    try {
      for (const name of this.schema.outputs.keys()) {
        const id = this.outputIds.get(name);
        if (id === undefined) continue;
        const node = await this.nodeMap.get(id);
        if (!isOk(node)) continue;
        const writable = await node.isWritable();
        if (isOk(writable) && writable) await node.abortWithStatus(status);
      }
    } catch {
      // Cancellation has already completed; local output cleanup is best effort.
    }
  }

  private async waitForStatus(
    promise: Promise<Status>,
    timeoutMs: number | undefined,
    message: string,
  ): Promise<Status> {
    if (timeoutMs !== undefined && (!Number.isFinite(timeoutMs) || timeoutMs < 0)) {
      return invalidArgumentError('timeoutMs must be a non-negative finite number.');
    }
    if (timeoutMs === undefined) return promise;
    const timeout = new Deferred<Status>();
    const timer = setTimeout(() => timeout.resolve(deadlineExceededError(message)), timeoutMs);
    try { return await Promise.race([promise, timeout.promise]); }
    catch (error) { return statusFromUnknown(error, 'Waiting for Action raised an exception.'); }
    finally { clearTimeout(timer); }
  }
}
