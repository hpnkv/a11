/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import { decode } from "@msgpack/msgpack";

import { ACTION_STATUS_MIMETYPE } from "../action_schema.js";
import { base64Encode } from "../bytes.js";
import { decodeStatus } from "../status_codec.js";
import {
  isOk,
  okStatus,
  statusFromUnknown,
  type Status,
  type StatusOr,
} from "../status.js";
import { jsonTokens, type JsonToken } from "./json.js";

export const MAX_MSGPACK_PREVIEW_BYTES = 64 * 1024;
export const MAX_BINARY_PREVIEW_BYTES = 64;
export const MAX_NESTED_BYTE_PREVIEW_BYTES = 24;
export const MAX_INLINE_JSON_CHARS = 120;

interface ValuePresentationBase {
  mimetype: string;
  copyText: string;
}

export type ValuePresentation =
  | (ValuePresentationBase & {
      kind: "text";
      text: string;
      markdown: boolean;
    })
  | (ValuePresentationBase & {
      kind: "json";
      text: string;
      tokens: JsonToken[];
      value: unknown;
    })
  | (ValuePresentationBase & {
      kind: "image" | "audio" | "video";
      bytes: Uint8Array;
      byteLength: number;
      extension: string;
    })
  | (ValuePresentationBase & {
      kind: "binary";
      bytes: Uint8Array;
      byteLength: number;
      preview: string;
      hex: string;
      base64: string;
    });

export function isMessagePack(mimetype: string): boolean {
  const mediaType = mimetype.split(";", 1)[0]!.trim().toLowerCase();
  return (
    mediaType === "application/msgpack" ||
    mediaType === "application/x-msgpack" ||
    mediaType.endsWith("+msgpack")
  );
}

export function hexEscaped(
  bytes: Uint8Array,
  limit = MAX_BINARY_PREVIEW_BYTES,
): string {
  const shown = bytes.subarray(0, limit);
  const preview = Array.from(
    shown,
    (byte) => `\\x${byte.toString(16).padStart(2, "0")}`,
  ).join("");
  return preview + (bytes.byteLength > shown.byteLength ? "…" : "");
}

export function bytesToHex(bytes: Uint8Array): string {
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join(
    "",
  );
}

