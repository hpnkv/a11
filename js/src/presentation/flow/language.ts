/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import type { StatusOr } from "../../status.js";

export interface FlowLanguageRequest {
  method:
    | "check"
    | "complete"
    | "definition"
    | "describe"
    | "format"
    | "symbols"
    | "tokens";
  source: string;
  path?: string;
  offsets?: "utf16";
  offset?: number;
  context?: Record<string, unknown>;
}

export interface FlowServiceReply {
  id?: string | number;
  ok: boolean;
  result?: unknown;
  error?: { message?: string };
}

export interface FlowLanguageTransport {
  request(
    request: FlowLanguageRequest,
    signal?: AbortSignal,
  ): Promise<StatusOr<FlowServiceReply>>;
}

export interface FlowEdit {
  start: number;
  end: number;
  text: string;
}

export interface FlowFix {
  label: string;
  edits: FlowEdit[];
}

export type FlowDiagnosticSeverity =
  "error" | "warning" | "weak-warning" | "information";

export interface FlowDiagnostic {
  code: string;
  severity: FlowDiagnosticSeverity;
  message: string;
  range: {
    start: { offset: number; line: number; column: number };
    end: { offset: number; line: number; column: number };
  };
  fixes?: FlowFix[];
}

export interface FlowDiagnostics {
  format: "flow.diagnostics/v1";
  diagnostics: FlowDiagnostic[];
}

export interface FlowCompletion {
  name: string;
  kind: string;
  insert: string;
  caret?: number;
  tail?: string;
  type?: string;
  documentation?: string;
}

export interface FlowCompletions {
  format: "flow.completions/v1";
  prefix: string;
  prefix_start: number;
  proposals: FlowCompletion[];
}

export interface FlowFormat {
  format: "flow.format/v1";
  formatted: string;
  changed: boolean;
  edits: FlowEdit[];
  diagnostics?: FlowDiagnostic[];
}

export interface FlowHover {
  format: "flow.hover/v1";
  found: boolean;
  text?: string;
  kind?: string;
  summary?: string;
  detail?: string;
  markdown?: string;
  display?: {
    label: string;
    role: string;
    type?: string;
    inferred_type: boolean;
    trailing_type: boolean;
    body?: string;
  };
}

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null;

export function diagnosticsOf(reply: FlowServiceReply): FlowDiagnostics | null {
  const result = reply.ok ? reply.result : null;
  if (!isRecord(result) || result.format !== "flow.diagnostics/v1") return null;
  if (!Array.isArray(result.diagnostics)) return null;
  return result as unknown as FlowDiagnostics;
}

export function completionsOf(reply: FlowServiceReply): FlowCompletions | null {
  const result = reply.ok ? reply.result : null;
  if (!isRecord(result) || result.format !== "flow.completions/v1") return null;
  if (!Array.isArray(result.proposals)) return null;
  return result as unknown as FlowCompletions;
}

export function formatOf(reply: FlowServiceReply): FlowFormat | null {
  const result = reply.ok ? reply.result : null;
  if (!isRecord(result) || result.format !== "flow.format/v1") return null;
  if (typeof result.formatted !== "string") return null;
  return result as unknown as FlowFormat;
}

export function hoverOf(reply: FlowServiceReply): FlowHover | null {
  const result = reply.ok ? reply.result : null;
  if (!isRecord(result) || result.format !== "flow.hover/v1") return null;
  if (typeof result.found !== "boolean") return null;
  return result as unknown as FlowHover;
}

export function applyFlowEdits(
  source: string,
  edits: readonly FlowEdit[],
): string {
  return [...edits]
    .sort((left, right) => right.start - left.start)
    .reduce(
      (changed, edit) =>
        changed.slice(0, edit.start) + edit.text + changed.slice(edit.end),
      source,
    );
}
