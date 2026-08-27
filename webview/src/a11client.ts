/**
 * The webview chat client owns the A11 WebSocket to `a11 gateway` and drives
 * chat turns. The TypeScript library connects directly to the gateway URL in
 * settings; the Kotlin plugin does not broker or start the service.
 *
 * On first use it opens the socket, builds the IDE-tool registry (so the
 * gateway can reverse-dispatch tool calls), announces the tools via
 * its own registry, and then calls `interact_with_llm` and streams the reply
 * off `text_output` / `thoughts` / `new_interactions`.
 *
 * IDE tools are announced and served here. The gateway adds matching shell
 * tools according to the allowed-tools header. The model receives one tool
 * list, and tool logs reach the client either directly or through tool-result
 * interaction metadata.
 *
 * The client keeps a flat list of A11 `Interaction`s and sends it with each
 * turn, matching `a11 chat` in `a11/cli/chat_ui.py`. The gateway records these
 * interactions for `listConversations` and `loadConversation`; a reopened
 * conversation is
 * the same list of objects, so it continues rather than starts over.
 * They are the provider's own objects, not a transcript reconstructed from
 * text, so a turn's tool calls and their results are still in front of the
 * model on the next turn. That works across the language boundary because both
 * sides name the types the same way; see `js/src/serial_tags.ts`.
 */

import {
    Action,
    ActionRegistry,
    INTERACT_WITH_LLM_SCHEMA,
    LlmHeaders,
    Session,
    StatusCode,
    StreamMode,
    WebSocketWireStream,
    getToolDefinitions,
    isOk,
    makeTextMessageInteraction,
    type Interaction,
    type Status,
    type WireStream,
} from '@curiositystack/a11';

import {applyPortSchemas, buildIdeToolRegistry, type ToolRunSink} from './ideTools.js';
import {getConfig, listActions, readFlow, type A11Config, type ActionDescriptor} from './bridge.js';
import {runFlow} from './runFlow.js';
import {
    fetchConversation,
    fetchConversations,
    toolCalls,
    toolLogs,
    type ConversationSummary,
} from './conversations.js';

const need = <T>(value: T | Status): T => {
    if (!isOk(value)) throw new Error(`${StatusCode[(value as Status).code]}: ${(value as Status).message}`);
    return value as T;
};

/**
 * Whether a failure is this side giving up on a read rather than the turn
 * failing. [need] is what turns a status into the thrown error, so the code
 * name is the message's prefix.
 */
const isTimeout = (error: unknown): boolean =>
    error instanceof Error && error.message.startsWith(`${StatusCode[StatusCode.DEADLINE_EXCEEDED]}:`);

/**
 * The model is an assistant inside an IDE. Tell it to act through the offered
 * tools rather than only describing proposed changes.
 *
 * It rides on the first interaction of the conversation — every backend reads
 * system instructions only there.
 */
const SYSTEM_PROMPT = `You are an AI assistant embedded in a JetBrains IDE, working on the user's open project.

You have tools that read and modify the IDE's live state: the active editor, the
project index, and the PSI (symbols, references, refactorings). Use them.

- When a request can be answered or carried out with a tool, call the tool. Do not
  describe the steps you would take and stop; take them.
- Prefer the IDE's own knowledge (symbols, references, refactorings) over guessing
  from file text. However, do not rely on the IDE's knowledge alone; use your own
  reasoning, and common sense to apply more general tools to fill in gaps. If IDE 
  tools do not provide you with information, use shell commands to recover it.
- Chain tools as needed: inspect first, then act, then report what changed.
- Avoid reading complete files: use search tools, pattern filters, line subset
  limiters, etc. Apply same limiters to shell commands.
- Remember that shell commands may fail, deadlock, run for a very long time, etc.
  Use shell utilities to provide your commands with strong upper bounds on output
  size and execution time.
- Only ask the user a question when a tool has reported an ambiguity you cannot
  resolve (for example, a symbol name that is declared more than once).
`;

/**
 * The system prompt for a conversation, with where the user actually is spliced
 * in: which IDE, which project, which directory on disk.
 *
 * The Kotlin host supplies current values through `getConfig`, including which
 * JetBrains IDE is running. The project path is omitted when the project has no
 * single root.
 */
function systemPrompt(config: A11Config): string {
    const where = [
        `- IDE: ${config.ide} ${config.ideVersion}`.trimEnd(),
        `- Project: ${config.projectName}`,
        ...(config.projectPath ? [`- Project path: ${config.projectPath}`] : []),
    ];
    return `${SYSTEM_PROMPT}
The user is working on:

${where.join('\n')}

Paths you report or pass to tools are interpreted relative to the project path
unless they are already absolute.
`;
}