export function safeJson(value: unknown): string {
  try {
    return (
      JSON.stringify(
        value,
        (_key, item) =>
          item instanceof Uint8Array
            ? `${hexEscaped(item, MAX_NESTED_BYTE_PREVIEW_BYTES)} (${formatBytes(item.byteLength)})`
            : typeof item === "bigint"
              ? `${item}n`
              : item,
        2,
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

/** Studio's compact type badge for one retained stream value. */
export function valueTypeLabel(value: unknown): StatusOr<string> {
  try {
    if (value instanceof Uint8Array) return "bytes";
    if (Array.isArray(value)) return `array ${value.length}`;
    if (value === null) return "null";
    return typeof value;
  } catch (error) {
    return statusFromUnknown(error, "Could not identify the value type.");
  }
}

/** Studio's bounded one-line fallback for a retained stream value. */
export function previewValue(value: unknown): StatusOr<string> {
  try {
    return previewValueUnchecked(value);
  } catch (error) {
    return statusFromUnknown(error, "Could not preview the value.");
  }
}

function previewValueUnchecked(value: unknown): string {
  if (value instanceof Uint8Array)
    return `${hexEscaped(value, 18)} · ${value.byteLength.toLocaleString()} bytes`;
  if (Array.isArray(value))
    return `[${value.length} items] ${value
      .slice(0, 3)
      .map(previewValueUnchecked)
      .join(", ")}`;
  if (value && typeof value === "object") {
    const keys = Object.keys(value as Record<string, unknown>);
    return keys.length
      ? `{ ${keys.slice(0, 5).join(", ")}${keys.length > 5 ? ", …" : ""} }`
      : "{}";
  }
  if (typeof value === "string")
    return value.replace(/\s+/g, " ").trim() || "empty string";
  if (value === undefined) return "undefined";
  return String(value);
}

/** A complete single-line preview only when it is short and JSON-shaped. */
export function compactJsonPreview(
  value: unknown,
  mimetype: string,
  limit = MAX_INLINE_JSON_CHARS,
): StatusOr<string | null> {
  try {
    let candidate = value;
    if (typeof value === "string" && mimetype.toLowerCase().includes("json")) {
      try {
        candidate = JSON.parse(value);
      } catch {
        return null;
      }
    } else if (
      typeof value !== "object" ||
      value === null ||
      value instanceof Uint8Array
    ) {
      return null;
    }
    const rendered = JSON.stringify(candidate);
    return rendered && rendered.length <= limit ? rendered : null;
  } catch (error) {
    return statusFromUnknown(error, "Could not build the inline JSON preview.");
  }
}

export function looksLikeMarkdown(text: string): boolean {
  return /(^|\n)\s{0,3}(?:#{1,6}\s|[-+*]\s|\d+\.\s|>\s|```|~~~)|\[[^\]]+\]\([^)]+\)|\*\*[^*]+\*\*|__[^_]+__|`[^`]+`|\n\|.+\|\s*\n\|?\s*:?-+/.test(
    text,
  );
}

function extension(mimetype: string): string {
  if (mimetype === "image/jpeg") return "jpg";
  if (mimetype === "image/svg+xml") return "svg";
  return mimetype.split("/")[1]?.split(/[;+]/, 1)[0] || "bin";
}

function formatBytes(bytes: number): string {
  return bytes < 1024 ? `${bytes} B` : `${(bytes / 1024).toFixed(1)} KiB`;
}

function jsonPresentation(
  value: unknown,
  mimetype: string,
  text = safeJson(value),
): ValuePresentation {
  return {
    kind: "json",
    mimetype,
    value,
    text,
    tokens: jsonTokens(text),
    copyText: safeJson(value),
  };
}

/** Studio's built-in MIME-first value classification and bounded previews. */
function presentBuiltinValue(value: unknown, mimetype = ""): ValuePresentation {
  if (value instanceof Uint8Array) {
    const bytes = new Uint8Array(value);
    if (mimetype.startsWith("image/")) {
      return {
        kind: "image",
        mimetype,
        bytes,
        byteLength: bytes.byteLength,
        extension: extension(mimetype),
        copyText: base64Encode(bytes),
      };
    }
    if (mimetype.startsWith("audio/") || mimetype.startsWith("video/")) {
      return {
        kind: mimetype.startsWith("audio/") ? "audio" : "video",
        mimetype,
        bytes,
        byteLength: bytes.byteLength,
        extension: extension(mimetype),
        copyText: base64Encode(bytes),
      };
    }
    if (
      isMessagePack(mimetype) &&
      bytes.byteLength <= MAX_MSGPACK_PREVIEW_BYTES
    ) {
      try {
        return jsonPresentation(decode(bytes, { useBigInt64: true }), mimetype);
      } catch {
        // Studio deliberately falls back to exact bytes for a broken preview.
      }
    }
    if (
      mimetype.split(";", 1)[0]!.trim().toLowerCase() === ACTION_STATUS_MIMETYPE
    ) {
      const decoded = decodeStatus(bytes);
      if (isOk(decoded)) return jsonPresentation(decoded.status, mimetype);
    }
    const hex = bytesToHex(bytes);
    return {
      kind: "binary",
      mimetype,
      bytes,
      byteLength: bytes.byteLength,
      preview: hexEscaped(bytes),
      hex,
      base64: base64Encode(bytes),
      copyText: hex,
    };
  }
  if (typeof value === "string") {
    if (mimetype.toLowerCase().includes("json")) {
      try {
        const parsed: unknown = JSON.parse(value);
        return jsonPresentation(parsed, mimetype);
      } catch {
        return {
          kind: "json",
          mimetype,
          value,
          text: value,
          tokens: jsonTokens(value),
          copyText: value,
        };
      }
    }
    return {
      kind: "text",
      mimetype,
      text: value,
      markdown:
        mimetype.toLowerCase().includes("markdown") || looksLikeMarkdown(value),
      copyText: value,
    };
  }
  return jsonPresentation(value, mimetype);
}

export function presentValue(
  value: unknown,
  mimetype = "",
): StatusOr<ValuePresentation> {
  try {
    return presentBuiltinValue(value, mimetype);
  } catch (error) {
    return statusFromUnknown(error, "Could not present the value.");
  }
}

export interface ValuePresenter {
  matches(value: unknown, mimetype: string): boolean;
  present(value: unknown, mimetype: string): StatusOr<ValuePresentation>;
}

/** Extensible semantic presenters; consumers still own every rendered element. */
export class ValuePresenterRegistry {
  private readonly presenters: ValuePresenter[] = [];

  prepend(presenter: ValuePresenter): Status {
    try {
      this.presenters.unshift(presenter);
      return okStatus();
    } catch (error) {
      return statusFromUnknown(
        error,
        "Could not register the value presenter.",
      );
    }
  }

  present(value: unknown, mimetype = ""): StatusOr<ValuePresentation> {
    try {
      for (const presenter of this.presenters) {
        if (!presenter.matches(value, mimetype)) continue;
        return presenter.present(value, mimetype);
      }
      return presentBuiltinValue(value, mimetype);
    } catch (error) {
      return statusFromUnknown(error, "Could not present the value.");
    }
  }
}
