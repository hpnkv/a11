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

export type JsonSchema = Record<string, unknown>;

export function asSchema(value: unknown): JsonSchema | undefined {
  return typeof value === "object" && value !== null && !Array.isArray(value)
    ? (value as JsonSchema)
    : undefined;
}

export function schemaText(schema: JsonSchema, key: string): string {
  return typeof schema[key] === "string" ? (schema[key] as string) : "";
}

export function requiredNames(schema: JsonSchema): string[] {
  const required = schema["required"];
  return Array.isArray(required)
    ? required.filter((name): name is string => typeof name === "string")
    : [];
}

export interface SchemaShape {
  readonly schema: JsonSchema;
  readonly nullable: boolean;
}

/** Unwrap a single T|null union while retaining constraints beside it. */
export function schemaShape(
  schema: JsonSchema | undefined,
): SchemaShape | undefined {
  if (!schema) return undefined;
  for (const keyword of ["anyOf", "oneOf"]) {
    const alternatives = schema[keyword];
    if (!Array.isArray(alternatives)) continue;
    const shapes = alternatives.map(asSchema);
    if (shapes.some((shape) => shape === undefined)) continue;
    const concrete = (shapes as JsonSchema[]).filter(
      (shape) => shape["type"] !== "null",
    );
    const nulls = (shapes as JsonSchema[]).filter(
      (shape) => shape["type"] === "null",
    );
    if (concrete.length !== 1 || nulls.length !== 1) continue;
    const unwrapped = { ...schema, ...concrete[0] };
    delete unwrapped["anyOf"];
    delete unwrapped["oneOf"];
    return { schema: unwrapped, nullable: true };
  }
  return { schema, nullable: schema["type"] === "null" };
}

export function isFormable(schema: JsonSchema | undefined): boolean {
  const shape = schemaShape(schema)?.schema;
  if (!shape) return false;
  if (Array.isArray(shape["enum"])) return true;
  const type = shape["type"];
  if (type === "object") return asSchema(shape["properties"]) !== undefined;
  if (type === "array") return asSchema(shape["items"]) !== undefined;
  return (
    type === "string" ||
    type === "number" ||
    type === "integer" ||
    type === "boolean"
  );
}

export function isLeaf(schema: JsonSchema | undefined): boolean {
  if (!isFormable(schema)) return true;
  const type = schemaShape(schema)?.schema["type"];
  return type !== "object" && type !== "array";
}

export function typeLabel(schema: JsonSchema | undefined): string {
  if (!schema) return "any";
  const resolved = schemaShape(schema);
  const shape = resolved?.schema ?? schema;
  if (Array.isArray(shape["enum"])) {
    return `one of${resolved?.nullable ? " | null" : ""}`;
  }
  const type = schemaText(shape, "type") || "any";
  const suffix = resolved?.nullable ? " | null" : "";
  if (type !== "array") return `${type}${suffix}`;
  return `array<${typeLabel(asSchema(shape["items"]))}>${suffix}`;
}

export function isProse(port: Pick<PortEntry, "type">): boolean {
  return port.type.startsWith("text/");
}

export function blankFor(schema: JsonSchema | undefined): unknown {
  if (!isFormable(schema)) return undefined;
  const resolved = schemaShape(schema);
  if (resolved?.nullable) return undefined;
  const shape = resolved?.schema as JsonSchema;
  if (Array.isArray(shape["enum"])) return undefined;
  if (shape["type"] === "object") return {};
  if (shape["type"] === "array") return [];
  if (shape["type"] === "boolean") return false;
  return undefined;
}

export function isEmpty(value: unknown): boolean {
  if (value === undefined) return true;
  if (value === null) return false;
  if (typeof value === "string") return value.trim() === "";
  if (Array.isArray(value)) return value.length === 0;
  if (typeof value === "object") {
    return Object.values(value as Record<string, unknown>).every(isEmpty);
  }
  return false;
}

/** The first Studio-compatible required-value error, or null. */
export function missing(
  schema: JsonSchema | undefined,
  value: unknown,
  path: string,
  required: boolean,
): string | null {
  const resolved = schemaShape(asSchema(schema));
  const shape = resolved?.schema;
  if (value === null) return resolved?.nullable ? null : `${path} is required`;
  const empty = isEmpty(value);
  if (empty && !required) return null;
  if (shape && shape["type"] === "object" && !Array.isArray(value)) {
    const properties = asSchema(shape["properties"]) ?? {};
    const names = requiredNames(shape);
    const held = (value ?? {}) as Record<string, unknown>;
    for (const [name, raw] of Object.entries(properties)) {
      const reason = missing(
        asSchema(raw),
        held[name],
        path ? `${path}.${name}` : name,
        names.includes(name),
      );
      if (reason) return reason;
    }
    return empty ? `${path} is required` : null;
  }
  if (empty) return `${path} is required`;
  if (shape?.["type"] === "array" && Array.isArray(value)) {
    const items = asSchema(shape["items"]);
    for (const [index, held] of value.entries()) {
      const reason = missing(items, held, `${path}[${index}]`, true);
      if (reason) return reason;
    }
  }
  return null;
}
