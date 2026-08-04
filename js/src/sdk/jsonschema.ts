/**
 * JSON-Schema helpers used when translating A11 actions into model tools.
 *
 * {@link organiseAndDeduplicateJsonschema} is a direct port of
 * `a11/sdk/llm_tools/jsonschema_utils.py`: it hoists every `$defs` entry to the
 * document root and deduplicates repeated subschemas so `$ref`s (which resolve
 * against the root) stay valid once a schema is spliced into a larger tool
 * definition. {@link zodToJsonSchema} produces a JSON Schema for a zod type
 * without throwing.
 */

import { z } from 'zod';

import { noexcept, type StatusOr } from '../status.js';

/** Convert a zod schema into a JSON Schema object, reporting failure as Status. */
export function zodToJsonSchema(schema: z.ZodType): StatusOr<Record<string, unknown>> {
  return noexcept(
    () => z.toJSONSchema(schema, { io: 'input' }) as Record<string, unknown>,
    'Could not convert the zod schema to JSON Schema.',
  );
}

type JsonValue = unknown;

function isPlainObject(value: unknown): value is Record<string, JsonValue> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function isDedupableJsonschema(resolved: Record<string, JsonValue>): boolean {
  if ('enum' in resolved) return true;
  return resolved.type === 'object' && 'properties' in resolved;
}

/**
 * Hoist every `$defs` entry to the schema root and deduplicate by content.
 *
 * Walks the tree once, resolves each `$ref` against the nearest enclosing
 * `$defs` scope, and re-homes the result under a single root-level `$defs`,
 * replacing every duplicate occurrence with a `$ref` to one canonical copy.
 */
export function organiseAndDeduplicateJsonschema(
  schema: Record<string, JsonValue>,
): Record<string, JsonValue> {
  const rootDefs: Record<string, JsonValue> = {};
  const keyToName = new Map<string, string>();
  const inProgress = new Map<object, string>();

  const dedupeKey = (value: JsonValue): string => stableStringify(value);

  const uniqueName = (preferred: string): string => {
    let name = preferred;
    let suffix = 2;
    while (name in rootDefs) {
      name = `${preferred}__${suffix}`;
      suffix += 1;
    }
    return name;
  };

  const register = (preferredName: string, resolved: Record<string, JsonValue>): string => {
    const key = dedupeKey(resolved);
    const existing = keyToName.get(key);
    if (existing !== undefined) return existing;
    const name = uniqueName(preferredName);
    rootDefs[name] = resolved;
    keyToName.set(key, name);
    return name;
  };

  const resolveNamed = (
    name: string,
    target: JsonValue,
    scopeStack: Array<Record<string, JsonValue>>,
  ): string => {
    if (isPlainObject(target) || Array.isArray(target)) {
      const existingProgress = inProgress.get(target as object);
      if (existingProgress !== undefined) return existingProgress;
    }
    const reservedName = uniqueName(name);
    rootDefs[reservedName] = null; // reserve, guards against cycles
    if (isPlainObject(target) || Array.isArray(target)) {
      inProgress.set(target as object, reservedName);
    }
    let resolved: JsonValue;
    try {
      resolved = walk(target, scopeStack, false);
    } finally {
      if (isPlainObject(target) || Array.isArray(target)) {
        inProgress.delete(target as object);
      }
    }
    const key = dedupeKey(resolved);
    const existing = keyToName.get(key);
    if (existing !== undefined && existing !== reservedName) {
      delete rootDefs[reservedName];
      return existing;
    }
    rootDefs[reservedName] = resolved;
    keyToName.set(key, reservedName);
    return reservedName;
  };

  const walk = (
    node: JsonValue,
    scopeStack: Array<Record<string, JsonValue>>,
    wrap = true,
  ): JsonValue => {
    if (Array.isArray(node)) return node.map((item) => walk(item, scopeStack));
    if (!isPlainObject(node)) return node;

    let scopes = scopeStack;
    const localDefs = node.$defs;
    if (isPlainObject(localDefs)) scopes = [...scopeStack, localDefs];

    const ref = node.$ref;
    if (typeof ref === 'string' && ref.startsWith('#/$defs/')) {
      const name = ref.slice('#/$defs/'.length);
      let target: JsonValue;
      for (let index = scopes.length - 1; index >= 0; index -= 1) {
        const scope = scopes[index]!;
        if (name in scope) {
          target = scope[name];
          break;
        }
      }
      const overrides: Record<string, JsonValue> = {};
      for (const [key, value] of Object.entries(node)) {
        if (key !== '$ref' && key !== '$defs') overrides[key] = walk(value, scopes);
      }
      if (target === undefined) return { $ref: ref, ...overrides };
      const finalName = resolveNamed(name, target, scopes);
      return { $ref: `#/$defs/${finalName}`, ...overrides };
    }

    const resolved: Record<string, JsonValue> = {};
    for (const [key, value] of Object.entries(node)) {
      if (key !== '$defs') resolved[key] = walk(value, scopes);
    }

    if (wrap && typeof resolved.title === 'string' && isDedupableJsonschema(resolved)) {
      const name = register(resolved.title, resolved);
      return { $ref: `#/$defs/${name}` };
    }
    return resolved;
  };

  const organised = walk(schema, [], false);
  if (Object.keys(rootDefs).length > 0) {
    return { $defs: rootDefs, ...(organised as Record<string, JsonValue>) };
  }
  return organised as Record<string, JsonValue>;
}

/** Deterministic JSON with object keys sorted, used only for dedupe keys. */
function stableStringify(value: JsonValue): string {
  return JSON.stringify(value, (_key, val) => {
    if (isPlainObject(val)) {
      const sorted: Record<string, JsonValue> = {};
      for (const key of Object.keys(val).sort()) sorted[key] = val[key];
      return sorted;
    }
    return val;
  });
}