/** Streaming callbacks for one chat turn. */
export interface ChatCallbacks {
    onToken(text: string): void;

    onThought?(text: string): void;
}

export class A11ChatSession {
    private session: Session | null = null;
    private stream: WireStream | null = null;
    private registry: ActionRegistry | null = null;
    private descriptors: ActionDescriptor[] = [];
    private toolNames: string[] = [];
    private connecting: Promise<void> | null = null;
    /**
     * Whether a tool ran during the turn in
     * flight; a retry must not repeat it.
     */
    private ranTool = false;
    /** The conversation so far, threaded back into every turn. */
    private history: Interaction[] = [];

    /**
     * Settings for the active turn. [refreshConfig] reloads them before each
     * turn so changes made after the chat view was mounted take effect.
     */
    private config!: A11Config;

    constructor(
        /**
         * Notified for each tool the model
         * runs, with that run's user-facing log.
         */
        private readonly onToolRun?: ToolRunSink,
    ) {
    }

    private async ensureConnected(): Promise<void> {
        // Settle a dial already in flight before judging the connection: it is
        // about to install a session, and one that appeared after the check
        // below would be a session nobody had re-examined against the current
        // settings.
        if (this.connecting) {
            try {
                await this.connecting;
            } finally {
                this.connecting = null;
            }
        }
        await this.refreshConfig();
        if (this.session) return;
        if (!this.connecting) this.connecting = this.connect();
        try {
            await this.connecting;
        } finally {
            this.connecting = null;
        }
    }

    /**
     * Re-read the settings, and drop the session if it was dialed at a gateway
     * URL the settings no longer name.
     *
     * This runs before every turn because provider, model, API key, and allowed
     * tools are request headers rather than connection properties. Reading
     * them only while connecting would retain stale provider and model values.
     *
     * Only a URL change requires a new socket. Conversation history is stored
     * by this client and survives that reconnection.
     */
    private async refreshConfig(): Promise<void> {
        const dialedUrl = this.session ? this.config?.url ?? null : null;
        this.config = await getConfig();
        if (dialedUrl !== null && dialedUrl !== this.config.url) this.discardSession();
    }

    private async connect(): Promise<void> {
        // The gateway asks this session what it serves over `__list_actions__`
        // and proxies the registry entries returned here.
        //
        // Run logs still reach the gateway, on the reserved log port of the
        // action it dispatched. That is deliberate: its tool runner files a log
        // under the tool call rather than forwarding it into the tool result,
        // and a log the gateway never receives is a log a reopened conversation
        // cannot show.
        const descriptors = await listActions();
        const {registry, toolNames} = buildIdeToolRegistry(descriptors, (run) => {
            this.ranTool = true;
            this.onToolRun?.(run);
        });
        const session = need(Session.create({actionRegistry: registry, noStreamTimeoutMs: null}));
        const stream = need(WebSocketWireStream.connect(this.config.url));
        // Surface a failed handshake here rather than as a later opaque error.
        need(await session.addStream(stream, StreamMode.START));
        this.registry = registry;
        this.descriptors = descriptors;
        this.toolNames = toolNames;
        this.session = session;
        this.stream = stream;
    }

    /**
     * The conversation's id: the id of the interaction that opened it.
     *
     * The backend names a conversation the same way, so this identifies it in
     * the history without anything having to be handed back from the server.
     * Null until the first turn succeeds and the conversation exists.
     */
    get conversationId(): string | null {
        return this.history[0]?.id ?? null;
    }

    /**
     * Start a fresh conversation.
     *
     * Only the history is dropped, not the session: the socket, the tool
     * registry and the announced tools are all conversation-independent.
     * Emptying the history is what makes the next turn mint a new first
     * interaction — and so a new conversation id — and re-attach the system
     * prompt to it.
     */
    startNewConversation(): void {
        this.history = [];
    }

    /**
     * The stored conversations, most recently active first.
     *
     * This may connect and start the backend process, so call it only after an
     * explicit request such as opening conversation history.
     */
    async listConversations(): Promise<ConversationSummary[]> {
        await this.ensureConnected();
        return fetchConversations(this.session!, this.stream!);
    }

    /**
     * Reopen a stored conversation and continue in it.
     *
     * The fetched interactions *become* the history, so the next turn threads
     * the old conversation back to the model in full, and lands on the same
     * conversation node on the backend: its id is the first interaction's id,
     * and that interaction is replayed unchanged.
     */
    async loadConversation(id: string): Promise<Interaction[]> {
        await this.ensureConnected();
        const interactions = await fetchConversation(this.session!, this.stream!, id);
        this.history = interactions;
        return interactions;
    }

