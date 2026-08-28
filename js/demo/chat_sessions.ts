/**
 * "A chat that survives a reload" guide demo.
 *
 * One `interact_with_llm` call per turn, with the provider chosen by a header, and
 * the conversation held here as the provider's own `Interaction` objects. The
 * backend records those same objects as it answers, so `get_conversation` is all
 * a reloaded page needs to continue a conversation rather than start a new one —
 * there is no transcript to replay and no server-assigned session handle to keep.
 */

import {
  ActionPortSchema,
  ActionSchema,
  INTERACTION_TAG,
  parseInteraction,
  type Interaction,
} from '../src/index.js';

import {
  BackendControls,
  DEFAULT_SERVER_URL,
  addBubble,
  connect,
  interactionText,
  makeCall,
  need,
  reportExampleSuccess,
  runTurn,
  showError,
  streamInto,
  whileBusy,
  type Connection,
} from './demo_support.js';

/** Mirrors `a11/gateway/conversation_actions.py`'s `get_conversation`. */
const GET_CONVERSATION_SCHEMA = new ActionSchema({
  name: 'get_conversation',
  description: "Stream one conversation's interactions, oldest first.",
  inputs: {id: new ActionPortSchema({name: 'id', type: 'text/plain', unary: true, required: true})},
  outputs: {
    interactions: new ActionPortSchema({name: 'interactions', type: 'application/json', required: true}),
  },
});

const SYSTEM_PROMPT =
  'You are a concise assistant embedded in a documentation page. Answer in at' +
  ' most three sentences unless asked for more.';

class ChatDemo {
  private connection: Connection | null = null;
  private history: Interaction[] = [];

  private readonly backend = new BackendControls('chat');
  private readonly errors = document.querySelector<HTMLDivElement>('#chat-errors')!;
  private readonly messages = document.querySelector<HTMLDivElement>('#chat-messages')!;
  private readonly thoughts = document.querySelector<HTMLDivElement>('#chat-thoughts')!;

  /**
   * The conversation's id is the id of the interaction that opened it, which
   * this side minted. So the page names the conversation the moment it starts,
   * with nothing handed back from the backend.
   */
  private get conversationId(): string | null {
    return this.history[0]?.id ?? null;
  }

  /** The session, opened on first use and reused for every later turn. */
  private async connected(): Promise<Connection> {
    if (this.connection !== null) return this.connection;
    this.connection = await connect(this.backend.server.value.trim() || DEFAULT_SERVER_URL);
    return this.connection;
  }

  /** Drop a session that failed, so the next turn dials a fresh one. */
  private discard(): void {
    try {
      this.connection?.session.halfClose();
    } catch {
      // Already gone; there is nothing to close politely.
    }
    this.connection = null;
  }

  async send(prompt: string): Promise<void> {
    this.errors.textContent = '';
    addBubble(this.messages, prompt, 'question');
    const answer = addBubble(this.messages, '', 'answer');
    try {
      const connection = await this.connected();
      const added = await runTurn({
        connection,
        backend: this.backend.value,
        prompt,
        history: this.history,
        systemPrompt: SYSTEM_PROMPT,
        onToken: streamInto(answer, this.messages),
        onThought: (text) => streamInto(this.currentThought(), this.thoughts)(text),
      });
      // Only a turn that got this far joins the conversation, so a failed one
      // leaves the history as it was and the prompt can be sent again.
      this.history.push(...added);
      this.rememberInUrl();
      reportExampleSuccess('persistent-chat');
    } catch (error) {
      this.discard();
      answer.remove();
      showError(this.errors, error);
    }
  }

  private thought: HTMLElement | null = null;

  /** One bubble per turn's thinking, made on the first thought that arrives. */
  private currentThought(): HTMLElement {
    if (this.thought === null || this.thought.dataset.turn !== String(this.history.length)) {
      this.thought = addBubble(this.thoughts, '', 'note');
      this.thought.dataset.turn = String(this.history.length);
    }
    return this.thought;
  }

  /** Start a fresh conversation, keeping the socket and the backend choice. */
  newConversation(): void {
    this.history = [];
    this.thought = null;
    this.messages.replaceChildren();
    this.thoughts.replaceChildren();
    this.rememberInUrl();
  }

  /**
   * Reopen a stored conversation and continue in it.
   *
   * The fetched interactions *become* the history, so the next turn threads the
   * old conversation back to the model in full and lands on the same
   * conversation on the backend: its id is the first interaction's id, and that
   * interaction is replayed unchanged.
   */
  async open(id: string): Promise<void> {
    this.errors.textContent = '';
    try {
      const connection = await this.connected();
      const call = makeCall(connection, GET_CONVERSATION_SCHEMA);
      need(await call.call());
      const idInput = need(await call.getInput('id'));
      need(await idInput.finalize(id));

      const restored: Interaction[] = [];
      const node = need(await call.getOutput('interactions', false));
      for (;;) {
        const next = need(await node.next({timeoutMs: 30_000, expectedTag: INTERACTION_TAG}));
        if (next === null) break;
        // Re-parsed on the way in: the brand is what lets these go back out as
        // `a11.sdk.Interaction` on the next turn rather than as anonymous JSON.
        restored.push(need(parseInteraction(next)));
      }
      need(await call.wait(30_000));

      this.history = restored;
      this.thought = null;
      this.messages.replaceChildren();
      this.thoughts.replaceChildren();
      for (const interaction of restored) {
        const text = (await interactionText(interaction)).trim();
        if (text) addBubble(this.messages, text, interaction.role === 'model' ? 'answer' : 'question');
      }
      if (restored.length === 0) {
        addBubble(this.messages, 'The backend has no such conversation.', 'note');
      }
      this.rememberInUrl();
    } catch (error) {
      showError(this.errors, error);
    }
  }

  /** Put the conversation in the address bar, so a reload really is one. */
  private rememberInUrl(): void {
    const url = new URL(window.location.href);
    const id = this.conversationId;
    if (id) url.searchParams.set('conversation', id);
    else url.searchParams.delete('conversation');
    window.history.replaceState(null, '', url);
  }

  /** What the page does on load: reopen whatever the URL names. */
  async start(): Promise<void> {
    const wanted = new URL(window.location.href).searchParams.get('conversation');
    if (wanted) await this.open(wanted);
  }
}

const root = document.querySelector('#chat-demo');
if (root) {
  const demo = new ChatDemo();
  const form = document.querySelector<HTMLFormElement>('#chat-form')!;
  const input = document.querySelector<HTMLInputElement>('#chat-input')!;
  form.onsubmit = (event) => {
    event.preventDefault();
    const prompt = input.value.trim();
    if (!prompt) return;
    input.value = '';
    void whileBusy(form, () => demo.send(prompt));
  };
  document.querySelector<HTMLButtonElement>('#chat-new')!.onclick = () => demo.newConversation();
  void demo.start();
}
