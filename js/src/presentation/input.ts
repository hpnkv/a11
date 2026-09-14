/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import type { PortEntry } from "../schema_json.js";
import {
  isOk,
  statusFromUnknown,
  type StatusOr,
} from "../status.js";
import {
  asSchema,
  isFormable,
  isProse,
  requiredNames,
  schemaShape,
  schemaText,
  type JsonSchema,
  typeLabel,
} from "./schema.js";

const FLOW_DURATION_UNITS: Readonly<Record<string, number>> = {
  ns: 1 / 1_000_000,
  us: 1 / 1_000,
  ms: 1,
  s: 1_000,
  m: 60_000,
  h: 3_600_000,
};

export function parseFlowDuration(value: string): number | null {
  const source = value.trim();
  if (!source) return 0;
  const part = /(\d+(?:\.\d+)?)(ns|us|ms|s|m|h)/gy;
  let offset = 0;
  let milliseconds = 0;
  while (offset < source.length) {
    part.lastIndex = offset;
    const matched = part.exec(source);
    if (!matched || matched.index !== offset) return null;
    milliseconds += Number(matched[1]) * FLOW_DURATION_UNITS[matched[2]]!;
    offset = part.lastIndex;
  }
  return Number.isFinite(milliseconds) ? milliseconds : null;
}

export interface RelativeScheduleOptions {
  startedAt: number;
  signal?: AbortSignal;
  waitUntil?: (at: number, signal?: AbortSignal) => Promise<StatusOr<boolean>>;
  now?: () => number;
  timer?: {
    set(callback: () => void, delayMs: number): unknown;
    clear(handle: unknown): void;
  };
}

export async function sendWithRelativeDelays<T, SendResult = void>(
  values: readonly T[],
  delaysMs: readonly number[],
  send: (
    value: T,
    index: number,
  ) => StatusOr<SendResult> | Promise<StatusOr<SendResult>>,
  options: RelativeScheduleOptions,
): Promise<StatusOr<boolean>> {
  try {
    const waitUntil =
      options.waitUntil ??
      ((at, signal) =>
        waitUntilInstant(
          at,
          signal,
          options.now ?? Date.now,
          options.timer ?? defaultTimer,
        ));
    const pending: Array<Promise<StatusOr<SendResult>>> = [];
    let scheduledAt = options.startedAt;
    for (let index = 0; index < values.length; index += 1) {
      scheduledAt += delaysMs[index] ?? 0;
      const waited = await waitUntil(scheduledAt, options.signal);
      if (!isOk(waited)) return waited;
      if (!waited) return false;
      pending.push(Promise.resolve(send(values[index]!, index)));
    }
    for (const sent of await Promise.all(pending)) {
      if (!isOk<SendResult>(sent)) return sent;
    }
    return true;
  } catch (error) {
    return statusFromUnknown(error, "Could not send scheduled input values.");
  }
}

const defaultTimer = {
  set: (callback: () => void, delayMs: number): unknown =>
    globalThis.setTimeout(callback, delayMs),
  clear: (handle: unknown): void =>
    globalThis.clearTimeout(handle as ReturnType<typeof setTimeout>),
};

async function waitUntilInstant(
  at: number,
  signal: AbortSignal | undefined,
  now: () => number,
  timerApi: RelativeScheduleOptions["timer"] & {},
): Promise<StatusOr<boolean>> {
  if (signal?.aborted) return false;
  const remaining = at - now();
  if (remaining <= 0) return true;
  return new Promise<StatusOr<boolean>>((resolve) => {
    let completed = false;
    const finish = (sent: boolean): void => {
      if (completed) return;
      completed = true;
      try {
        timerApi.clear(handle);
        signal?.removeEventListener("abort", aborted);
      } catch (error) {
        resolve(statusFromUnknown(error, "Could not stop an input timer."));
        return;
      }
      resolve(sent);
    };
    const aborted = (): void => finish(false);
    const handle = timerApi.set(() => finish(true), remaining);
    signal?.addEventListener("abort", aborted, { once: true });
    if (signal?.aborted) aborted();
  });
}