    /**
     * Run a flow the plugin ships, on the gateway, streaming its outputs.
     *
     * The gateway compiles the source and runs the composition; the flow's
     * `call` steps come back down this same stream to the IDE tools, and its
     * `run` steps are the gateway's own. So one call reaches both ends, and the
     * values between the steps never pass through here at all.
     *
     * `outputs` names the flow's output ports to read as they fill; what comes
     * back is the same values collected, for a caller that wants them at the
     * end.
     *
     * Connecting first is not only about the socket: a flow saying
     * `call get_active_file` is compiled against the schemas the gateway got by
     * asking this session what it serves, which is where the port names on both
     * sides of a pipe come from. Until it has asked, the gateway refuses the
     * flow with `NOT_FOUND` rather than dispatching anything.
     */
    async runFlow(
        name: string,
        outputs: Record<string, (value: unknown) => void>,
        onLog?: (log: string) => void,
    ): Promise<Record<string, unknown>> {
        await this.ensureConnected();
        return runFlow(this.session!, this.stream!, {
            source: await readFlow(name),
            headers: {
                [LlmHeaders.PROVIDER]: this.config.provider,
                [LlmHeaders.MODEL]: this.config.model,
                [LlmHeaders.API_KEY]: this.config.apiKey,
                [LlmHeaders.BASE_URL]: this.config.baseUrl,
                // Every action the flow names is checked against this before
                // any of them runs -- `run` steps as much as `call` ones. So a
                // flow that asks a model needs `interact_with_llm` here, on top
                // of the IDE's own tools that a chat turn allows.
                [LlmHeaders.ALLOWED_LLM_ACTIONS]: [
                    ...this.allowedTools(),
                    INTERACT_WITH_LLM_SCHEMA.name,
                ].join(','),
            },
            outputs,
            onLog,
        });
    }

    /**
     * Run one chat turn, streaming assistant text (and thoughts) via callbacks.
     *
     * A session that has failed is never reused. The backend is a child process
     * that can die between turns — it crashes, it is restarted, the IDE was
     * asleep — and the socket to it dies with it. Holding on to that session
     * would make one failure permanent: every later message would fail on the
     * same closed socket. So a failed turn discards the session, and a turn
     * that failed before producing anything is retried once on a fresh one,
     * which is the difference between "type it again" and a chat window that
     * stays broken.
     *
     * The conversation survives that: it is held here, not in the session, so
     * the fresh one is handed the whole history back and the retried turn is
     * answered in context rather than from nothing.
     */
    async chat(prompt: string, callbacks: ChatCallbacks): Promise<void> {
        let produced = false;
        const watched: ChatCallbacks = {
            onToken: (text) => {
                produced = true;
                callbacks.onToken(text);
            },
            onThought: callbacks.onThought && ((text) => {
                produced = true;
                callbacks.onThought?.(text);
            }),
        };
        this.ranTool = false;

        try {
            await this.runTurn(prompt, watched);
            return;
        } catch (error) {
            this.discardSession();
            // Only safe to retry when the turn achieved nothing: no text, no
            // thoughts, and above all no tool run, since a tool may have
            // changed the project.
            if (produced || this.ranTool) throw error;
            // The gateway may continue processing after a read timeout.
            // Retrying could duplicate the response and tool side effects.
            if (isTimeout(error)) throw error;
        }
        await this.runTurn(prompt, watched);
    }

    /** Drop a session that has failed, so the next turn dials a fresh one. */
    private discardSession(): void {
        try {
            this.session?.halfClose();
        } catch {
            // Already gone; there is nothing to close politely.
        }
        this.session = null;
        this.stream = null;
        this.registry = null;
        this.descriptors = [];
        this.toolNames = [];
    }

