/**
 * The TypeScript half of the schema-document contract.
 *
 * `testdata/actions/schema_document.json` is one `a11.actions/v1` document as
 * the native writer produces it. These tests pin that this side reads it into
 * the same schema and writes the same document back -- which is the thing that
 * replaces four hand-copied handshake schemas, and the only reason to trust that
 * a gateway asking a browser page what it serves gets an answer it understands.
 */

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

import {
  ActionRegistry,
  SCHEMA_DOCUMENT_FORMAT,
  builtinActionNames,
  isOk,
  schemaFromJson,
  schemaToJson,
  schemasInDocument,
} from '../dist/index.js';

const fixture = JSON.parse(
  readFileSync(
    fileURLToPath(new URL('../../testdata/actions/schema_document.json', import.meta.url)),
    'utf8',
  ),
);

test('a native document reads into a schema and writes back unchanged', () => {
  const entries = schemasInDocument(fixture.callable);
  assert.equal(isOk(entries), true);
  assert.equal(entries.length, 1);

  const schema = schemaFromJson(entries[0]);
  assert.equal(isOk(schema), true);
  assert.equal(schema.name, 'shell_execute');
  assert.equal(schema.description, 'Run a shell command.');
  assert.deepEqual([...schema.inputs.keys()].sort(), ['command', 'parameters']);
  assert.equal(schema.inputs.get('command').required, true);
  assert.equal(schema.inputs.get('command').unary, true);
  // Streaming, and it has to survive: the port structs in A11 disagree on what
  // an absent `unary` means, so a reader filling one in would invert this.
  assert.equal(schema.outputs.get('output_lines').unary, false);
  assert.equal(schema.outputs.get('status').unary, true);
  assert.equal(schema.outputToJsonField.get('status'), '$');
  // A port's value schema travels, which is what lets a model be shown a remote
  // tool's real argument types -- there is no local type here to derive one from.
  assert.equal(
    JSON.parse(schema.inputs.get('command').jsonSchema).type,
    'string',
  );

  // And back: the same entry, byte for byte after canonical ordering.
  const written = schemaToJson(schema, entries[0].runnable ?? false);
  assert.deepEqual(written, entries[0]);
});

test('unary is always written, never omitted when false', () => {
  const entries = schemasInDocument(fixture.callable);
  for (const entry of entries) {
    for (const port of [...(entry.inputs ?? []), ...(entry.outputs ?? [])]) {
      assert.ok(
        Object.hasOwn(port, 'unary'),
        `${entry.name}.${port.name} omitted unary`,
      );
    }
  }
});

test('a whole registry writes the document the native writer wrote', () => {
  const entries = schemasInDocument(fixture.callable);
  const schema = schemaFromJson(entries[0]);
  const registry = new ActionRegistry();
  // Schema only, no handler -- which is what `runnable: false` in the fixture
  // says, and how "this one lives on the peer" is spelled.
  assert.equal(isOk(registry.register(schema.name, schema)), true);

  const own = registry
    .listRegisteredActions()
    .filter((name) => !name.startsWith('__'));
  assert.deepEqual(own, ['shell_execute']);
  const round = schemaToJson(registry.getSchema('shell_execute'), false);
  assert.deepEqual(round, entries[0]);
});

test('the all-ports view keeps what a caller cannot write', () => {
  // Nothing in this fixture is autofilled, so the two views agree -- which is
  // itself worth pinning: `ports: "all"` must not change anything else.
  assert.deepEqual(
    schemasInDocument(fixture.all_ports),
    schemasInDocument(fixture.callable),
  );
});

test('the format tag and the builtin names match the native ones', () => {
  assert.equal(fixture.callable.format, SCHEMA_DOCUMENT_FORMAT);
  assert.deepEqual(builtinActionNames(), [
    '__get_schema__',
    '__list_actions__',
    '__ping',
  ]);
});

test('a builtin is answered by a registry that holds nothing', async () => {
  const registry = new ActionRegistry();
  for (const name of builtinActionNames()) {
    assert.equal(registry.isRegistered(name), true, name);
    assert.equal(isOk(registry.getSchema(name)), true, name);
    assert.equal(isOk(registry.getHandler(name)), true, name);
  }
  // Refused rather than shadowed: a peer must always be answerable.
  const schema = schemaFromJson({
    name: '__list_actions__',
    outputs: [{ name: 'actions', type: 'application/json', unary: true }],
  });
  assert.equal(isOk(schema), true);
  assert.equal(isOk(registry.register('__list_actions__', schema)), false);
  assert.equal(isOk(registry.unregister('__ping')), false);
});
