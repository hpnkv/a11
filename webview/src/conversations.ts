/**
 * Chat history: listing the stored conversations and reopening one.
 *
 * The backend keeps every conversation as the `Interaction`s it is made of, so
 * reopening one is not replaying a transcript — it is getting the provider's own
 * objects back. That is what lets a rehydrated conversation be threaded into the
 * next turn exactly like a live one, tool calls and all.
 *
 * A conversation's id is the id of its first interaction, which this side mints.
 * So the webview never needs a server-assigned handle: `history[0].id` names the
 * conversation the moment it starts.
 */

import {
  Action,
  ActionPortSchema,
  ActionSchema,
  INTERACTION_TAG,
  StatusCode,
  fromChunk,
  isOk,
  parseInteraction,
  type Chunk,
  type Interaction,
  type Session,
  type Status,
  type WireStream,
} from '@curiositystack/a11';

/** How long to wait on a history read before giving up on it. */
const TIMEOUT_MS = 30_000;

const need = <T>(value: T | Status): T => {
  if (!isOk(value)) throw new Error(`${StatusCode[(value as Status).code]}: ${(value as Status).message}`);
  return value as T;
};

/** Mirrors the backend's `get_conversations` schema (`conversation_actions.py`). */
const GET_CONVERSATIONS_SCHEMA = new ActionSchema({
  name: 'get_conversations',
  description: 'List the stored conversations, most recently active first.',
  outputs: {
    conversations: new ActionPortSchema({
      name: 'conversations',
      type: 'application/json',
      required: true,
    }),
  },
});

/** Mirrors the backend's `get_conversation` schema. */
const GET_CONVERSATION_SCHEMA = new ActionSchema({
  name: 'get_conversation',
  description: "Stream one conversation's interactions, oldest first.",
  inputs: {
    id: new ActionPortSchema({ name: 'id', type: 'text/plain', unary: true, required: true }),
  },
  outputs: {
    interactions: new ActionPortSchema({
      name: 'interactions',
      type: 'application/json',
      required: true,
    }),
  },
});

/** One row of the history list, as the backend's index records it. */
export interface ConversationSummary {
  id: string;
  title: string;
  /** Epoch millis of the conversation's first turn. */
  started_at: number;
}

/** The stored conversations, most recently active first. */
export async function fetchConversations(
  session: Session,
  stream: WireStream,
): Promise<ConversationSummary[]> {
  const call = need(Action.create(GET_CONVERSATIONS_SCHEMA, { session, stream, nodeMap: session.getNodeMap() }));
  need(await call.call());
  const output = need(await call.getOutput('conversations', false));
  const summaries: ConversationSummary[] = [];
  for (;;) {
    const next = need(await output.next({ timeoutMs: TIMEOUT_MS }));
    if (next === null) break;
    summaries.push(next as ConversationSummary);
  }
  need(await call.wait(TIMEOUT_MS));
  return summaries;
}

/**
 * One conversation's interactions, oldest first; empty if the backend has no
 * such conversation (a stale id is not an error).
 *
 * Each value is re-parsed on the way in. Decoding already brands it, but the
 * brand is what lets these go back out to the backend as `a11.sdk.Interaction`
 * rather than anonymous JSON the strict `interactions` port refuses — and these
 * are destined for exactly that port on the next turn.
 */
export async function fetchConversation(
  session: Session,
  stream: WireStream,
  id: string,
): Promise<Interaction[]> {
  const call = need(Action.create(GET_CONVERSATION_SCHEMA, { session, stream, nodeMap: session.getNodeMap() }));
  need(await call.call());
  const idInput = need(await call.getInput('id'));
  need(await idInput.finalize(id));

  const output = need(await call.getOutput('interactions', false));
  const interactions: Interaction[] = [];
  for (;;) {
    const next = need(await output.next({ timeoutMs: TIMEOUT_MS, expectedTag: INTERACTION_TAG }));
    if (next === null) break;
    interactions.push(need(parseInteraction(next)));
  }
  need(await call.wait(TIMEOUT_MS));
  return interactions;
}

/** Text of one decoded content payload, whatever shape the backend used. */
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
 * Best-effort human-readable text of an interaction.
 *
 * This reads the content *shapes* rather than the backend: the neutral
 * `{role, content: [{type: 'text', text}]}` envelope `makeTextMessageInteraction`
 * builds, and the provider message dumps Claude and Gemini put there. Tool-use
 * blocks and images contribute nothing, which is what a transcript wants.
 * `normalizeInteraction` would be the principled route but only has a gemma
 * normalizer on this side, and throws on the untagged interactions we mint.
 */
export async function interactionText(interaction: Interaction): Promise<string> {
  const parts: string[] = [];
  for (const item of interaction.content ?? []) {
    const decoded = await fromChunk(item as Chunk);
    parts.push(isOk(decoded) ? payloadText(decoded) : '');
  }
  return parts.join('');
}

/** One tool call an interaction made: what was called, and under which id. */
export interface ToolCall {
  name: string;
  id: string;
}

/** The tools this interaction called, in order. */
export function toolCalls(interaction: Interaction): ToolCall[] {
  return (interaction.action_calls ?? [])
    .map((call) => call as { name?: unknown; id?: unknown })
    .filter((call): call is ToolCall => typeof call.name === 'string' && typeof call.id === 'string')
    .map((call) => ({ name: call.name, id: call.id }));
}

/**
 * Where a backend files a turn's tool run logs: the metadata of the interaction
 * carrying that turn's tool results (`a11.sdk.llm.TOOL_LOGS_METADATA_KEY`).
 */
const TOOL_LOGS_KEY = 'tool_logs';

/**
 * The run logs recorded with this interaction, keyed by tool-call id.
 *
 * These are each tool's own narration of what it did — the plugin's for an IDE
 * tool, the gateway's for one of its own. The model is never shown them (the
 * tool runner keeps that port out of the tool result) but they are kept with the
 * conversation, so a reopened one reads like the live one did.
 */
export function toolLogs(interaction: Interaction): Record<string, string> {
  const raw = interaction.backend_specific_metadata?.[TOOL_LOGS_KEY];
  if (raw === undefined) return {};
  // `dict[str, bytes]` on the Python side, so this arrives as a Uint8Array; a
  // string is accepted too, since the schema allows a caller to set one by hand.
  const text = typeof raw === 'string' ? raw : new TextDecoder().decode(raw);
  try {
    const parsed: unknown = JSON.parse(text);
    if (!parsed || typeof parsed !== 'object') return {};
    return Object.fromEntries(
      Object.entries(parsed as Record<string, unknown>).filter(
        (entry): entry is [string, string] => typeof entry[1] === 'string',
      ),
    );
  } catch {
    // A malformed log is not worth failing a whole conversation to render.
    return {};
  }
}

/**
 * Whether this interaction only carries tool results back to the model.
 *
 * Backends record a tool round trip as an assistant interaction that made the
 * calls, followed by a user-role one carrying their outputs. The second is
 * bookkeeping, not something the user said, and the tool boxes under the
 * assistant turn already stand for it.
 */
export function isToolResultCarrier(interaction: Interaction): boolean {
  return Object.keys(interaction.action_outputs ?? {}).length > 0;
}