export type InputWidgetKind =
  | "json"
  | "text"
  | "prose"
  | "number"
  | "integer"
  | "boolean"
  | "enum"
  | "nullable"
  | "object"
  | "list"
  | "image";

export interface InputChoice {
  label: string;
  value: unknown;
}

/** A renderer-neutral input widget tree. */
export interface InputWidget {
  kind: InputWidgetKind;
  id: string;
  path: string;
  label: string;
  description: string;
  typeLabel: string;
  mimetype: string;
  required: boolean;
  nullable: boolean;
  value: unknown;
  minimum?: number;
  maximum?: number;
  choices?: InputChoice[];
  children?: InputWidget[];
}

function widgetFor(
  schema: JsonSchema | undefined,
  value: unknown,
  options: {
    path: string;
    label: string;
    description: string;
    mimetype: string;
    required: boolean;
  },
): InputWidget {
  const resolved = schemaShape(schema);
  const shape = resolved?.schema;
  const base = {
    id: options.path,
    path: options.path,
    label: options.label,
    description:
      options.description || (shape ? schemaText(shape, "description") : ""),
    typeLabel: typeLabel(schema),
    mimetype: options.mimetype,
    required: options.required,
    nullable: resolved?.nullable ?? false,
    value,
  };
  if (!isFormable(schema)) return { ...base, kind: "json" };
  if (resolved?.nullable) {
    return {
      ...base,
      kind: "nullable",
      children:
        value === undefined || value === null
          ? []
          : [
              widgetFor(shape, value, {
                ...options,
                required: true,
                description: base.description,
              }),
            ],
    };
  }
  if (Array.isArray(shape?.["enum"])) {
    return {
      ...base,
      kind: "enum",
      choices: shape["enum"].map((choice) => ({
        label: String(choice),
        value: choice,
      })),
    };
  }
  if (shape?.["type"] === "object") {
    const held =
      value !== null && typeof value === "object" && !Array.isArray(value)
        ? (value as Record<string, unknown>)
        : {};
    const names = requiredNames(shape);
    return {
      ...base,
      kind: "object",
      children: Object.entries(asSchema(shape["properties"]) ?? {}).map(
        ([name, child]) =>
          widgetFor(asSchema(child), held[name], {
            path: options.path ? `${options.path}.${name}` : name,
            label: name,
            description: "",
            mimetype: options.mimetype,
            required: names.includes(name),
          }),
      ),
    };
  }
  if (shape?.["type"] === "array") {
    const values = Array.isArray(value) ? value : [];
    return {
      ...base,
      kind: "list",
      children: values.map((item, index) =>
        widgetFor(asSchema(shape["items"]), item, {
          path: `${options.path}[${index}]`,
          label: String(index + 1),
          description: "",
          mimetype: options.mimetype,
          required: true,
        }),
      ),
    };
  }
  const type = shape?.["type"];
  if (shape && (type === "number" || type === "integer")) {
    return {
      ...base,
      kind: type,
      ...(typeof shape["minimum"] === "number"
        ? { minimum: shape["minimum"] }
        : {}),
      ...(typeof shape["maximum"] === "number"
        ? { maximum: shape["maximum"] }
        : {}),
    };
  }
  if (type === "boolean") return { ...base, kind: "boolean" };
  if (options.mimetype.startsWith("image/")) return { ...base, kind: "image" };
  return {
    ...base,
    kind: isProse({ type: options.mimetype }) ? "prose" : "text",
  };
}

/** Build Studio's semantic widget for one unary value of a port. */
export function describePortInput(
  port: PortEntry,
  value: unknown,
  path = port.name,
): StatusOr<InputWidget> {
  try {
    return widgetFor(asSchema(port.json_schema), value, {
      path,
      label: port.name,
      description: port.description ?? "",
      mimetype: port.type,
      required: port.required === true,
    });
  } catch (error) {
    return statusFromUnknown(
      error,
      `Could not describe input port '${port.name}'.`,
    );
  }
}
