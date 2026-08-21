/**
 * The actions every A11 peer answers, whatever it was built to do.
 *
 * The TypeScript half of `cpp/a11/actions/builtins.h`, and the reason a browser
 * page or an IDE plugin no longer announces anything: a gateway asks what this
 * side serves, and this is what answers.
 *
 * These are not registrations. A peer that cannot be asked has to be told, and
 * telling is what four hand-copied handshakes were for -- so discovery cannot be
 * something an application remembers to install. It has to hold for a registry
 * nobody configured and for one an application has called `unregister` all over,
 * which rules out being an entry in the map. What holds instead is this table,
 * consulted by {@link ActionRegistry} on a miss.
 *
 * The handlers reach their registry through the action they were given rather
 * than capturing one, which is what keeps this a module-level constant and not a
 * cycle.
 */

import type { Action, ActionHandler } from './action.js';
import { ActionPortSchema, ActionSchema } from './action_schema.js';
import { utf8Decode, utf8Encode } from './bytes.js';
import { Chunk, ChunkMetadata } from './data.js';
import {
  failedPreconditionError,
  invalidArgumentError,
  isOk,
  okStatus,
  type Status,
} from './status.js';
import {
  schemaDocument,
  schemaToJson,
  type PortView,
  type SchemaEntry,
} from './schema_json.js';

/** Lists the actions a peer serves, with their schemas. */
export const LIST_ACTIONS_NAME = '__list_actions__';
/** Returns one action's schema, or NotFound. */
export const GET_SCHEMA_NAME = '__get_schema__';
/** Echoes a value, so a caller can tell A11 from anything holding a port. */
export const PING_NAME = '__ping';

const JSON_MIMETYPE = 'application/json';
const TEXT_MIMETYPE = 'text/plain';

function port(
  name: string,
  type: string,
  description: string,
  required: boolean,
  unary: boolean,
): ActionPortSchema {
  return new ActionPortSchema({ name, type, description, required, unary });
}

function textChunk(text: string, mimetype: string): Chunk {
  return new Chunk({
    data: utf8Encode(text),
    metadata: new ChunkMetadata({ mimetype }),
  });
}

/**
 * The text on a unary input, or '' where it said nothing.
 *
 * Reads bytes rather than deserialising: the two things a builtin accepts are a
 * JSON request document and an action name, which are text either way.
 */
async function readUnaryText(action: Action, portName: string): Promise<string> {
  const node = await action.getInput(portName);
  if (!isOk(node)) return '';
  const chunk = await node.nextChunk();
  if (!isOk(chunk) || chunk === null || chunk.isNull || chunk.isEmpty) {
    return '';
  }
  const text = utf8Decode(chunk.data);
  return isOk(text) ? text : '';
}

async function writeUnary(
  action: Action,
  portName: string,
  text: string,
  mimetype: string,
): Promise<Status> {
  const node = await action.getOutput(portName);
  if (!isOk(node)) return node;
  const written = await node.finalize(textChunk(text, mimetype));
  return isOk(written) ? okStatus() : written;
}

/** Which schemas a request asked for, and how much of each. */
interface SchemaQuery {
  names: string[];
  exact: string[];
  ports: PortView;
  includeReserved: boolean;
  runnableOnly: boolean;
}

function parseQuery(encoded: string): SchemaQuery {
  const query: SchemaQuery = {
    names: [],
    exact: [],
    ports: 'callable',
    includeReserved: false,
    runnableOnly: false,
  };
  const trimmed = encoded.trim();
  // No request is the default request: asking a peer what it serves, with
  // nothing further to say, is the common case and must not need a document.
  if (trimmed === '' || trimmed === 'null') return query;
  let value: unknown;
  try {
    value = JSON.parse(trimmed);
  } catch {
    return query;
  }
  if (Array.isArray(value)) {
    // A bare array is read as patterns, because that is what a caller who wrote
    // one meant.
    query.names = value.filter((one): one is string => typeof one === 'string');
    return query;
  }
  if (value === null || typeof value !== 'object') return query;
  const asked = value as Record<string, unknown>;
  if (Array.isArray(asked.names)) {
    query.names = asked.names.filter((one): one is string => typeof one === 'string');
  }
  if (Array.isArray(asked.exact)) {
    query.exact = asked.exact.filter((one): one is string => typeof one === 'string');
  }
  if (asked.ports === 'all') query.ports = 'all';
  query.includeReserved = asked.include_reserved === true;
  query.runnableOnly = asked.runnable_only === true;
  return query;
}

/** Whether a name is one of A11's own rather than an application's. */
export function isReservedActionName(name: string): boolean {
  return name.length > 4 && name.startsWith('__');
}

function accepts(query: SchemaQuery, name: string): boolean {
  const named = query.exact.includes(name);
  if (!query.includeReserved && isReservedActionName(name) && !named) return false;
  if (query.names.length === 0 && query.exact.length === 0) return true;
  if (named) return true;
  for (const pattern of query.names) {
    try {
      // Full match, the same rule `x-a11-allowed-llm-actions` uses, so a
      // pattern means one thing across A11 rather than two.
      if (new RegExp(`^(?:${pattern})$`).test(name)) return true;
    } catch {
      // A pattern that will not compile matches nothing.
    }
  }
  return false;
}

