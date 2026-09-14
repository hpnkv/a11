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

import type { Chunk } from "../data.js";
import { base64Decode } from "../bytes.js";
import { fromChunk } from "../serialization.js";
import {
  isOk,
  isStatus,
  okStatus,
  statusFromUnknown,
  type Status,
  type StatusOr,
} from "../status.js";
import {
  NormalizedContentType,
  normalizeInteraction,
  type Interaction,
  type UsageMetadata,
} from "./llm.js";

/**
 * Where a turn's user-facing tool logs ride: JSON bytes of
 * `{tool call id: log}` in the `backend_specific_metadata` of the interaction
 * carrying that turn's tool results. They live there rather than on a port
 * because that is the one part of an interaction no backend turns into provider
 * content -- the log must never reach the model, but a conversation replayed
 * from storage is poorer for its absence.
 */
export const TOOL_LOGS_METADATA_KEY = "tool_logs";
export const TOOL_STATUSES_METADATA_KEY = "tool_statuses";

/** What a block is, and therefore how a client should draw it. */
export enum BlockKind {
  /** Assistant or user prose. */
  TEXT = "text",
  /** Reasoning the model exposed. Clients commonly fold this away. */
  THOUGHT = "thought",
  /** Inline image content. */
  IMAGE = "image",
  /** A tool call; `text` is the tool's own user-facing log, not its result. */
  TOOL_RUN = "tool_run",
  /** A tool result a client may want to show separately from its run. */
  TOOL_RESULT = "tool_result",
  /** A failure, carrying `status`. */
  ERROR = "error",
  /** Token accounting for a turn. */
  USAGE = "usage",
}

/** One renderable piece of a turn. */
export interface PresentationBlock {
  /** Renderer-facing block category. */
  kind: BlockKind;
  /** Tool call id for TOOL_RUN/TOOL_RESULT; the two are matched on it. */
  id: string;
  /** The body. For a tool run this is its user-facing log. */
  text: string;
  /** Registered tool name for a tool block. */
  toolName: string;
  /** Decoded arguments supplied by the model, keyed by input port. */
  toolArguments?: Record<string, unknown>;
  /** Decoded values produced by the tool, keyed by output port. */
  toolOutputs?: Record<string, unknown>;
  /** The decisive input fragments have closed, so special cards may render. */
  toolArgumentsComplete?: boolean;
  /** Failure represented by an error or failed tool block. */
  status?: Status;
  /** Media type for image and binary content. */
  mimeType: string;
  /** Owned image or binary bytes. */
  data?: Uint8Array;
  /** Provider-independent token accounting. */
  usage?: UsageMetadata;
  /** Still being appended to; only ever true on the live path. */
  partial: boolean;
  /** Interaction that produced this block. */
  interactionId: string;
  /** Conversation role that produced this block. */
  role: string;
}

/** The blocks one conversational turn contributes. */
export interface PresentationTurn {
  /** Conversation role for this turn. */
  role: string;
  /** Interactions folded into this turn. */
  interactionIds: string[];
  /** Ordered renderer-independent content. */
  blocks: PresentationBlock[];
}

/** What a renderer implements to be driven incrementally. */
export interface PresentationSink {
  /** Observe a newly opened live block. */
  onBlockOpened?(block: PresentationBlock): void;
  /** Append one text delta to an open live block. */
  onBlockAppended?(block: PresentationBlock, delta: string): void;
  /** Observe a block becoming complete. */
  onBlockClosed?(block: PresentationBlock): void;
}

