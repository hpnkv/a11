/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import {
  ACTION_LOG_OUTPUT,
  isCloseStatusChunk,
  isStatusChunk,
  statusFromChunk,
} from "../../action_schema.js";
import {
  type LogLevel,
  logRecordFromChunk,
  logText,
} from "../../action_log.js";
import { fromChunk } from "../../serialization.js";
import {
  isOk,
  isStatus,
  statusFromUnknown,
  type Status,
  type StatusOr,
} from "../../status.js";
import type { WireMessage } from "../../data.js";
import { appendOutput, isTextualPort, type OutputBlock } from "../output.js";

export type FlowEntry =
  | { kind: "started"; flow: string; ports: string[] }
  | { kind: "value"; port: string; mimetype: string; value: unknown }
  | { kind: "log"; level: LogLevel; channel: string; text: string }
  | { kind: "closed"; port: string; status: Status }
  | { kind: "outcome"; status: Status };

/** Incrementally decodes the native A11 wire stream emitted by a Flow run. */
export class FlowReader {
  private readonly ports = new Map<string, string>();
  private action = "";
  flow = "";

  async accept(message: WireMessage): Promise<StatusOr<FlowEntry[]>> {
    try {
      const entries: FlowEntry[] = [];
      for (const action of message.actions ?? []) {
        this.action = action.id ?? "";
        this.flow = action.name ?? this.flow;
        const named: string[] = [];
        for (const port of action.outputs ?? []) {
          if (!port.id || !port.name) continue;
          this.ports.set(port.id, port.name);
          named.push(port.name);
        }
        entries.push({ kind: "started", flow: this.flow, ports: named });
      }
      for (const fragment of message.nodeFragments ?? []) {
        const id = fragment.id ?? "";
        const chunk = fragment.data;
        if (chunk === undefined || !("metadata" in chunk)) continue;
        if (isStatusChunk(chunk)) {
          const decodedStatus = statusFromChunk(chunk);
          // Studio's Flow event model omits empty optional fields. Besides
          // keeping snapshots compact, this preserves its established JSON and
          // test-fixture shape when the wire codec supplies `details: []`.
          const status: Status = {
            code: decodedStatus.code,
            message: decodedStatus.message,
            ...(decodedStatus.details?.length
              ? { details: decodedStatus.details }
              : {}),
            ...(decodedStatus.cause === undefined
              ? {}
              : { cause: decodedStatus.cause }),
          };
          if (id === this.action || id === "") {
            entries.push({ kind: "outcome", status });
          } else if (isCloseStatusChunk(chunk)) {
            entries.push({ kind: "closed", port: this.portOf(id), status });
          }
          continue;
        }
        if (chunk.isNull) continue;
        if (id.endsWith(`#${ACTION_LOG_OUTPUT}`)) {
          const record = logRecordFromChunk(chunk);
          if (!record.internal) {
            entries.push({
              kind: "log",
              level: record.level,
              channel: record.channel,
              text: logText(record),
            });
          }
          continue;
        }
        const value = await fromChunk(chunk);
        if (isStatus(value) && !isOk(value)) {
          return {
            ...value,
            message: `Could not decode Flow output '${this.portOf(id)}': ${value.message}`,
          };
        }
        entries.push({
          kind: "value",
          port: this.portOf(id),
          mimetype: chunk.mimetype || "application/octet-stream",
          value,
        });
      }
      return entries;
    } catch (error) {
      return statusFromUnknown(
        error,
        "Could not read a message from the Flow run.",
      );
    }
  }

  private portOf(id: string): string {
    const named = this.ports.get(id);
    if (named) return named;
    const hash = id.lastIndexOf("#");
    return hash >= 0 ? id.slice(hash + 1) : id;
  }
}

export interface RunView {
  blocks: OutputBlock[];
  notes: Note[];
}

export const EMPTY_RUN: RunView = { blocks: [], notes: [] };

/** Fold only the new batch so long token streams stay linear. */
export function foldRun(view: RunView, arrived: readonly FlowEntry[]): RunView {
  if (arrived.length === 0) return view;
  let blocks = view.blocks;
  let notes = view.notes;
  for (const entry of arrived) {
    if (entry.kind === "value") {
      blocks = appendOutput(
        blocks,
        entry.port,
        entry.value,
        isTextualPort(entry.mimetype),
        entry.mimetype,
      );
      continue;
    }
    if (notes === view.notes) notes = notes.slice();
    notes.push(entry);
  }
  return { blocks, notes };
}

export function splitRun(entries: readonly FlowEntry[]): RunView {
  return foldRun(EMPTY_RUN, entries);
}

export type Note = Exclude<FlowEntry, { kind: "value" }>;
