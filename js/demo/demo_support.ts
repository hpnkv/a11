/**
 * What the four guide demos that talk to `a11.demos.web_demos_server` all need:
 * a WebSocket session, the backend toolbar (provider, model, key, base URL), one
 * chat turn against `interact_with_llm`, the `__register_tools__` handshake, and
 * a few DOM helpers.
 *
 * It is one module rather than four copies because the demos differ in what they
 * *do* with a turn — read a report, drive a canvas, draw an image — and not in
 * how a turn is run. The code here is the same sequence the IntelliJ plugin's
 * webview runs (`intellij-plugin/webview/src/a11client.ts`), cut down to what a
 * documentation page shows.
 */

import {
    Action,
    ActionPortSchema,
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
    makeTextMessageInteraction,
    type AsyncNode,
    type Chunk,
    type Interaction,
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
 * A locally run `python -m a11.demos.web_demos_server` is
 * `ws://127.0.0.1:9010/a11-demos`, which the same field accepts — as does the
 * `https://`/`http://` spelling of either, which [connect] rewrites, because a
 * service address and its WebSocket URL name the same endpoint.
 */
export const DEFAULT_SERVER_URL = 'wss://a11.services:9443/a11-demos';

// --- The backend a turn is answered by --------------------------------------

/** Everything that decides *who* answers, all of it a header on the call. */
export interface Backend {
    provider: string;
    model: string;
    apiKey: string;
    baseUrl: string;
}

/**
 * What each provider is usually called and reached at.
 *
 * The hosted ones need a key and no base URL; a keyless provider reached over a
 * base URL (Ollama) is the other way round. Nothing else here is
 * provider-specific — that is the whole point of `interact_with_llm`.
 *
 * The base URL is resolved by whoever *serves* the action, not by the page, so
 * `http://127.0.0.1:11434` means the backend's own Ollama — which is why it is
 * the default: the hosted demo backend runs one, and a turn against it needs no
 * key at all.
 */
export const BACKEND_DEFAULTS: Record<string, { model: string; baseUrl: string }> = {
    ollama: {model: 'glm-4.7-flash', baseUrl: 'http://127.0.0.1:11434'},
    claude: {model: 'claude-sonnet-4-6', baseUrl: ''},
    gemini: {model: 'gemini-3.5-flash', baseUrl: ''},
};

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

/** The backend, as the headers that carry it on a call. Empty ones are left off. */
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
 * A person writes (and a service publishes) `https://host:port/path`; the
 * transport wants `wss://`. They are the same endpoint — A11's WebSocket server
 * speaks HTTP/1.1 as well as HTTP/2, so a browser connects to it directly — and
 * accepting both is one line rather than a footgun in every demo's URL field.
 */
export function webSocketUrl(url: string): string {
    return url.trim().replace(/^http(s?):\/\//i, 'ws$1://');
}

/**
 * Open a session to the demo server over one WebSocket.
 *
 * `registry` is what the *page* serves: an empty one for a demo that only calls
 * out, and a populated one for the demo whose actions the model calls back into
 * (see [announceTools]). It is bound to the session before the stream is
 * attached, so an inbound call cannot arrive before there is a handler for it.
 */
export async function connect(url: string, registry = new ActionRegistry()): Promise<Connection> {
    const session = need(Session.create({actionRegistry: registry, noStreamTimeoutMs: null}));
    const stream = need(WebSocketWireStream.connect(webSocketUrl(url)));
    need(await session.addStream(stream, StreamMode.START));
    return {session, stream};
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

/** Read an output port and keep nothing: an undrained port stalls its producer. */
export async function drainPort(action: Action, port: string): Promise<void> {
    try {
        await readPort(action, port, () => {
        });
    } catch {
        // A port the page does not use must never fail the turn it belongs to.
    }
}

// --- Announcing the page's own actions ---------------------------------------

/** The reserved action a peer announces its own tools with, once per connection. */
export const REGISTER_TOOLS_SCHEMA = new ActionSchema({
    name: '__register_tools__',
    description: "Announce the caller's tool schemas for reverse dispatch.",
    inputs: {tools: new ActionPortSchema({name: 'tools', type: 'application/json', required: true})},
    outputs: {ok: new ActionPortSchema({name: 'ok', type: 'application/json', required: true})},
});

/** One port, as the backend's tool bridge reads it back. */
function describePort(port: ActionPortSchema, userFacing = false): Record<string, unknown> {
    const described: Record<string, unknown> = {
        name: port.name,
        type: port.type,
        description: port.description,
        required: port.required,
        unary: port.unary,
    };
    if (userFacing) described.user_facing = true;
    return described;
}

/**
 * An `ActionSchema` as a descriptor the backend can rebuild a callable proxy
 * from. This is the *port* description, not the JSON-Schema tool definition the
 * model is shown: two documents, two ports, and sending one where the other
 * belongs yields a proxy with no inputs at all.
 */
export function describeTool(schema: ActionSchema): Record<string, unknown> {
    return {
        name: schema.name,
        description: schema.description,
        inputs: [...schema.inputs.values()].map((port) => describePort(port)),
        // Nothing is flagged: narration goes through `action.log()`, whose port is
        // not in the schema. The backend finds it in the same place on every
        // action, keeps it away from the model, and files it under the call — so a
        // replayed conversation still shows what a tool did rather than only that
        // it ran.
        outputs: [...schema.outputs.values()].map((port) => describePort(port)),
    };
}

/**
 * Tell the backend which actions this page serves, so the model's calls to them
 * are dispatched back down this same socket.
 *
 * Once per connection, before the first turn: the backend registers a proxy per
 * descriptor on this connection's own registry, and a turn that has not been
 * through this handshake has nothing to call.
 */
export async function announceTools(
    connection: Connection,
    schemas: readonly ActionSchema[],
): Promise<string[]> {
    const announce = makeCall(connection, REGISTER_TOOLS_SCHEMA);
    need(await announce.call());
    const tools = need(await announce.getInput('tools'));
    for (const schema of schemas) need(await tools.put(describeTool(schema)));
    need(await tools.putNullFinal());
    need(await tools.drainAndClose());
    const ok = need(await announce.getOutput('ok', false));
    const acknowledged = need(await ok.next({timeoutMs: 30_000})) as { registered?: string[] } | null;
    need(await announce.wait(30_000));
    return acknowledged?.registered ?? [];
}

// --- One chat turn -----------------------------------------------------------

/** What a turn reports while it runs. */
export interface TurnCallbacks {
    onToken?: (text: string) => void;
    onThought?: (text: string) => void;
}

/** What a turn is: a conversation so far, a new question, and who answers it. */
export interface TurnRequest extends TurnCallbacks {
    connection: Connection;
    backend: Backend;
    prompt: string;
    /** Every interaction of the conversation so far, oldest first. */
    history: readonly Interaction[];
    /** Rides on the first interaction of a conversation; later ones ignore it. */
    systemPrompt?: string;
    /** Actions the page serves that this turn may use, already announced. */
    tools?: readonly ActionSchema[];
    /** Per-port JSON Schemas, for ports whose MIME type does not say enough. */
    portSchemas?: Readonly<Record<string, PortValueSchemas>>;
}

/**
 * Run one turn and return the interactions it added.
 *
 * The caller appends them to its own history: the conversation lives in the page
 * as the provider's own interaction objects, so the next turn puts the whole of
 * it back in front of the model, and the backend records the same objects as it
 * goes (which is what makes a reload continue rather than start over).
 */
export async function runTurn(request: TurnRequest): Promise<Interaction[]> {
    const {connection, backend, prompt, history} = request;
    const registry = connection.session.getActionRegistry();

    const call = makeCall(connection, INTERACT_WITH_LLM_SCHEMA);
    for (const [header, value] of LlmHeadersFor(backend)) need(call.setHeader(header, value));
    const toolNames = (request.tools ?? []).map((schema) => schema.name);
    if (toolNames.length > 0) {
        // The allow-list is the request: a tool the model is not offered here cannot
        // be called, and is not even described to it.
        need(call.setHeader(LlmHeaders.ALLOWED_LLM_ACTIONS, toolNames.join(',')));
    }
    need(await call.call());

    const question = need(
        await makeTextMessageInteraction(prompt, history.length === 0 ? (request.systemPrompt ?? '') : ''),
    );

    const interactions = need(await call.getInput('interactions'));
    for (const interaction of history) need(await interactions.put(interaction));
    need(await interactions.putFinal(question));
    need(await interactions.drainAndClose());

    // Closed empty, so the backend applies its own default request config.
    const config = need(await call.getInput('config'));
    need(await config.putNullFinal());
    need(await config.drainAndClose());

    const definitions = need(getToolDefinitions(registry, toolNames, request.portSchemas ?? {}));
    const tools = need(await call.getInput('tools'));
    for (const definition of definitions) need(await tools.put(definition));
    need(await tools.putNullFinal());
    need(await tools.drainAndClose());

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
    });
    const events = drainPort(call, 'event_stream');

    await readPort(call, 'text_output', (value) => request.onToken?.(String(value)));
    await thoughts;
    await interactionsOut;
    await events;

    // The turn's terminal status is the only place a failure *after* the first
    // token shows up, so dropping it would turn "the turn died" into "the model
    // said nothing".
    need(await call.wait(READ_TIMEOUT_MS));

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
 * Best-effort readable text of an interaction, for drawing a stored conversation.
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
