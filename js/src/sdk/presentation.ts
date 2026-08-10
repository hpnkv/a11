/**
 * How any client turns Interactions into renderable units.
 *
 * The TypeScript half of the contract in `a11/sdk/presentation.py`. Every A11
 * client that shows a conversation has had to answer the same questions -- what
 * text did this interaction contribute, which tool ran, where is its log, which
 * interaction is only carrying a tool result and should not be drawn -- and each
 * answered them separately. This is the one answer on this side.
 *
 * The unit is a {@link PresentationBlock}: a flat, ordered, renderer-agnostic
 * piece of a turn, so a client's job shrinks to a switch over
 * {@link BlockKind}. `testdata/presentation_events.json` is the golden fixture
 * both languages are held to; if this file and its Python counterpart disagree,
 * one of the two test suites says so.
 *
 * **One reducer, two feeders.** A live turn arrives as port events (text deltas,
 * thought deltas, whole interactions); a reopened conversation arrives as stored
 * Interactions. Both go through {@link PresentationReducer}, so replayed history
 * is drawn by exactly the code that draws a live turn.
 *
 * **Ordering.** The live feeder sees true interleaving because it is watching it
 * happen. A stored Interaction has no timeline, so replay is a stable
 * approximation: text, then tool runs in `action_calls` order, then usage. Live
 * order is authoritative; replay is deterministic but not necessarily what the
 * user originally watched.
 */

import type { Chunk } from '../data.js';
import { fromChunk } from '../serialization.js';
import { isOk } from '../status.js';
import type { Status } from '../status.js';
import type { Interaction, UsageMetadata } from './llm.js';

/**
 * Where a turn's user-facing tool logs ride: JSON bytes of
 * `{tool call id: log}` in the `backend_specific_metadata` of the interaction
 * carrying that turn's tool results. They live there rather than on a port
 * because that is the one part of an interaction no backend turns into provider
 * content -- the log must never reach the model, but a conversation replayed
 * from storage is poorer for its absence.
 */
export const TOOL_LOGS_METADATA_KEY = 'tool_logs';

/** What a block is, and therefore how a client should draw it. */
export enum BlockKind {
  /** Assistant or user prose. */
  TEXT = 'text',
  /** Reasoning the model exposed. Clients commonly fold this away. */
  THOUGHT = 'thought',
  /** Inline image content. */
  IMAGE = 'image',
  /** A tool call; `text` is the tool's own user-facing log, not its result. */
  TOOL_RUN = 'tool_run',
  /** A tool result a client may want to show separately from its run. */
  TOOL_RESULT = 'tool_result',
  /** A failure, carrying `status`. */
  ERROR = 'error',
  /** Token accounting for a turn. */
  USAGE = 'usage',
}

/** One renderable piece of a turn. */
export interface PresentationBlock {
  kind: BlockKind;
  /** Tool call id for TOOL_RUN/TOOL_RESULT; the two are matched on it. */
  id: string;
  /** The body. For a tool run this is its user-facing log. */
  text: string;
  toolName: string;
  status?: Status;
  mimeType: string;
  usage?: UsageMetadata;
  /** Still being appended to; only ever true on the live path. */
  partial: boolean;
  interactionId: string;
  role: string;
}

/** The blocks one conversational turn contributes. */
export interface PresentationTurn {
  role: string;
  interactionIds: string[];
  blocks: PresentationBlock[];
}

/** What a renderer implements to be driven incrementally. */
export interface PresentationSink {
  onBlockOpened?(block: PresentationBlock): void;
  onBlockAppended?(block: PresentationBlock, delta: string): void;
  onBlockClosed?(block: PresentationBlock): void;
}

function emptyBlock(kind: BlockKind, role: string): PresentationBlock {
  return {
    kind,
    id: '',
    text: '',
    toolName: '',
    mimeType: '',
    partial: false,
    interactionId: '',
    role,
  };
}

