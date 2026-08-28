/**
 * What the four guide demos that talk to `a11.demos.web_demos_server` all need:
 * a WebSocket session, the backend toolbar (provider, model, key, base URL),
 * one chat turn against `interact_with_llm`, the registry a page serves, and a
 * few DOM helpers.
 *
 * The demos share this turn lifecycle while presenting different results such
 * as reports, canvas updates, and images. It is a compact version of the
 * IntelliJ webview sequence in
 * `intellij-plugin/webview/src/a11client.ts`.
 */

import {
    Action,
    ActionRegistry,
    ActionSchema,
    INTERACT_WITH_LLM_SCHEMA,
    LlmHeaders,
    Session,
    StatusCode,
    StreamMode,
    WebSocketWireStream,
    fromChunk,
    getToolDefinitions,
    isOk,
    logRecordFromChunk,
    logText,
    makeTextMessageInteraction,
    type AsyncNode,
    type Chunk,
    type Interaction,
    type LogLevel,
    type PortValueSchemas,
    type Status,
    type WireStream,
} from '../src/index.js';

/** Unwrap a `StatusOr`, turning a failure into an error the page can show. */
export const need = <T>(value: T | Status): T => {
    if (!isOk(value)) throw new Error(`${StatusCode[(value as Status).code]}: ${(value as Status).message}`);
    return value as T;
};

/** How long a read waits before the page decides the backend has stopped. */
export const READ_TIMEOUT_MS = 300_000;

/**
 * The hosted demo backend the guides point at.
 *
 * The default is the exchange's WebSocket relay for the hosted `demoserver`
 * identity. A locally run `python -m a11.demos.web_demos_server` is
 * `ws://127.0.0.1:9010/a11-demos`, which the same field accepts. The
 * `https://` or `http://` spelling also works because [connect] rewrites it.
 */
export const DEFAULT_SERVER_URL = 'wss://a11.to/ws/demoserver';

const DEADLINE_HEADER = 'x-a11-deadline';
const TURN_DEADLINE_MS = 180_000;

// --- The backend a turn is answered by --------------------------------------

/** Provider settings sent as call headers. */
export interface Backend {
    provider: string;
    model: string;
    apiKey: string;
    baseUrl: string;
}

/**
 * What each provider is usually called and reached at.
 *
 * Hosted providers need a key and no base URL. Ollama uses a base URL and no
 * key. Other interaction handling remains provider-independent.
 *
 * The action server resolves the base URL. The default therefore selects the
 * demo backend's local Ollama instance and requires no API key.
 */
export const BACKEND_DEFAULTS: Record<string, { model: string; baseUrl: string }> = {
    ollama: {model: 'glm-5.3-flash:cloud', baseUrl: 'https://ollama.com'},
    claude: {model: 'claude-sonnet-4-6', baseUrl: ''},
    gemini: {model: 'gemini-3.5-flash', baseUrl: ''},
};

/** The visible demo key; the server substitutes the real one. */
export const DEMO_API_KEY = 'use-a11-demo-resources';

/**
 * The provider/model/key/base-URL controls of a demo, as one value.
 *
 * Switching the provider refills the model and base-URL fields with that
 * provider's usual pair, which is the only reason this is a class: the four
 * fields are one choice, not four.
 */
export class BackendControls {
    private readonly provider: HTMLSelectElement;
    private readonly model: HTMLInputElement;
    private readonly apiKey: HTMLInputElement;
    private readonly baseUrl: HTMLInputElement;
    readonly server: HTMLInputElement;

    constructor(prefix: string, onChange?: () => void) {
        this.provider = document.querySelector<HTMLSelectElement>(`#${prefix}-provider`)!;
        this.model = document.querySelector<HTMLInputElement>(`#${prefix}-model`)!;
        this.apiKey = document.querySelector<HTMLInputElement>(`#${prefix}-api-key`)!;
        this.baseUrl = document.querySelector<HTMLInputElement>(`#${prefix}-base-url`)!;
        this.server = document.querySelector<HTMLInputElement>(`#${prefix}-server`)!;
        // Pre-fill the demo key so a visitor sees the demo work without setup.
        if (!this.apiKey.value) this.apiKey.value = DEMO_API_KEY;
        this.provider.onchange = () => {
            const defaults = BACKEND_DEFAULTS[this.provider.value];
            if (defaults) {
                this.model.value = defaults.model;
                this.baseUrl.value = defaults.baseUrl;
            }
            onChange?.();
        };
    }

