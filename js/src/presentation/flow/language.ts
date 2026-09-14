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

/** Add nearby action/JSON Schema documentation when native hover is generic. */
export function withContextualHoverDescription(
  reply: FlowServiceReply,
  request: FlowLanguageRequest,
  context?: Record<string, unknown>,
): FlowServiceReply {
  const hover = hoverOf(reply);
  if (!hover?.found || !context || request.offset === undefined) return reply;
  const identifier = hover.text || identifierAt(request.source, request.offset);
  if (!identifier) return reply;
  const actions = Array.isArray(context.actions)
    ? context.actions.filter(isRecord)
    : [];
  const precedingRuns = [
    ...request.source
      .slice(0, request.offset)
      .matchAll(/\brun\s+([A-Za-z_][\w-]*)\s*\(/g),
  ];
  const nearbyAction = precedingRuns.at(-1)?.[1];
  actions.sort(
    (left, right) =>
      Number(right.name === nearbyAction) - Number(left.name === nearbyAction),
  );
  const descriptions = new Set<string>();
  for (const action of actions) {
    if (action.name === identifier)
      addDescription(descriptions, action.description);
    for (const direction of ["inputs", "outputs"] as const) {
      const ports = Array.isArray(action[direction]) ? action[direction] : [];
      for (const candidate of ports) {
        if (!isRecord(candidate)) continue;
        if (candidate.name === identifier)
          addDescription(descriptions, candidate.description);
        findPropertyDescriptions(
          candidate.json_schema,
          identifier,
          descriptions,
        );
      }
    }
  }
  const description = descriptions.values().next().value;
  if (!description) return reply;
  const existing = [hover.markdown, hover.summary, hover.detail]
    .filter((part): part is string => typeof part === "string")
    .join("\n");
  if (existing.includes(description)) return reply;
  const escaped = description.replace(/([\\`*_[\]<>])/g, "\\$1");
  const heading = hover.markdown || hover.summary || `\`${identifier}\``;
  return {
    ...reply,
    result: {
      ...hover,
      detail: hover.detail || description,
      markdown: `${heading}\n\n${escaped}`,
    },
  };
}

function identifierAt(source: string, offset: number): string {
  let start = Math.min(offset, source.length);
  let end = start;
  while (start > 0 && /[\w-]/.test(source[start - 1] ?? "")) start -= 1;
  while (end < source.length && /[\w-]/.test(source[end] ?? "")) end += 1;
  return source.slice(start, end);
}

function addDescription(found: Set<string>, value: unknown): void {
  if (typeof value === "string" && value.trim()) found.add(value.trim());
}

function findPropertyDescriptions(
  schema: unknown,
  identifier: string,
  found: Set<string>,
  seen = new Set<object>(),
): void {
  if (!isRecord(schema) || seen.has(schema)) return;
  seen.add(schema);
  if (isRecord(schema.properties)) {
    const property = schema.properties[identifier];
    if (isRecord(property)) {
      addDescription(found, property.description);
      if (typeof property.title === "string" && !property.description)
        addDescription(found, property.title);
    }
  }
  for (const value of Object.values(schema)) {
    if (Array.isArray(value)) {
      for (const item of value)
        findPropertyDescriptions(item, identifier, found, seen);
    } else {
      findPropertyDescriptions(value, identifier, found, seen);
    }
  }
}