function emptyBlock(kind: BlockKind, role: string): PresentationBlock {
  return {
    kind,
    id: "",
    text: "",
    toolName: "",
    mimeType: "",
    partial: false,
    interactionId: "",
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
  if (typeof value === "string") return value;
  if (!value || typeof value !== "object") return "";
  const record = value as { content?: unknown; text?: unknown };
  if (typeof record.content === "string") return record.content;
  if (Array.isArray(record.content)) {
    return record.content
      .map((block) => {
        if (!block || typeof block !== "object") return "";
        const part = block as { type?: unknown; text?: unknown };
        return part.type === "text" && typeof part.text === "string"
          ? part.text
          : "";
      })
      .join("");
  }
  return typeof record.text === "string" ? record.text : "";
}

/** The user-facing tool logs an interaction carries, keyed by call id. */
export function toolLogs(
  interaction: Interaction,
): StatusOr<Record<string, string>> {
  try {
    const raw = interaction.backend_specific_metadata?.[TOOL_LOGS_METADATA_KEY];
    if (!raw) return {};
    const text =
      typeof raw === "string"
        ? raw
        : new TextDecoder().decode(raw as Uint8Array);
    const parsed = JSON.parse(text);
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed))
      return {};
    const logs: Record<string, string> = {};
    for (const [key, value] of Object.entries(parsed))
      logs[key] = String(value);
    return logs;
  } catch (error) {
    return statusFromUnknown(error, "Could not read the tool logs.");
  }
}

/** Native failures for tool calls carried by a result interaction. */
export function toolStatuses(
  interaction: Interaction,
): StatusOr<Record<string, Status>> {
  try {
    const raw =
      interaction.backend_specific_metadata?.[TOOL_STATUSES_METADATA_KEY];
    if (!raw) return {};
    const text =
      typeof raw === "string"
        ? raw
        : new TextDecoder().decode(raw as Uint8Array);
    const parsed = JSON.parse(text);
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed))
      return {};
    return parsed as Record<string, Status>;
  } catch (error) {
    return statusFromUnknown(error, "Could not read the tool statuses.");
  }
}

async function decodeToolPorts(
  interaction: Interaction,
  direction: "action_inputs" | "action_outputs",
): Promise<StatusOr<Record<string, Record<string, unknown>>>> {
  try {
    const result: Record<string, Record<string, unknown>> = {};
    for (const [callId, fragments] of Object.entries(
      interaction[direction] ?? {},
    )) {
      const grouped: Record<string, unknown[]> = {};
      for (const fragment of fragments) {
        if (!fragment) continue;
        const chunk = fragment.getChunk();
        if (!isOk(chunk)) {
          return {
            ...chunk,
            message: `Could not read tool ${direction === "action_inputs" ? "input" : "output"} '${fragment.id}': ${chunk.message}`,
          };
        }
        const value = await fromChunk(chunk as Chunk);
        if (isStatus(value) && !isOk(value)) {
          return {
            ...value,
            message: `Could not decode tool ${direction === "action_inputs" ? "input" : "output"} '${fragment.id}': ${value.message}`,
          };
        }
        (grouped[fragment.id] ??= []).push(value);
      }
      if (Object.keys(grouped).length === 0) continue;
      result[callId] = Object.fromEntries(
        Object.entries(grouped).map(([name, values]) => [
          name,
          values.length === 1 ? values[0] : values,
        ]),
      );
    }
    return result;
  } catch (error) {
    return statusFromUnknown(
      error,
      `Could not decode tool ${direction === "action_inputs" ? "inputs" : "outputs"}.`,
    );
  }
}

/** Decoded input values for every tool call carried by an interaction. */
export function toolInputs(
  interaction: Interaction,
): Promise<StatusOr<Record<string, Record<string, unknown>>>> {
  return decodeToolPorts(interaction, "action_inputs");
}

/** Decoded output values for every tool call carried by an interaction. */
export function toolOutputs(
  interaction: Interaction,
): Promise<StatusOr<Record<string, Record<string, unknown>>>> {
  return decodeToolPorts(interaction, "action_outputs");
}

