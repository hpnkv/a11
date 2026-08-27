/**
 * An {@link ActionSchema} in JSON, which is how one travels.
 *
 * The TypeScript half of `a11.actions/v1`. The format's shape lives in
 * `cpp/a11/actions/describe.h`; this reads and writes the same thing, and
 * `testdata/actions/schema_document.json` pins the two against each other so
 * agreement is checked rather than assumed. That fixture exists because the
 * thing it replaces -- one handshake schema hand-copied into four languages --
 * is exactly what prose agreement produces.
 *
 * A document is `{format, actions}`; each entry is one schema plus `runnable`,
 * which says whether the answering side holds a handler and so is the
 * registry's annotation on a schema rather than part of one.
 *
 * `required` and `unary` are written only when true, and a port's `json_schema`
 * only when it says more than `{"type": "object"}` -- which is what an adapter
 * shows a model for a port carrying no schema at all. Every reader fills in
 * {@link ActionPortSchema}'s own defaults, so what is absent and what is
 * spelled out mean the same thing. Those defaults are this document's and not
 * A11's everywhere: `flow.catalogue/v1` defaults `unary` to true and omits it
 * when true, the opposite convention for a different format.
 */

import {
  ActionHeaderSchema,
  ActionPortSchema,
  ActionSchema,
} from './action_schema.js';
import { utf8Decode } from './bytes.js';
import {
  invalidArgumentError,
  isOk,
  type StatusOr,
} from './status.js';

/** The `format` field every schema document carries. */
export const SCHEMA_DOCUMENT_FORMAT = 'a11.actions/v1';

/** Which ports a written schema includes. */
export type PortView = 'callable' | 'all';

/** One entry of a document's `actions` array. */
export interface SchemaEntry {
  name: string;
  description?: string;
  runnable?: boolean;
  inputs?: PortEntry[];
  outputs?: PortEntry[];
  headers?: HeaderEntry[];
  output_to_json_field?: Record<string, string>;
}

/** One port, as an entry writes it. */
export interface PortEntry {
  name: string;
  type: string;
  description?: string;
  required?: boolean;
  unary?: boolean;
  autofilled?: boolean;
  json_schema?: unknown;
}

/** One header, as an entry writes it. */
export interface HeaderEntry {
  name: string;
  description?: string;
  has_default?: boolean;
  default?: string;
}

/** A whole document. */
export interface SchemaDocument {
  format: string;
  actions: SchemaEntry[];
}

/**
 * Returns whether a JSON Schema contains no constraints.
 *
 * A port with no schema is shown to a model as `{"type": "object"}` anyway, so
 * a document spelling that out states exactly what leaving it out does.
 */
function saysNothing(schema: unknown): boolean {
  return (
    schema !== null &&
    typeof schema === 'object' &&
    !Array.isArray(schema) &&
    Object.keys(schema).length === 1 &&
    (schema as { type?: unknown }).type === 'object'
  );
}

function portToJson(port: ActionPortSchema, autofilled: boolean): PortEntry {
  const entry: PortEntry = { name: port.name, type: port.type };
  if (port.description) entry.description = port.description;
  if (port.required) entry.required = true;
  if (port.unary) entry.unary = true;
  if (autofilled) entry.autofilled = true;
  if (port.jsonSchema) {
    try {
      const parsed: unknown = JSON.parse(port.jsonSchema);
      if (!saysNothing(parsed)) entry.json_schema = parsed;
    } catch {
      // A schema nobody can read is worse than an absent one, which at least
      // reads as "no type information".
    }
  }
  return entry;
}

function portFromJson(entry: PortEntry): StatusOr<ActionPortSchema> {
  return ActionPortSchema.create({
    name: entry.name,
    type: entry.type ?? 'application/json',
    description: entry.description ?? '',
    required: entry.required === true,
    // False when absent, which is what the writer omits the field for and what
    // ActionPortSchema itself defaults to.
    unary: entry.unary === true,
    jsonSchema:
      entry.json_schema === undefined || entry.json_schema === null
        ? ''
        : JSON.stringify(entry.json_schema),
  });
}

function sortedPorts(
  ports: Map<string, ActionPortSchema>,
): ActionPortSchema[] {
  // Declaration order is not preserved by the map, and a document that
  // reshuffles itself between calls is one nobody can diff.
  return [...ports.values()].sort((left, right) =>
    left.name < right.name ? -1 : left.name > right.name ? 1 : 0,
  );
}

