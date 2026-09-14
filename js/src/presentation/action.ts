/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import type { SchemaDocument, SchemaEntry } from "../schema_json.js";
import {
  isOk,
  notFoundError,
  okStatus,
  statusFromUnknown,
  type Status,
  type StatusOr,
} from "../status.js";

export interface HeaderField {
  name: string;
  description: string;
  declared: boolean;
  fallback?: string;
}

export const SUGGESTED_HEADERS: readonly string[] = Object.freeze([
  "x-a11-llm-provider",
  "x-a11-llm-model",
  "x-a11-llm-api-key",
  "x-a11-allowed-llm-actions",
  "x-a11-deadline",
]);

/** Whether a header value should be concealed and excluded from persistence. */
export function isSecretHeader(name: string): boolean {
  const folded = name.toLowerCase();
  return ["key", "token", "secret", "password", "authorization"].some((word) =>
    folded.includes(word),
  );
}

/** Studio's declared-header ordering, with tracing plumbing last. */
export function headerFieldsFor(
  entry: Pick<SchemaEntry, "headers">,
): StatusOr<HeaderField[]> {
  try {
    return (entry.headers ?? [])
      .map((header) => ({
        name: header.name,
        description: header.description ?? "",
        declared: true,
        fallback: header.default,
      }))
      .sort((left, right) => {
        const plumbing =
          Number(left.name.toLowerCase().startsWith("x-otel-")) -
          Number(right.name.toLowerCase().startsWith("x-otel-"));
        return plumbing !== 0 ? plumbing : left.name.localeCompare(right.name);
      });
  } catch (error) {
    return statusFromUnknown(error, "Could not prepare the action headers.");
  }
}

/** Action defaults first, then values explicitly remembered by the product. */
export function seedHeaderValues(
  fields: readonly HeaderField[],
  remembered: Readonly<Record<string, string>>,
): StatusOr<Record<string, string>> {
  try {
    const seeded: Record<string, string> = {};
    for (const field of fields) {
      if (field.fallback) seeded[field.name] = field.fallback;
    }
    return { ...seeded, ...remembered };
  } catch (error) {
    return statusFromUnknown(error, "Could not prepare the initial headers.");
  }
}

/** A settings default matched like an HTTP header: case-insensitively. */
export function defaultHeaderValue(
  defaults: Readonly<Record<string, string>>,
  name: string,
): StatusOr<string | undefined> {
  try {
    const folded = name.trim().toLowerCase();
    return Object.entries(defaults).find(
      ([candidate]) => candidate.trim().toLowerCase() === folded,
    )?.[1];
  } catch (error) {
    return statusFromUnknown(error, "Could not resolve the header default.");
  }
}

/** Resolve only declared or explicitly added headers, using Studio precedence. */
export function resolveCallHeaders(
  fields: readonly HeaderField[],
  explicit: Readonly<Record<string, string>>,
  extra: readonly { name: string; value: string }[],
  defaults: Readonly<Record<string, string>>,
): StatusOr<Record<string, string>> {
  try {
    const sending: Record<string, string> = {};
    for (const field of fields) {
      const own = Object.prototype.hasOwnProperty.call(explicit, field.name);
      const fallback = defaultHeaderValue(defaults, field.name);
      if (!isOk(fallback)) return fallback;
      const value = own ? explicit[field.name] : (fallback ?? field.fallback);
      if (value) sending[field.name] = value;
    }
    for (const row of extra) {
      const name = row.name.trim();
      if (!name) continue;
      const fallback = defaultHeaderValue(defaults, name);
      if (!isOk(fallback)) return fallback;
      const value = row.value || fallback;
      if (value) {
        const folded = name.toLowerCase();
        const prior = Object.keys(sending).find(
          (candidate) => candidate.toLowerCase() === folded,
        );
        if (prior) delete sending[prior];
        sending[name] = value;
      }
    }
    return sending;
  } catch (error) {
    return statusFromUnknown(error, "Could not resolve the call headers.");
  }
}

export function redactHeaders(
  headers: Readonly<Record<string, string>>,
): StatusOr<Record<string, string>> {
  try {
    return Object.fromEntries(
      Object.entries(headers).map(([name, value]) => [
        name,
        isSecretHeader(name) ? "••••••••" : value,
      ]),
    );
  } catch (error) {
    return statusFromUnknown(error, "Could not redact the action headers.");
  }
}