/** Tool calls whose decisive Studio input has arrived and stopped streaming. */
export function completeToolInputIds(
  interaction: Interaction,
): StatusOr<string[]> {
  try {
    const complete: string[] = [];
    for (const call of interaction.action_calls ?? []) {
      const fragments = interaction.action_inputs?.[call.id] ?? [];
      if (fragments.length === 0) continue;
      const latest = new Map<string, (typeof fragments)[number]>();
      for (const fragment of fragments) {
        const previous = latest.get(fragment.id);
        if (!previous || (fragment.seq ?? 0) >= (previous.seq ?? 0)) {
          latest.set(fragment.id, fragment);
        }
      }
      const requiredInput =
        call.name === "report_completion"
          ? "summary"
          : call.name === "request_user_input"
            ? "question"
            : "";
      if (
        requiredInput &&
        latest.has(requiredInput) &&
        [...latest.values()].every((fragment) => !fragment.continued)
      ) {
        complete.push(call.id);
      }
    }
    return complete;
  } catch (error) {
    return statusFromUnknown(error, "Could not inspect the tool inputs.");
  }
}

/** Best-effort human-readable text of an interaction's content. */
export async function plainText(
  interaction: Interaction,
): Promise<StatusOr<string>> {
  try {
    const parts: string[] = [];
    for (const item of interaction.content ?? []) {
      const decoded = await fromChunk(item as Chunk);
      if (isStatus(decoded) && !isOk(decoded)) {
        return {
          ...decoded,
          message: `Could not read conversation text: ${decoded.message}`,
        };
      }
      parts.push(shapeText(decoded));
    }
    return parts.join("");
  } catch (error) {
    return statusFromUnknown(error, "Could not read conversation text.");
  }
}

/**
 * Whether this interaction exists only to carry tool results.
 *
 * A tool round trip is two interactions: the assistant's call, then a user-role
 * interaction holding the outputs. The second is bookkeeping the model needs and
 * a reader does not, so clients skip drawing it and fold its logs into the call
 * it answers.
 */
export async function isToolResultCarrier(
  interaction: Interaction,
): Promise<StatusOr<boolean>> {
  try {
    const outputs = interaction.action_outputs ?? {};
    if (Object.keys(outputs).length === 0) return false;
    const text = await plainText(interaction);
    if (!isOk(text)) return text;
    return text.length === 0;
  } catch (error) {
    return statusFromUnknown(error, "Could not inspect the tool result.");
  }
}