/** One schema as an `actions` entry. */
export function schemaToJson(
  schema: ActionSchema,
  runnable = true,
  ports: PortView = 'callable',
): SchemaEntry {
  const entry: SchemaEntry = { name: schema.name };
  if (schema.description) entry.description = schema.description;
  entry.runnable = runnable;

  const inputs: PortEntry[] = [];
  for (const port of sortedPorts(schema.inputs)) {
    const autofilled = port.autofills.length > 0;
    // A caller cannot write an autofilled input: the runtime requires it empty
    // before applying the receiver's default, so offering it invites a failure.
    if (autofilled && ports === 'callable') continue;
    inputs.push(portToJson(port, autofilled));
  }
  const outputs: PortEntry[] = [];
  for (const port of sortedPorts(schema.outputs)) {
    outputs.push(portToJson(port, false));
  }
  if (inputs.length > 0) entry.inputs = inputs;
  if (outputs.length > 0) entry.outputs = outputs;

  const headers: HeaderEntry[] = [];
  for (const header of [...schema.headers.values()].sort((left, right) =>
    left.name < right.name ? -1 : left.name > right.name ? 1 : 0,
  )) {
    const written: HeaderEntry = { name: header.name };
    if (header.description) written.description = header.description;
    if (header.defaultValue !== null) {
      written.has_default = true;
      const text = utf8Decode(header.defaultValue);
      if (isOk(text)) written.default = text;
    }
    headers.push(written);
  }
  if (headers.length > 0) entry.headers = headers;

  if (schema.outputToJsonField.size > 0) {
    const mapping: Record<string, string> = {};
    for (const key of [...schema.outputToJsonField.keys()].sort()) {
      mapping[key] = schema.outputToJsonField.get(key) as string;
    }
    entry.output_to_json_field = mapping;
  }
  return entry;
}

/**
 * The schema an entry was written from.
 *
 * What cannot survive the trip comes back empty: an input's autofills are
 * receiver-owned defaults and remain local. A `user_facing` flag
 * from an older client is read and dropped -- narration travels on the reserved
 * log port, which no schema declares.
 */
export function schemaFromJson(entry: SchemaEntry): StatusOr<ActionSchema> {
  if (entry === null || typeof entry !== 'object') {
    return invalidArgumentError('An action schema entry must be an object.');
  }
  if (typeof entry.name !== 'string' || entry.name.length === 0) {
    return invalidArgumentError('An action schema entry must have a name.');
  }
  const inputs = new Map<string, ActionPortSchema>();
  const outputs = new Map<string, ActionPortSchema>();
  for (const [key, into] of [
    ['inputs', inputs],
    ['outputs', outputs],
  ] as const) {
    const written = entry[key];
    if (!Array.isArray(written)) continue;
    for (const one of written) {
      if (one === null || typeof one !== 'object') continue;
      if (typeof one.name !== 'string' || one.name.length === 0) continue;
      const port = portFromJson(one);
      if (!isOk(port)) return port;
      into.set(port.name, port);
    }
  }
  const headers = new Map<string, ActionHeaderSchema>();
  if (Array.isArray(entry.headers)) {
    for (const one of entry.headers) {
      if (one === null || typeof one !== 'object') continue;
      if (typeof one.name !== 'string' || one.name.length === 0) continue;
      const header = ActionHeaderSchema.create({
        name: one.name,
        description: one.description ?? '',
        defaultValue: typeof one.default === 'string' ? one.default : null,
      });
      if (!isOk(header)) return header;
      headers.set(header.name, header);
    }
  }
  const outputToJsonField = new Map<string, string>();
  if (entry.output_to_json_field && typeof entry.output_to_json_field === 'object') {
    for (const [output, field] of Object.entries(entry.output_to_json_field)) {
      // Only a mapping onto a port that came with it: an output named here and
      // absent above would fail validation with a message about the wrong
      // thing.
      if (typeof field !== 'string' || !outputs.has(output)) continue;
      outputToJsonField.set(output, field);
    }
  }
  return ActionSchema.create({
    name: entry.name,
    description: entry.description ?? '',
    inputs,
    outputs,
    headers,
    outputToJsonField,
  });
}

/**
 * The entries of a document, accepting a whole document or just its array.
 *
 * A caller handed one or the other should not have to care which.
 */
export function schemasInDocument(
  document: unknown,
): StatusOr<SchemaEntry[]> {
  if (Array.isArray(document)) return document as SchemaEntry[];
  if (document === null || typeof document !== 'object') {
    return invalidArgumentError(
      'An action schema document must be an object or an array.',
    );
  }
  const actions = (document as SchemaDocument).actions;
  if (!Array.isArray(actions)) {
    return invalidArgumentError(
      "An action schema document is missing its 'actions' array.",
    );
  }
  return actions as SchemaEntry[];
}

/** A whole document from entries. */
export function schemaDocument(actions: SchemaEntry[]): SchemaDocument {
  return { format: SCHEMA_DOCUMENT_FORMAT, actions };
}
