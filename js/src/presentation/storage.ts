/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import { base64Decode, base64Encode } from "../bytes.js";
import {
  isOk,
  isStatus,
  okStatus,
  statusFromUnknown,
  type Status,
  type StatusOr,
} from "../status.js";

export interface StringStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem(key: string): void;
}

type Encoded =
  | null
  | boolean
  | number
  | string
  | { t: "undefined" }
  | { t: "bytes"; v: string }
  | { t: "array"; v: Encoded[] }
  | { t: "object"; v: [string, Encoded][] };

function encode(value: unknown): Encoded {
  if (value === undefined) return { t: "undefined" };
  if (value instanceof Uint8Array)
    return { t: "bytes", v: base64Encode(value) };
  if (
    value === null ||
    typeof value === "string" ||
    typeof value === "boolean" ||
    typeof value === "number"
  )
    return value;
  if (Array.isArray(value)) return { t: "array", v: value.map(encode) };
  if (typeof value === "object") {
    return {
      t: "object",
      v: Object.entries(value as Record<string, unknown>).map(([key, item]) => [
        key,
        encode(item),
      ]),
    };
  }
  return String(value);
}

function decode(value: Encoded): StatusOr<unknown> {
  if (value === null || typeof value !== "object") return value;
  if (value.t === "undefined") return undefined;
  if (value.t === "bytes") return base64Decode(value.v);
  if (value.t === "array") {
    const result: unknown[] = [];
    for (const item of value.v) {
      const decoded = decode(item);
      if (isStatus(decoded) && !isOk(decoded)) return decoded;
      result.push(decoded);
    }
    return result;
  }
  const result: Record<string, unknown> = {};
  for (const [key, item] of value.v) {
    const decoded = decode(item);
    if (isStatus(decoded) && !isOk(decoded)) return decoded;
    result[key] = decoded;
  }
  return result;
}

export function encodeStoredValue(value: unknown): StatusOr<string> {
  try {
    return JSON.stringify(encode(value));
  } catch (error) {
    return statusFromUnknown(error, "Could not encode presentation state.");
  }
}

export function decodeStoredValue<T>(value: string): StatusOr<T> {
  try {
    return decode(JSON.parse(value) as Encoded) as StatusOr<T>;
  } catch (error) {
    return statusFromUnknown(error, "Could not decode presentation state.");
  }
}

/** Namespaced storage whose disabled/quota failures remain ordinary statuses. */
export class PresentationStorage {
  constructor(
    private readonly storage: StringStorage,
    private readonly prefix: string,
  ) {}

  read<T>(key: string): StatusOr<T | null> {
    try {
      const raw = this.storage.getItem(this.key(key));
      if (raw === null) return null;
      return decodeStoredValue<T>(raw);
    } catch (error) {
      return statusFromUnknown(error, "Could not read presentation state.");
    }
  }

  write(key: string, value: unknown): Status {
    const encoded = encodeStoredValue(value);
    if (!isOk(encoded)) return encoded;
    try {
      this.storage.setItem(this.key(key), encoded);
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Could not save presentation state.");
    }
  }

  remove(key: string): Status {
    try {
      this.storage.removeItem(this.key(key));
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Could not remove presentation state.");
    }
  }

  private key(key: string): string {
    return `${this.prefix}.${encodeURIComponent(key)}`;
  }
}