async function runListActions(action: Action): Promise<Status> {
  const registry = action.getRegistry();
  if (registry === null || registry === undefined) {
    return failedPreconditionError(
      'This action was not dispatched through a registry, so there is nothing to describe.',
    );
  }
  const query = parseQuery(await readUnaryText(action, 'request'));
  const entries: SchemaEntry[] = [];
  for (const name of [...registry.listRegisteredActions()].sort()) {
    if (!accepts(query, name)) continue;
    const schema = registry.getSchema(name);
    if (!isOk(schema)) continue;
    const runnable = isOk(registry.getHandler(name));
    if (query.runnableOnly && !runnable) continue;
    entries.push(schemaToJson(schema, runnable, query.ports));
  }
  return writeUnary(
    action,
    'actions',
    JSON.stringify(schemaDocument(entries)),
    JSON_MIMETYPE,
  );
}

async function runGetSchema(action: Action): Promise<Status> {
  const registry = action.getRegistry();
  if (registry === null || registry === undefined) {
    return failedPreconditionError(
      'This action was not dispatched through a registry, so there is nothing to describe.',
    );
  }
  const name = await readUnaryText(action, 'action');
  if (name === '') {
    return invalidArgumentError(
      "__get_schema__ needs the name of an action on its 'action' input.",
    );
  }
  // NotFound rather than an empty document, and distinct from the
  // InvalidArgument an unnameable id gets.
  const schema = registry.getSchema(name);
  if (!isOk(schema)) return schema;
  const runnable = isOk(registry.getHandler(name));
  return writeUnary(
    action,
    'schema',
    JSON.stringify(schemaDocument([schemaToJson(schema, runnable, 'all')])),
    JSON_MIMETYPE,
  );
}

async function runPing(action: Action): Promise<Status> {
  const value = await readUnaryText(action, 'input');
  return writeUnary(action, 'output', value, TEXT_MIMETYPE);
}

function listActionsSchema(): ActionSchema {
  return new ActionSchema({
    name: LIST_ACTIONS_NAME,
    description:
      'List the actions this peer serves, with their schemas, as one'
      + ' a11.actions/v1 document. Takes an optional request object on'
      + " 'request': 'names' (full-match patterns), 'exact' (names), 'ports'"
      + ' ("callable" or "all"), \'include_reserved\', and \'runnable_only\'.',
    inputs: new Map([
      ['request', port('request', JSON_MIMETYPE,
        'Which actions to describe. Absent means all of them.', false, true)],
    ]),
    outputs: new Map([
      ['actions', port('actions', JSON_MIMETYPE,
        'The a11.actions/v1 document, whole.', true, true)],
    ]),
  });
}

function getSchemaSchema(): ActionSchema {
  return new ActionSchema({
    name: GET_SCHEMA_NAME,
    description:
      'Describe one action this peer serves, as an a11.actions/v1 document.'
      + ' Fails NOT_FOUND when the name is not registered here.',
    inputs: new Map([
      ['action', port('action', TEXT_MIMETYPE,
        'Name of the action to describe.', true, true)],
    ]),
    outputs: new Map([
      ['schema', port('schema', JSON_MIMETYPE,
        'The a11.actions/v1 document for that one action.', true, true)],
    ]),
  });
}

function pingSchema(): ActionSchema {
  return new ActionSchema({
    // Wording and shape kept exactly: four languages' clients probe with this,
    // and a probe that started describing itself differently would be the first
    // thing anybody diffing two peers noticed.
    name: PING_NAME,
    description:
      'Ping the server to check if it is alive. Requires a single value on the'
      + ' port `input`, which it returns as a single value on the port `output`.',
    inputs: new Map([
      ['input', port('input', TEXT_MIMETYPE, 'Ping input value', false, false)],
    ]),
    outputs: new Map([
      ['output', port('output', TEXT_MIMETYPE, 'Pong response value', false, false)],
    ]),
  });
}

interface Builtin {
  schema: ActionSchema;
  handler: ActionHandler;
}

let table: Map<string, Builtin> | null = null;

function builtins(): Map<string, Builtin> {
  if (table === null) {
    table = new Map<string, Builtin>([
      [LIST_ACTIONS_NAME, { schema: listActionsSchema(), handler: runListActions }],
      [GET_SCHEMA_NAME, { schema: getSchemaSchema(), handler: runGetSchema }],
      [PING_NAME, { schema: pingSchema(), handler: runPing }],
    ]);
  }
  return table;
}

/** Whether `name` is a builtin every registry answers for. */
export function isBuiltinAction(name: string): boolean {
  return builtins().has(name);
}

/** The builtin named `name`, or undefined. */
export function getBuiltinAction(name: string): Builtin | undefined {
  return builtins().get(name);
}

/** Every builtin's name, sorted. */
export function builtinActionNames(): string[] {
  return [...builtins().keys()].sort();
}

export { okStatus };