    private async runTurn(prompt: string, callbacks: ChatCallbacks): Promise<void> {
        await this.ensureConnected();
        const session = this.session!;
        const stream = this.stream!;
        const registry = this.registry!;

        const call = need(
            Action.create(INTERACT_WITH_LLM_SCHEMA, {session, stream, nodeMap: session.getNodeMap()}),
        );
        need(call.setHeader(LlmHeaders.PROVIDER, this.config.provider));
        need(call.setHeader(LlmHeaders.MODEL, this.config.model));
        if (this.config.apiKey) need(call.setHeader(LlmHeaders.API_KEY, this.config.apiKey));
        if (this.config.baseUrl) need(call.setHeader(LlmHeaders.BASE_URL, this.config.baseUrl));
        // The IDE's tools *and* the patterns the user allowed the gateway to
        // add (`shell_.*` by default). A pattern here is what makes the gateway
        // offer a tool of its own: it matches its registered actions against
        // this header and adds the ones it may serve to the turn's tool list.
        need(call.setHeader(LlmHeaders.ALLOWED_LLM_ACTIONS, this.allowedTools().join(',')));
        need(await call.call());

        const userInteraction = need(
            await makeTextMessageInteraction(
                prompt,
                this.history.length === 0 ? systemPrompt(this.config) : '',
            ),
        );
        const interactions = need(await call.getInput('interactions'));
        for (const interaction of this.history) need(await interactions.put(interaction));
        need(await interactions.finalize(userInteraction));

        // Closed empty, so each backend applies its own default request config.
        const config = need(await call.getInput('config'));
        need(await config.finalize());

        const toolDefs = applyPortSchemas(need(getToolDefinitions(registry, this.toolNames)), this.descriptors);
        const toolsNode = need(await call.getInput('tools'));
        for (const def of toolDefs) need(await toolsNode.put(def));
        need(await toolsNode.finalize());

        // Read thoughts concurrently so the "thinking" affordance streams live
        // — and started before the text reader, so that a turn which thinks
        // before it speaks does not have its first thoughts arrive after its
        // first tokens. The backend writes both ports from the one provider
        // stream as it reads it, so what reaches these two loops in arrival
        // order is the order the model produced it, which is what lets the view
        // interleave thoughts, text and tool runs without knowing anything
        // about providers.
        const thoughtsTask = this.pumpText(call, 'thoughts', callbacks.onThought);
        // And the interactions, for the same reason: a gateway-side tool's run
        // log arrives on this stream, mid-turn, and waiting for the text to
        // finish would hold every shell command's box back until the model had
        // stopped talking.
        const produced: Interaction[] = [];
        const interactionsTask = this.pumpInteractions(call, produced);

        const textOut = need(await call.getOutput('text_output', false));
        for (; ;) {
            const token = need(await textOut.next({timeoutMs: 120_000}));
            if (token === null) break;
            callbacks.onToken(String(token));
        }

        await thoughtsTask;
        await interactionsTask;

        // The turn's terminal status is the *only* place a backend failure
        // after the first token shows up: the text stream has already ended
        // cleanly by then, so dropping this status turns "the turn died" into
        // "the model said nothing".
        need(await call.wait(120_000));

        // Only a turn that got this far joins the conversation, so a failed one
        // leaves the history as it was and the prompt can be retried.
        this.history.push(userInteraction, ...produced);
    }

    /**
     * The names and patterns of every tool
     * this turn may use, both ends' worth.
     */
    private allowedTools(): string[] {
        const extra = (this.config.allowedTools ?? []).filter((pattern) => !this.toolNames.includes(pattern));
        return [...this.toolNames, ...extra];
    }

    /**
     * Collect the turn's interactions as they arrive, reporting the tool runs
     * that happened on the gateway.
     *
     * A backend records a tool round trip as an assistant interaction that made
     * the calls, then a user-role one carrying their results — and it is on
     * that second one that the run logs ride, keyed by call id. So the call
     * names are remembered from the first and matched up on the second.
     *
     * Only the gateway's own tools are reported: an IDE tool ran here, and
     * [buildIdeToolRegistry] has already shown its log live. Reporting it again
     * from the record would draw the box twice.
     */
    private async pumpInteractions(call: Action, into: Interaction[]): Promise<void> {
        const names = new Map<string, string>();
        const node = need(await call.getOutput('new_interactions', false));
        for (; ;) {
            const next = need(await node.next({timeoutMs: 120_000}));
            if (next === null) break;
            const interaction = next as Interaction;
            into.push(interaction);
            for (const {id, name} of toolCalls(interaction)) names.set(id, name);
            for (const [id, log] of Object.entries(toolLogs(interaction))) {
                const name = names.get(id);
                if (!name || this.toolNames.includes(name)) continue;
                // A gateway tool may have changed the project just as an IDE
                // one may have, so this turn is no longer safe to retry either.
                this.ranTool = true;
                this.onToolRun?.({tool: name, log});
            }
        }
    }

    private async pumpText(call: Action, output: string, sink?: (text: string) => void): Promise<void> {
        if (!sink) return;
        // Thoughts are best-effort: never let a thoughts-stream hiccup fail the
        // turn.
        try {
            const node = need(await call.getOutput(output, false));
            for (; ;) {
                const next = need(await node.next({timeoutMs: 120_000}));
                if (next === null) break;
                sink(String(next));
            }
        } catch {
            // ignore — the text stream is authoritative for turn
            // success/failure.
        }
    }

    halfClose(): void {
        try {
            this.session?.halfClose();
        } catch {
            // best-effort teardown
        }
    }
}
