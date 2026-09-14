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

import { ACTION_STATUS_MIMETYPE } from "../action_schema.js";
import type {
  ActionMessage,
  Chunk,
  NodeFragment,
  WireMessage,
} from "../data.js";
import { decodeStatus } from "../status_codec.js";
import {
  invalidArgumentError,
  isOk,
  okStatus,
  statusFromUnknown,
  type Status,
  type StatusOr,
} from "../status.js";
import {
  hexEscaped,
  MAX_NESTED_BYTE_PREVIEW_BYTES,
  presentValue,
  safeJson,
} from "./value.js";

export type TrafficDirection = "in" | "out";
export type TrafficOrigin = "session" | "flow";
export const TRAFFIC_PREVIEW_BYTES = 512;
export const MAX_TRAFFIC_IMAGE_PREVIEW_BYTES = 512 * 1024;
export const TRAFFIC_GLANCE_CHARS = 28;
export const MAX_TRAFFIC_ENTRIES = 2_000;

export interface TrafficDecoded {
  text: string;
  binary: boolean;
}
export interface TrafficField {
  name: string;
  value: TrafficDecoded;
  bytes: Uint8Array;
}
export interface TrafficAction {
  id: string;
  name: string;
  headers: TrafficField[];
  inputs: { name: string; id: string }[];
  outputs: { name: string; id: string }[];
}
export interface TrafficFragment {
  id: string;
  seq: number | null;
  continued: boolean;
  mimetype: string;
  attributes: TrafficField[];
  bytes: number;
  preview: string;
  decodedPreview?: unknown;
  previewFormat?: "MessagePack" | "A11 status";
  imagePreview?: Uint8Array;
  glance: string;
  hex: boolean;
  reference: boolean;
}
export interface TrafficEntry {
  key: number;
  direction: TrafficDirection;
  origin: TrafficOrigin;
  at: number;
  headers: TrafficField[];
  actions: TrafficAction[];
  fragments: TrafficFragment[];
}
export interface TrafficFilters {
  query: string;
  direction: "all" | TrafficDirection;
  origin: "all" | TrafficOrigin;
}
export interface TrafficSnapshot {
  entries: readonly TrafficEntry[];
  visible: readonly TrafficEntry[];
  filters: TrafficFilters;
  metrics: { sent: number; received: number; fragments: number };
}

export function decodeTrafficUtf8(bytes: Uint8Array): TrafficDecoded {
  try {
    return {
      text: new TextDecoder("utf-8", { fatal: true }).decode(bytes),
      binary: false,
    };
  } catch {
    let text = "";
    for (const byte of bytes)
      text +=
        byte >= 0x20 && byte < 0x7f
          ? String.fromCharCode(byte)
          : `\\x${byte.toString(16).padStart(2, "0")}`;
    return { text, binary: true };
  }
}

/** Snapshot one message without retaining its unbounded payloads. */
export function summariseTraffic(
  message: WireMessage,
  direction: TrafficDirection,
  at: number,
  key: number,
  origin: TrafficOrigin = "session",
): StatusOr<TrafficEntry> {
  try {
    return {
      key,
      direction,
      origin,
      at,
      headers: fieldsOf(message.headers),
      actions: message.actions.map(summariseAction),
      fragments: message.nodeFragments.map(summariseFragment),
    };
  } catch (error) {
    return statusFromUnknown(error, "Could not summarise a wire message.");
  }
}