    get value(): Backend {
        return {
            provider: this.provider.value,
            model: this.model.value.trim(),
            apiKey: this.apiKey.value.trim(),
            baseUrl: this.baseUrl.value.trim(),
        };
    }
}

/**
 * The backend, as the headers that carry
 * it on a call. Empty ones are left off.
 */
export function LlmHeadersFor(backend: Backend): Array<[string, string]> {
    const headers: Array<[string, string]> = [
        [LlmHeaders.PROVIDER, backend.provider],
        [LlmHeaders.MODEL, backend.model],
    ];
    if (backend.apiKey) headers.push([LlmHeaders.API_KEY, backend.apiKey]);
    if (backend.baseUrl) headers.push([LlmHeaders.BASE_URL, backend.baseUrl]);
    return headers;
}

// --- The session -------------------------------------------------------------

/** A live connection to the demo server. */
export interface Connection {
    session: Session;
    stream: WireStream;
}

/**
 * The WebSocket URL of a service address.
 *
 * Services may publish `https://host:port/path`, while the WebSocket transport
 * requires `wss://`. This helper accepts either form for the same endpoint.
 */
export function webSocketUrl(url: string): string {
    return url.trim().replace(/^http(s?):\/\//i, 'ws$1://');
}

/**
 * A lightweight browser/device fingerprint for rate-limit bucketing.
 *
 * This is not a tracking identifier — it distinguishes devices that share
 * an IP so the rate limiter can give each one its own budget. Sent as a
 * query parameter because the browser WebSocket API does not support
 * custom handshake headers.
 */
function deviceFingerprint(): string {
    const parts = [
        navigator.userAgent,
        navigator.language,
        `${screen.width}x${screen.height}x${screen.colorDepth}`,
        Intl.DateTimeFormat().resolvedOptions().timeZone,
        navigator.hardwareConcurrency?.toString() ?? '',
    ];
    // A simple hash: turn the concatenation into a short hex string.
    let hash = 0;
    const joined = parts.join('|');
    for (let i = 0; i < joined.length; i++) {
        hash = ((hash << 5) - hash + joined.charCodeAt(i)) | 0;
    }
    return (hash >>> 0).toString(16).padStart(8, '0');
}

/**
 * Open a session to the demo server over one WebSocket.
 *
 * `registry` is what the *page* serves: an empty one for a demo that only calls
 * out, and a populated one for the demo whose actions the model calls back
 * into. It is bound to the session before the stream is attached, so an inbound
 * call cannot arrive before there is a handler for it -- and so that the
 * backend, which asks this session what it serves over `__list_actions__`, gets
 * the right answer whenever it asks. There is nothing to announce.
 *
 * A lightweight device fingerprint is appended to the URL so the server
 * can rate-limit by device without custom WebSocket headers.
 */
export async function connect(url: string, registry = new ActionRegistry()): Promise<Connection> {
    const session = need(Session.create({actionRegistry: registry, noStreamTimeoutMs: null}));
    const wsUrl = new URL(webSocketUrl(url));
    wsUrl.searchParams.set('fp', deviceFingerprint());
    const stream = need(WebSocketWireStream.connect(wsUrl.toString()));
    need(await session.addStream(stream, StreamMode.START));
    return {session, stream};
}

/**
 * Try to reach the demo server early, so the page tells the user right away
 * if it cannot connect rather than waiting for the first action.
 *
 * Returns the established connection on success, or shows a polished
 * message in `errorRegion` and returns null.
 */
export async function probeConnection(
    url: string,
    errorRegion: HTMLElement,
    registry?: ActionRegistry,
): Promise<Connection | null> {
    try {
        return await connect(url, registry);
    } catch {
        errorRegion.innerHTML = '';
        const banner = document.createElement('div');
        banner.className = 'a11-connection-banner';
        banner.innerHTML =
            '<strong>Could not reach the demo server.</strong> ' +
            'Check that the server URL above is correct and reachable. ' +
            'If you are running your own backend, make sure it is started.';
        errorRegion.append(banner);
        return null;
    }
}

/** Make an action on a connection, ready to be `call`ed on the far side. */
export function makeCall(connection: Connection, schema: ActionSchema): Action {
    return need(
        Action.create(schema, {
            session: connection.session,
            stream: connection.stream,
            nodeMap: connection.session.getNodeMap(),
        }),
    );
}

/** Read one output port to its end, value by value. */
export async function readPort(
    action: Action,
    port: string,
    onValue: (value: unknown) => void,
    timeoutMs = READ_TIMEOUT_MS,
): Promise<void> {
    const node = need(await action.getOutput(port, false));
    for (; ;) {
        const next = need(await node.next({timeoutMs}));
        if (next === null) return;
        onValue(next);
    }
}

/**
 * Claim an action's log port, before it is dispatched.
 *
 * Every A11 action has a reserved log port outside its schema, so a page can
 * follow any slow action without agreeing on a custom narration port.
 *
 * Two rules, and the order of the calls in a page is how they are kept. Claim
 * *before* `call()`, because a log written
 * before anything holds the port goes to
 * the server's own log and is gone. And read it with `readLogFrom` *alongside*
 * the outputs rather than before them, because nothing finalizes a log port --
 * the action's ending closes it.
 */
export async function claimLog(action: Action): Promise<AsyncNode> {
    return need(await action.getLogNode());
}

/** Read lines from a log port claimed by `claimLog`. */
export async function readLogFrom(
    node: AsyncNode,
    onLine: (line: string, level: LogLevel) => void,
    timeoutMs = READ_TIMEOUT_MS,
): Promise<void> {
    for (; ;) {
        const chunk = need(await node.nextChunk(timeoutMs));
        if (chunk === null) return;
        const record = logRecordFromChunk(chunk);
        // Internal lines are A11 narrating its own dispatch: useful when
        // debugging the runtime, noise in front of somebody reading a demo.
        if (!record.internal) onLine(logText(record), record.level);
    }
}

/**
 * Read an output port and keep nothing: an undrained port stalls its producer.
 */
export async function drainPort(action: Action, port: string): Promise<void> {
    try {
        await readPort(action, port, () => {
        });
    } catch {
        // A port the page does not use must never fail the turn it belongs to.
    }
}

// --- One chat turn -----------------------------------------------------------

/** What a turn reports while it runs. */
export interface TurnCallbacks {
    onToken?: (text: string) => void;
    onThought?: (text: string) => void;
}

/**
 * What a turn is: a conversation so far, a new question, and who answers it.
 */
export interface TurnRequest extends TurnCallbacks {
    connection: Connection;
    backend: Backend;
    prompt: string;
    /** Every interaction of the conversation so far, oldest first. */
    history: readonly Interaction[];
    /**
     * Rides on the first interaction of a conversation; later ones ignore it.
     */
    systemPrompt?: string;
    /** Actions the page serves that this turn may use, already announced. */
    tools?: readonly ActionSchema[];
    /** Per-port JSON Schemas, for ports whose MIME type does not say enough. */
    portSchemas?: Readonly<Record<string, PortValueSchemas>>;
}

/**
 * Run one turn and return the interactions it added.
 *
 * The caller appends them to its own history: the conversation lives in the
 * page as the provider's own interaction objects, so the next turn puts the
 * whole of it back in front of the model, and the backend records the same
 * objects as it goes (which is what makes a reload continue rather than start
 * over).
 */
export async function runTurn(request: TurnRequest): Promise<Interaction[]> {
    const {connection, backend, prompt, history} = request;
    const registry = connection.session.getActionRegistry();

    const call = makeCall(connection, INTERACT_WITH_LLM_SCHEMA);
    need(call.setHeader(DEADLINE_HEADER, String(Date.now() + TURN_DEADLINE_MS)));
    for (const [header, value] of LlmHeadersFor(backend)) need(call.setHeader(header, value));
    const toolNames = (request.tools ?? []).map((schema) => schema.name);
    if (toolNames.length > 0) {
        // The allow-list is the request: a tool the model is not offered here
        // cannot be called, and is not even described to it.
        need(call.setHeader(LlmHeaders.ALLOWED_LLM_ACTIONS, toolNames.join(',')));
    }
    need(await call.call());

    const question = need(
        await makeTextMessageInteraction(prompt, history.length === 0 ? (request.systemPrompt ?? '') : ''),
    );

    const interactions = need(await call.getInput('interactions'));
    for (const interaction of history) need(await interactions.put(interaction));
    need(await interactions.finalize(question));

    // Closed empty, so the backend applies its own default request config.
    const config = need(await call.getInput('config'));
    need(await config.finalize());

    const definitions = need(getToolDefinitions(registry, toolNames, request.portSchemas ?? {}));
    const tools = need(await call.getInput('tools'));
    for (const definition of definitions) need(await tools.put(definition));
    need(await tools.finalize());

    // Start waiting for the action's terminal status early.  If the handler
    // fails before writing any output (wrong model, rate limit, bad key), the
    // promise settles with the error immediately; the race below surfaces it
    // instead of hanging on a port read that will never produce data.
    const termination = call.wait(READ_TIMEOUT_MS);

    // A non-OK terminal status, turned into a rejection so Promise.race
    // can pick it up.  On success the promise never settles, letting the
    // normal port read win the race.
    const earlyFailure = termination.then((result) => {
        need(result);
        return new Promise<void>(() => {});
    });

    // Thoughts and interactions are read alongside the text, not after it: all
    // three are written from the one provider stream as it arrives, and reading
    // them in sequence would hold a model's thinking back until it stopped
    // talking.
    const produced: Interaction[] = [];
    const thoughts = request.onThought
        ? readPort(call, 'thoughts', (value) => request.onThought?.(String(value))).catch(() => {
        })
        : Promise.resolve();
    const interactionsOut = readPort(call, 'new_interactions', (value) => {
        produced.push(value as Interaction);
    }).catch(() => {});
    const events = drainPort(call, 'event_stream');

    await Promise.race([
        readPort(call, 'text_output', (value) => request.onToken?.(String(value))),
        earlyFailure,
    ]);
    await thoughts;
    await interactionsOut;
    await events;

    // The turn's terminal status is the only place a failure *after* the first
    // token shows up, so dropping it would turn "the turn died" into "the model
    // said nothing".
    need(await termination);

    return [question, ...produced];
}

// --- Reading a conversation back ---------------------------------------------

/** Text of one decoded content payload, whatever shape the backend wrote. */
function payloadText(value: unknown): string {
    if (typeof value === 'string') return value;
    if (!value || typeof value !== 'object') return '';
    const record = value as { content?: unknown; text?: unknown };
    if (typeof record.content === 'string') return record.content;
    if (Array.isArray(record.content)) {
        return record.content
            .map((block) => {
                if (!block || typeof block !== 'object') return '';
                const part = block as { type?: unknown; text?: unknown };
                return part.type === 'text' && typeof part.text === 'string' ? part.text : '';
            })
            .join('');
    }
    return typeof record.text === 'string' ? record.text : '';
}

/**
 * Best-effort readable text of an interaction, for drawing a stored
 * conversation.
 *
 * It reads the content *shapes* rather than the provider: the neutral
 * `{role, content: [{type: 'text', text}]}` envelope this side writes, and the
 * provider message dumps a backend stores. Tool calls and images contribute
 * nothing, which is what a transcript wants.
 */
export async function interactionText(interaction: Interaction): Promise<string> {
    const parts: string[] = [];
    for (const item of interaction.content ?? []) {
        const decoded = await fromChunk(item as Chunk);
        parts.push(isOk(decoded) ? payloadText(decoded) : '');
    }
    return parts.join('');
}

// --- DOM ---------------------------------------------------------------------

/** Append a chat bubble and keep the transcript scrolled to it. */
export function addBubble(
    container: HTMLElement,
    text: string,
    kind: 'question' | 'answer' | 'note',
): HTMLDivElement {
    const bubble = document.createElement('div');
    bubble.className = `a11-bubble ${kind}`;
    bubble.textContent = text;
    container.append(bubble);
    container.scrollTop = container.scrollHeight;
    return bubble;
}

/** Append one line to a log pane. */
export function addLine(container: HTMLElement, text: string, kind = ''): void {
    const line = document.createElement('div');
    line.className = `a11-log-line ${kind}`.trim();
    line.textContent = text;
    container.append(line);
    container.scrollTop = container.scrollHeight;
}

/** A bubble that grows as tokens arrive. */
export function streamInto(bubble: HTMLElement, container: HTMLElement): (text: string) => void {
    return (text: string) => {
        bubble.textContent = `${bubble.textContent ?? ''}${text}`;
        container.scrollTop = container.scrollHeight;
    };
}

/** Show a failure where the page has said it will show them. */
export function showError(region: HTMLElement, error: unknown): void {
    region.textContent = error instanceof Error ? error.message : String(error);
}

/** Report that a documentation example reached its successful result. */
export function reportExampleSuccess(example: string): void {
    window.dispatchEvent(
        new CustomEvent('a11:example-succeeded', {detail: {example}}),
    );
}

/** Disable a form while a turn runs, and put it back afterwards. */
export async function whileBusy<T>(form: HTMLFormElement, work: () => Promise<T>): Promise<T | undefined> {
    const controls = [...form.elements] as HTMLInputElement[];
    const wasDisabled = controls.map((control) => control.disabled);
    for (const control of controls) control.disabled = true;
    try {
        return await work();
    } finally {
        controls.forEach((control, index) => {
            control.disabled = wasDisabled[index] ?? false;
        });
    }
}

export type {AsyncNode, Interaction};