/** The blocks a single interaction contributes. */
export async function presentInteraction(
  interaction: Interaction,
  logs: Record<string, string> = {},
  statuses: Record<string, Status> = {},
  inputs?: Record<string, Record<string, unknown>>,
  outputs?: Record<string, Record<string, unknown>>,
  completeInputs: ReadonlySet<string> = new Set(),
): Promise<StatusOr<PresentationTurn>> {
  try {
    const role = String(interaction.role ?? "model");
    const blocks: PresentationBlock[] = [];
    const make = (kind: BlockKind): PresentationBlock => ({
      ...emptyBlock(kind, role),
      interactionId: interaction.id ?? "",
    });

    const text = await plainText(interaction);
    if (!isOk(text)) return text;
    if (text) blocks.push({ ...make(BlockKind.TEXT), text });

    const normalized = normalizeInteraction(interaction);
    if (isOk(normalized)) {
      for (const part of normalized.parts) {
        if (part.type !== NormalizedContentType.IMAGE) continue;
        const data = part.data ? base64Decode(part.data) : null;
        blocks.push({
          ...make(BlockKind.IMAGE),
          mimeType: part.mime_type ?? "",
          ...(data !== null && isOk(data) ? { data } : {}),
        });
      }
    }

    // Tool calls come from `action_calls` rather than from content: it is the
    // backend-independent record of what ran, and it carries the call ids the
    // logs are keyed by.
    let decodedInputs = inputs;
    if (decodedInputs === undefined) {
      const decoded = await toolInputs(interaction);
      if (!isOk(decoded)) return decoded;
      decodedInputs = decoded;
    }
    for (const call of interaction.action_calls ?? []) {
      const id = (call as { id?: string }).id ?? "";
      blocks.push({
        ...make(BlockKind.TOOL_RUN),
        id,
        toolName: (call as { name?: string }).name ?? "",
        text: logs[id] ?? "",
        toolArguments: decodedInputs[id],
        toolOutputs: outputs?.[id],
        toolArgumentsComplete: completeInputs.has(id) || undefined,
        status: statuses[id],
      });
    }

    if (interaction.usage_metadata) {
      blocks.push({
        ...make(BlockKind.USAGE),
        usage: interaction.usage_metadata,
      });
    }

    // isOk, not a comparison against 'OK': a healthy status is not spelled that
    // way, and treating it as an error gave every interaction an ERROR block.
    const status = interaction.status as Status | undefined;
    if (status && !isOk(status)) {
      blocks.push({ ...make(BlockKind.ERROR), status });
    }

    return { role, interactionIds: [interaction.id ?? ""], blocks };
  } catch (error) {
    return statusFromUnknown(error, "Could not present the interaction.");
  }
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
): Promise<StatusOr<PresentationTurn[]>> {
  try {
    const logs: Record<string, string> = {};
    const statuses: Record<string, Status> = {};
    const inputs: Record<string, Record<string, unknown>> = {};
    const outputs: Record<string, Record<string, unknown>> = {};
    const completeInputs = new Set<string>();
    for (const interaction of interactions) {
      const interactionLogs = toolLogs(interaction);
      if (!isOk(interactionLogs)) return interactionLogs;
      Object.assign(logs, interactionLogs);
      const interactionStatuses = toolStatuses(interaction);
      if (!isOk(interactionStatuses)) return interactionStatuses;
      Object.assign(statuses, interactionStatuses);
      const decodedInputs = await toolInputs(interaction);
      if (!isOk(decodedInputs)) return decodedInputs;
      Object.assign(inputs, decodedInputs);
      const decodedOutputs = await toolOutputs(interaction);
      if (!isOk(decodedOutputs)) return decodedOutputs;
      Object.assign(outputs, decodedOutputs);
      const completed = completeToolInputIds(interaction);
      if (!isOk(completed)) return completed;
      for (const id of completed) completeInputs.add(id);
    }

    const turns: PresentationTurn[] = [];
    for (const interaction of interactions) {
      if (String(interaction.role) === "system") continue;
      const carrier = await isToolResultCarrier(interaction);
      if (!isOk(carrier)) return carrier;
      if (carrier) continue;
      const turn = await presentInteraction(
        interaction,
        logs,
        statuses,
        inputs,
        outputs,
        completeInputs,
      );
      if (!isOk(turn)) return turn;
      if (turn.blocks.length > 0) turns.push(turn);
    }
    return turns;
  } catch (error) {
    return statusFromUnknown(error, "Could not present the conversation.");
  }
}

/**
 * Accumulates one turn's blocks, fed live or from storage.
 *
 * The live feeder calls {@link onText}/{@link onThought} as deltas arrive and
 * {@link onInteraction} as whole interactions land on `new_interactions`; a
 * replay feeder calls only {@link onInteraction}.
 *
 * @example Feed text deltas into a renderer-independent turn.
 * ```ts
 * const reducer = new PresentationReducer({
 *   onBlockAppended: (_block, delta) => output.append(delta),
 * });
 * reducer.onText('The first');
 * reducer.onText(' result');
 * reducer.endTurn();
 * ```
 */
export class PresentationReducer {
  private readonly collected: PresentationBlock[] = [];
  private open: PresentationBlock | null = null;
  private readonly seenCalls = new Set<string>();
  private readonly logs: Record<string, string> = {};
  private readonly statuses: Record<string, Status> = {};
  private readonly inputs: Record<string, Record<string, unknown>> = {};
  private readonly outputs: Record<string, Record<string, unknown>> = {};
  private readonly completeInputs = new Set<string>();
  /**
   * Whether prose has arrived as deltas. Only then is the text inside a later
   * interaction a duplicate; text from a *different* interaction is not, which
   * is what replaying a whole conversation depends on.
   */
  private streamedText = false;

  constructor(
    private readonly sink: PresentationSink = {},
    private readonly role: string = "model",
  ) {}

  /** The turn's blocks so far, in order. */
  get blocks(): PresentationBlock[] {
    return [...this.collected];
  }

  /** Append assistant prose. */
  onText(delta: string): Status {
    try {
      if (delta) this.streamedText = true;
      this.append(BlockKind.TEXT, delta);
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Could not present streamed text.");
    }
  }