export class TrafficController {
  private snapshot: TrafficSnapshot = {
    entries: [],
    visible: [],
    filters: { query: "", direction: "all", origin: "all" },
    metrics: { sent: 0, received: 0, fragments: 0 },
  };
  private readonly listeners = new Set<() => void>();
  private key = 0;
  constructor(private readonly limit = MAX_TRAFFIC_ENTRIES) {}
  getSnapshot(): TrafficSnapshot {
    return this.snapshot;
  }
  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }
  accept(
    message: WireMessage,
    direction: TrafficDirection,
    at: number,
    origin: TrafficOrigin = "session",
  ): Status {
    try {
      const entry = summariseTraffic(
        message,
        direction,
        at,
        ++this.key,
        origin,
      );
      if (!isOk<TrafficEntry>(entry)) return entry;
      const entries = [...this.snapshot.entries, entry].slice(
        -Math.max(1, this.limit),
      );
      this.snapshot = derive(entries, this.snapshot.filters);
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not record wire traffic.");
    }
  }
  setFilters(filters: Partial<TrafficFilters>): Status {
    try {
      const next = { ...this.snapshot.filters, ...filters };
      if (!["all", "in", "out"].includes(next.direction))
        return invalidArgumentError("Unknown wire direction filter.");
      if (!["all", "session", "flow"].includes(next.origin))
        return invalidArgumentError("Unknown wire origin filter.");
      this.snapshot = derive(this.snapshot.entries, next);
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not filter wire traffic.");
    }
  }
  clear(): Status {
    try {
      this.snapshot = derive([], this.snapshot.filters);
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not clear wire traffic.");
    }
  }
  copyValue(): StatusOr<readonly TrafficEntry[]> {
    return redactTraffic(this.snapshot.entries);
  }
  dispose(): Status {
    this.listeners.clear();
    return okStatus();
  }
  private emit(): Status {
    for (const listener of this.listeners)
      try {
        listener();
      } catch (error) {
        return statusFromUnknown(
          error,
          "A wire-traffic subscriber could not receive an update.",
        );
      }
    return okStatus();
  }
}

/** Derive Studio's filters and summary counters from immutable traffic entries. */
export function deriveTrafficSnapshot(
  entries: readonly TrafficEntry[],
  filters: TrafficFilters,
): StatusOr<TrafficSnapshot> {
  try {
    return derive(entries, filters);
  } catch (error) {
    return statusFromUnknown(error, "Could not filter wire traffic.");
  }
}

/** Remove connection secrets before traffic is copied or exported. */
export function redactTraffic(
  entries: readonly TrafficEntry[],
): StatusOr<readonly TrafficEntry[]> {
  try {
    const secret = new Set([
      "x-a11-auth",
      "x-a11-auth-ref",
      "x-a11-auth-info",
    ]);
    const fields = (values: readonly TrafficField[]) =>
      values.map((field) =>
        secret.has(field.name.toLowerCase())
          ? {
              ...field,
              value: { text: "••••••••", binary: false },
              bytes: new Uint8Array(),
            }
          : field,
      );
    return entries.map((entry) => ({
      ...entry,
      headers: fields(entry.headers),
      actions: entry.actions.map((action) => ({
        ...action,
        headers: fields(action.headers),
      })),
      fragments: entry.fragments.map((fragment) => ({
        ...fragment,
        attributes: fields(fragment.attributes),
      })),
    }));
  } catch (error) {
    return statusFromUnknown(error, "Could not redact wire traffic.");
  }
}

function derive(
  entries: readonly TrafficEntry[],
  filters: TrafficFilters,
): TrafficSnapshot {
  const needle = filters.query.trim().toLocaleLowerCase();
  const visible = entries.filter((entry) => {
    if (filters.direction !== "all" && entry.direction !== filters.direction)
      return false;
    if (filters.origin !== "all" && entry.origin !== filters.origin)
      return false;
    return !needle || safeJson(entry).toLocaleLowerCase().includes(needle);
  });
  const sent = entries.filter((entry) => entry.direction === "out").length;
  return {
    entries,
    visible,
    filters,
    metrics: {
      sent,
      received: entries.length - sent,
      fragments: entries.reduce(
        (total, entry) => total + entry.fragments.length,
        0,
      ),
    },
  };
}