export interface ActionCatalogueSnapshot {
  query: string;
  selected: string | null;
  actions: readonly SchemaEntry[];
  visible: readonly SchemaEntry[];
}

/** Studio's action-list search over the combined visible name and description. */
export function filterActions<
  Action extends { name: string; description?: string },
>(actions: readonly Action[], query: string): StatusOr<Action[]> {
  try {
    const needle = query.trim().toLowerCase();
    return actions.filter(
      (action) =>
        !needle ||
        `${action.name} ${action.description ?? ""}`
          .toLowerCase()
          .includes(needle),
    );
  } catch (error) {
    return statusFromUnknown(error, "Could not filter the action list.");
  }
}

/** Filter and select an action catalogue without prescribing list markup. */
export class ActionCatalogueController {
  private snapshot: ActionCatalogueSnapshot;
  private readonly listeners = new Set<() => void>();

  constructor(document: SchemaDocument, selected?: string | null) {
    const actions = document.actions.filter(
      (action) => action.runnable !== false,
    );
    const initial =
      selected && actions.some((action) => action.name === selected)
        ? selected
        : (actions[0]?.name ?? null);
    this.snapshot = { query: "", selected: initial, actions, visible: actions };
  }

  getSnapshot(): ActionCatalogueSnapshot {
    return this.snapshot;
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  setQuery(query: string): Status {
    try {
      const visible = filterActions(this.snapshot.actions, query);
      if (!isOk(visible)) return visible;
      this.snapshot = { ...this.snapshot, query, visible };
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not filter the action list.");
    }
  }

  select(name: string): Status {
    try {
      if (!this.snapshot.actions.some((action) => action.name === name)) {
        return notFoundError(`Action '${name}' is not in this catalogue.`);
      }
      this.snapshot = { ...this.snapshot, selected: name };
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not select the action.");
    }
  }

  dispose(): Status {
    this.listeners.clear();
    return okStatus();
  }

  private emit(): Status {
    for (const listener of this.listeners) {
      try {
        listener();
      } catch (error) {
        return statusFromUnknown(
          error,
          "An action-list subscriber could not receive an update.",
        );
      }
    }
    return okStatus();
  }
}

export function flowRunSchemaProblem(action: SchemaEntry): string | null {
  if (action.name !== "flow_run") return "The action is not named flow_run.";
  return contractProblem(
    action,
    {
      source: { type: "text/plain", required: true },
      inputs: { type: "application/json", required: false },
      input_streams: { type: "application/json", required: false },
      flow: { type: "text/plain", required: false },
    },
    { result: { type: "application/json", required: true } },
  );
}

export function flowCheckSchemaProblem(action: SchemaEntry): string | null {
  if (action.name !== "flow_check")
    return "The action is not named flow_check.";
  return contractProblem(
    action,
    { source: { type: "text/plain", required: true } },
    { plan: { type: "application/json", required: true } },
  );
}

function contractProblem(
  action: SchemaEntry,
  inputs: Record<string, { type: string; required: boolean }>,
  outputs: Record<string, { type: string; required: boolean }>,
): string | null {
  return (
    portsProblem("input", action.inputs ?? [], inputs) ??
    portsProblem("output", action.outputs ?? [], outputs)
  );
}

function portsProblem(
  direction: "input" | "output",
  actual: readonly { name: string; type: string; required?: boolean }[],
  expected: Record<string, { type: string; required: boolean }>,
): string | null {
  const names = Object.keys(expected);
  if (
    actual.length !== names.length ||
    new Set(actual.map((port) => port.name)).size !== actual.length
  ) {
    return `Expected ${direction} ports: ${names.join(", ")}.`;
  }
  for (const name of names) {
    const port = actual.find((candidate) => candidate.name === name);
    const wanted = expected[name]!;
    if (!port) return `Missing ${direction} port ${name}.`;
    if (port.type.split(";", 1)[0]!.trim().toLowerCase() !== wanted.type) {
      return `${direction} port ${name} must use ${wanted.type}.`;
    }
    if ((port.required === true) !== wanted.required) {
      return `${direction} port ${name} must be ${wanted.required ? "required" : "optional"}.`;
    }
  }
  return null;
}
