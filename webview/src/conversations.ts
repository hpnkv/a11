/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Chat history: listing the stored conversations and reopening one.
 *
 * The backend stores conversations as `Interaction` values rather than rendered
 * transcripts. A reopened conversation can therefore continue with its tool
 * calls and provider data intact.
 *
 * The first interaction id identifies the conversation, so the webview does not
 * require a separate server-assigned handle.
 */

import {
  Action,
  ActionPortSchema,
  ActionSchema,
  INTERACTION_TAG,
  StatusCode,
  isOk,
  parseInteraction,
  plainText,
  toolLogs as presentationToolLogs,
  toolOutputs as presentationToolOutputs,
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

/**
 * Mirrors the backend's
 * `get_conversations` schema (`conversation_actions.py`).
 */
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
 * Each value is parsed again to ensure it carries the `a11.sdk.Interaction`
 * wire tag required by the strict `interactions` input on the next turn.
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

/**
 * Best-effort human-readable text of an interaction.
 *
 * This reads the content *shapes* rather than the backend: the neutral
 * `{role, content: [{type: 'text',
 * text}]}` envelope `makeTextMessageInteraction`
 * builds, and the provider message dumps Claude and Gemini put there. Tool-use
 * blocks and images contribute nothing, which is what a transcript wants.
 * `normalizeInteraction` would be the principled route but only has a gemma
 * normalizer on this side, and throws on the untagged interactions we mint.
 */
export async function interactionText(interaction: Interaction): Promise<string> {
  return need(await plainText(interaction));
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
/**
 * The run logs recorded with this interaction, keyed by tool-call id.
 *
 * Logs may come from an IDE tool or a gateway tool. The tool runner excludes
 * them from model-visible results, while conversation storage preserves them
 * for reopened sessions.
 */
export function toolLogs(interaction: Interaction): Record<string, string> {
  return need(presentationToolLogs(interaction));
}

/** Decoded output values for each tool call carried by one interaction. */
export async function toolOutputs(
  interaction: Interaction,
): Promise<Record<string, Record<string, unknown>>> {
  const decoded = await presentationToolOutputs(interaction);
  return isOk(decoded) ? decoded : {};
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