  /** Append exposed reasoning. */
  onThought(delta: string): Status {
    try {
      this.append(BlockKind.THOUGHT, delta);
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Could not present streamed thought.");
    }
  }

  /**
   * Fold in a whole interaction, live or replayed.
   *
   * Text already streamed as deltas is not added again: on the live path the
   * same prose arrives twice, once on `text_output` and once inside the
   * interaction that lands on `new_interactions`.
   */
  async onInteraction(interaction: Interaction): Promise<Status> {
    try {
      const interactionLogs = toolLogs(interaction);
      if (!isOk(interactionLogs)) return interactionLogs;
      Object.assign(this.logs, interactionLogs);
      const interactionStatuses = toolStatuses(interaction);
      if (!isOk(interactionStatuses)) return interactionStatuses;
      Object.assign(this.statuses, interactionStatuses);
      const inputs = await toolInputs(interaction);
      if (!isOk(inputs)) return inputs;
      Object.assign(this.inputs, inputs);
      const outputs = await toolOutputs(interaction);
      if (!isOk(outputs)) return outputs;
      Object.assign(this.outputs, outputs);
      const completed = completeToolInputIds(interaction);
      if (!isOk(completed)) return completed;
      for (const id of completed) {
        this.completeInputs.add(id);
      }
      // A late-arriving log belongs to the run block already drawn for it.
      for (const block of this.collected) {
        if (block.kind === BlockKind.TOOL_RUN && !block.text) {
          block.text = this.logs[block.id] ?? "";
        }
        if (block.kind === BlockKind.TOOL_RUN && this.statuses[block.id]) {
          block.status = this.statuses[block.id];
          block.partial = false;
          this.sink.onBlockClosed?.(block);
        }
        if (block.kind === BlockKind.TOOL_RUN && this.inputs[block.id]) {
          block.toolArguments = this.inputs[block.id];
        }
        if (block.kind === BlockKind.TOOL_RUN && this.outputs[block.id]) {
          block.toolOutputs = this.outputs[block.id];
        }
        if (
          block.kind === BlockKind.TOOL_RUN &&
          this.completeInputs.has(block.id)
        ) {
          block.toolArgumentsComplete = true;
        }
      }
      const carrier = await isToolResultCarrier(interaction);
      if (!isOk(carrier)) return carrier;
      if (carrier) return okStatus();

      const turn = await presentInteraction(
        interaction,
        this.logs,
        this.statuses,
        this.inputs,
        this.outputs,
        this.completeInputs,
      );
      if (!isOk(turn)) return turn;
      for (const block of turn.blocks) {
        if (block.kind === BlockKind.TEXT && this.streamedText) continue;
        if (block.kind === BlockKind.TOOL_RUN) {
          if (this.seenCalls.has(block.id)) continue;
          this.seenCalls.add(block.id);
        }
        this.closeOpen();
        if (block.kind === BlockKind.TOOL_RUN) block.partial = true;
        this.collected.push(block);
        this.sink.onBlockOpened?.(block);
        if (block.kind !== BlockKind.TOOL_RUN) this.sink.onBlockClosed?.(block);
      }
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Could not present the interaction.");
    }
  }

  /** Record a failure as the turn's last block. */
  onError(status: Status): Status {
    try {
      this.closeOpen();
      const block = { ...emptyBlock(BlockKind.ERROR, this.role), status };
      this.collected.push(block);
      this.sink.onBlockOpened?.(block);
      this.sink.onBlockClosed?.(block);
      return okStatus();
    } catch (error) {
      return statusFromUnknown(
        error,
        "Could not present the conversation error.",
      );
    }
  }

  /** Mark the turn complete, closing anything still streaming. */
  endTurn(): Status {
    try {
      this.closeOpen();
      for (const block of this.collected) {
        if (block.kind !== BlockKind.TOOL_RUN || !block.partial) continue;
        block.partial = false;
        this.sink.onBlockClosed?.(block);
      }
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Could not finish the presented turn.");
    }
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