/**
 * Best-effort text of one decoded content chunk, read by shape.
 *
 * Every backend wraps its provider payload in here, so this reads the shapes
 * rather than the backend: a bare string, `{text}`, or the
 * `{role, content: [{type: 'text', text}]}` envelope the clients and the
 * Claude/Gemini backends all produce.
 */
function shapeText(value: unknown): string {
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

/** The user-facing tool logs an interaction carries, keyed by call id. */
export function toolLogs(interaction: Interaction): Record<string, string> {
  const raw = interaction.backend_specific_metadata?.[TOOL_LOGS_METADATA_KEY];
  if (!raw) return {};
  let text: string;
  if (typeof raw === 'string') {
    text = raw;
  } else {
    try {
      text = new TextDecoder().decode(raw as Uint8Array);
    } catch {
      return {};
    }
  }
  try {
    const parsed = JSON.parse(text);
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) return {};
    const logs: Record<string, string> = {};
    for (const [key, value] of Object.entries(parsed)) logs[key] = String(value);
    return logs;
  } catch {
    // A broken log is not a reason to fail drawing a conversation.
    return {};
  }
}

/** Best-effort human-readable text of an interaction's content. */
export async function plainText(interaction: Interaction): Promise<string> {
  const parts: string[] = [];
  for (const item of interaction.content ?? []) {
    const decoded = await fromChunk(item as Chunk);
    parts.push(isOk(decoded) ? shapeText(decoded) : '');
  }
  return parts.join('');
}

/**
 * Whether this interaction exists only to carry tool results.
 *
 * A tool round trip is two interactions: the assistant's call, then a user-role
 * interaction holding the outputs. The second is bookkeeping the model needs and
 * a reader does not, so clients skip drawing it and fold its logs into the call
 * it answers.
 */
export async function isToolResultCarrier(interaction: Interaction): Promise<boolean> {
  const outputs = interaction.action_outputs ?? {};
  if (Object.keys(outputs).length === 0) return false;
  return (await plainText(interaction)).length === 0;
}

/** The blocks a single interaction contributes. */
export async function presentInteraction(
  interaction: Interaction,
  logs: Record<string, string> = {},
): Promise<PresentationTurn> {
  const role = String(interaction.role ?? 'model');
  const blocks: PresentationBlock[] = [];
  const make = (kind: BlockKind): PresentationBlock => ({
    ...emptyBlock(kind, role),
    interactionId: interaction.id ?? '',
  });

  const text = await plainText(interaction);
  if (text) blocks.push({ ...make(BlockKind.TEXT), text });

  // Tool calls come from `action_calls` rather than from content: it is the
  // backend-independent record of what ran, and it carries the call ids the
  // logs are keyed by.
  for (const call of interaction.action_calls ?? []) {
    const id = (call as { id?: string }).id ?? '';
    blocks.push({
      ...make(BlockKind.TOOL_RUN),
      id,
      toolName: (call as { name?: string }).name ?? '',
      text: logs[id] ?? '',
    });
  }

  if (interaction.usage_metadata) {
    blocks.push({ ...make(BlockKind.USAGE), usage: interaction.usage_metadata });
  }

  // isOk, not a comparison against 'OK': a healthy status is not spelled that
  // way, and treating it as an error gave every interaction an ERROR block.
  const status = interaction.status as Status | undefined;
  if (status && !isOk(status)) {
    blocks.push({ ...make(BlockKind.ERROR), status });
  }

  return { role, interactionIds: [interaction.id ?? ''], blocks };
}

/**
 * The turns a stored conversation should be drawn as.
 *
 * Collects every interaction's tool logs first, so a call can be shown with the
 * log that arrived in the *following* interaction, then skips the interactions
 * that exist only to carry results. System interactions are not drawn.
 */