function fieldsOf(map: ReadonlyMap<string, Uint8Array>): TrafficField[] {
  return [...map.entries()]
    .map(([name, value]) => ({
      name,
      value: decodeTrafficUtf8(value),
      bytes: new Uint8Array(value),
    }))
    .sort((left, right) => left.name.localeCompare(right.name));
}
function summariseAction(action: ActionMessage): TrafficAction {
  const ports = (values: readonly { name: string; id: string }[]) =>
    values.map(({ name, id }) => ({ name, id }));
  return {
    id: action.id,
    name: action.name,
    headers: fieldsOf(action.headers),
    inputs: ports(action.inputs),
    outputs: ports(action.outputs),
  };
}
function summariseFragment(fragment: NodeFragment): TrafficFragment {
  const base = {
    id: fragment.id,
    seq: fragment.seq,
    continued: fragment.continued,
  };
  if (!isInlineChunk(fragment.data))
    return {
      ...base,
      mimetype: "",
      attributes: [],
      bytes: 0,
      preview: "",
      glance: "by reference",
      hex: false,
      reference: true,
    };
  const chunk = fragment.data;
  const preview = previewOf(chunk.data, chunk.mimetype);
  const flat = preview.text.replace(/\s+/g, " ").trim();
  return {
    ...base,
    mimetype: chunk.mimetype,
    attributes: fieldsOf(chunk.metadata?.attributes ?? new Map()),
    bytes: chunk.data.byteLength,
    preview: preview.text,
    decodedPreview: preview.decoded,
    previewFormat: preview.format,
    imagePreview:
      chunk.mimetype.toLowerCase().startsWith("image/") &&
      chunk.data.byteLength <= MAX_TRAFFIC_IMAGE_PREVIEW_BYTES
        ? new Uint8Array(chunk.data)
        : undefined,
    glance:
      flat.length <= TRAFFIC_GLANCE_CHARS
        ? flat
        : `${flat.slice(0, TRAFFIC_GLANCE_CHARS - 1)}…`,
    hex: preview.hex,
    reference: false,
  };
}
function isInlineChunk(value: unknown): value is Chunk {
  return typeof value === "object" && value !== null && "metadata" in value;
}
function previewOf(
  data: Uint8Array,
  mimetype: string,
): {
  text: string;
  hex: boolean;
  decoded?: unknown;
  format?: TrafficFragment["previewFormat"];
} {
  const slice = data.subarray(0, TRAFFIC_PREVIEW_BYTES);
  const mediaType = mimetype.split(";", 1)[0]!.trim().toLowerCase();
  if (
    mediaType.startsWith("text/") ||
    mediaType === "application/json" ||
    mediaType.endsWith("+json")
  )
    return { text: new TextDecoder().decode(slice), hex: false };
  if (data.byteLength <= TRAFFIC_PREVIEW_BYTES) {
    if (mediaType === ACTION_STATUS_MIMETYPE) {
      const decoded = decodeStatus(data);
      if (isOk(decoded))
        return {
          text: compactJson(decoded.status),
          hex: false,
          decoded: decoded.status,
          format: "A11 status",
        };
    }
    const presented = presentValue(data, mimetype);
    if (isOk(presented) && presented.kind === "json")
      return {
        text: compactJson(presented.value),
        hex: false,
        decoded: presented.value,
        format: "MessagePack",
      };
  }
  const pairs = Array.from(slice, (byte) => byte.toString(16).padStart(2, "0"));
  const groups: string[] = [];
  for (let index = 0; index < pairs.length; index += 2)
    groups.push(pairs.slice(index, index + 2).join(""));
  return { text: groups.join(" "), hex: true };
}

/** Studio's single-line structured preview, including bounded byte leaves. */
function compactJson(value: unknown): string {
  try {
    return (
      JSON.stringify(value, (_key, item) =>
        item instanceof Uint8Array
          ? `${hexEscaped(item, MAX_NESTED_BYTE_PREVIEW_BYTES)} (${item.byteLength} B)`
          : typeof item === "bigint"
            ? `${item}n`
            : item,
      ) ?? String(value)
    );
  } catch {
    try {
      return String(value);
    } catch {
      return "<unprintable value>";
    }
  }
}