export async function presentConversation(
  interactions: readonly Interaction[],
): Promise<PresentationTurn[]> {
  const logs: Record<string, string> = {};
  for (const interaction of interactions) Object.assign(logs, toolLogs(interaction));

  const turns: PresentationTurn[] = [];
  for (const interaction of interactions) {
    if (String(interaction.role) === 'system') continue;
    if (await isToolResultCarrier(interaction)) continue;
    const turn = await presentInteraction(interaction, logs);
    if (turn.blocks.length > 0) turns.push(turn);
  }
  return turns;
}

/**
 * Accumulates one turn's blocks, fed live or from storage.
 *
 * The live feeder calls {@link onText}/{@link onThought} as deltas arrive and
 * {@link onInteraction} as whole interactions land on `new_interactions`; a
 * replay feeder calls only {@link onInteraction}.
 */
export class PresentationReducer {
  private readonly collected: PresentationBlock[] = [];
  private open: PresentationBlock | null = null;
  private readonly seenCalls = new Set<string>();
  private readonly logs: Record<string, string> = {};
  /**
   * Whether prose has arrived as deltas. Only then is the text inside a later
   * interaction a duplicate; text from a *different* interaction is not, which
   * is what replaying a whole conversation depends on.
   */
  private streamedText = false;

  constructor(
    private readonly sink: PresentationSink = {},
    private readonly role: string = 'model',
  ) {}

  /** The turn's blocks so far, in order. */
  get blocks(): PresentationBlock[] {
    return [...this.collected];
  }

  /** Append assistant prose. */
  onText(delta: string): void {
    if (delta) this.streamedText = true;
    this.append(BlockKind.TEXT, delta);
  }

  /** Append exposed reasoning. */
  onThought(delta: string): void {
    this.append(BlockKind.THOUGHT, delta);
  }

  /**
   * Fold in a whole interaction, live or replayed.
   *
   * Text already streamed as deltas is not added again: on the live path the
   * same prose arrives twice, once on `text_output` and once inside the
   * interaction that lands on `new_interactions`.
   */
  async onInteraction(interaction: Interaction): Promise<void> {
    Object.assign(this.logs, toolLogs(interaction));
    // A late-arriving log belongs to the run block already drawn for it.
    for (const block of this.collected) {
      if (block.kind === BlockKind.TOOL_RUN && !block.text) {
        block.text = this.logs[block.id] ?? '';
      }
    }
    if (await isToolResultCarrier(interaction)) return;

    const turn = await presentInteraction(interaction, this.logs);
    for (const block of turn.blocks) {
      if (block.kind === BlockKind.TEXT && this.streamedText) continue;
      if (block.kind === BlockKind.TOOL_RUN) {
        if (this.seenCalls.has(block.id)) continue;
        this.seenCalls.add(block.id);
      }
      this.closeOpen();
      this.collected.push(block);
      this.sink.onBlockOpened?.(block);
      this.sink.onBlockClosed?.(block);
    }
  }

  /** Record a failure as the turn's last block. */
  onError(status: Status): void {
    this.closeOpen();
    const block = { ...emptyBlock(BlockKind.ERROR, this.role), status };
    this.collected.push(block);
    this.sink.onBlockOpened?.(block);
    this.sink.onBlockClosed?.(block);
  }

  /** Mark the turn complete, closing anything still streaming. */
  endTurn(): void {
    this.closeOpen();
  }

  private append(kind: BlockKind, delta: string): void {
    if (!delta) return;
    if (this.open === null || this.open.kind !== kind) {
      this.closeOpen();
      this.open = { ...emptyBlock(kind, this.role), partial: true };
      this.collected.push(this.open);
      this.sink.onBlockOpened?.(this.open);
    }
    this.open.text += delta;
    this.sink.onBlockAppended?.(this.open, delta);
  }

  private closeOpen(): void {
    if (this.open === null) return;
    const closing = this.open;
    this.open = null;
    closing.partial = false;
    this.sink.onBlockClosed?.(closing);
  }
}
