/**
 * The TypeScript half of the cross-language tag contract.
 *
 * `testdata/serial_tags.json` is the one table every language answers to, and
 * `testdata/interaction_golden.json` is one interaction as the Python SDK
 * writes it. Between them these tests pin the thing the plugin depends on: a
 * conversation can be held in TypeScript and handed back to a Python backend
 * turn after turn, with the tool calls and results inside each interaction
 * arriving as the very objects that were sent.
 */

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

import {
  ACTION_STATUS_MIMETYPE,
  ActionMessage,
  CLOSE_STATUS_ATTRIBUTE,
  Chunk,
  ChunkMetadata,
  NodeFragment,
  SerializationRegistry,
  decodeStatusChunk,
  fromChunk,
  isCloseStatusChunk,
  isOk,
  statusToChunk,
  makeTextMessageInteraction,
  makeOllamaCreateChatConfig,
  toChunk,
  valueTag,
  wireValueCodecs,
} from '../dist/index.js';
import * as tags from '../dist/serial_tags.js';

const testdata = (name) =>
  JSON.parse(readFileSync(fileURLToPath(new URL(`../../testdata/${name}`, import.meta.url)), 'utf8'));

const fixtureTags = () => {
  const data = testdata('serial_tags.json');
  const result = [];
  for (const [section, entries] of Object.entries(data)) {
    if (section.startsWith('_')) continue;
    result.push(...Object.values(entries));
  }
  return result;
};

const need = (value) => {
  assert.ok(isOk(value), `expected ok, got ${JSON.stringify(value)}`);
  return value;
};

const goldenChunk = () => {
  const golden = testdata('interaction_golden.json');
  return {
    golden,
    data: Buffer.from(golden.base64, 'base64'),
    chunk: new Chunk({
      data: new Uint8Array(Buffer.from(golden.base64, 'base64')),
      metadata: new ChunkMetadata({ mimetype: golden.mimetype }),
    }),
  };
};

test('every tag in the shared table has a TypeScript constant', () => {
  const constants = new Set(Object.values(tags).filter((value) => typeof value === 'string'));
  const missing = fixtureTags().filter((tag) => !constants.has(tag));
  assert.deepEqual(missing, [], 'tags in testdata/serial_tags.json with no TypeScript constant');
});

test('every TypeScript tag constant is in the shared table', () => {
  const declared = new Set(fixtureTags());
  const extra = Object.entries(tags)
    .filter(([name, value]) => typeof value === 'string' && name.endsWith('_TAG'))
    .map(([, value]) => value)
    .filter((tag) => !declared.has(tag));
  assert.deepEqual(extra, [], 'TypeScript tags absent from testdata/serial_tags.json');
});

test('the runtime and SDK types are registered under their canonical tags', () => {
  const registered = new Set(wireValueCodecs().map((codec) => codec.tag));
  for (const tag of [
    tags.CHUNK_TAG,
    tags.CHUNK_METADATA_TAG,
    tags.NODE_REF_TAG,
    tags.NODE_FRAGMENT_TAG,
    tags.PORT_TAG,
    tags.ACTION_MESSAGE_TAG,
    tags.WIRE_MESSAGE_TAG,
    tags.STATUS_TAG,
    tags.INTERACTION_TAG,
    tags.PEER_TAG,
    tags.ACTION_CONFIG_TAG,
    tags.USAGE_METADATA_TAG,
    tags.INTERACT_WITH_OLLAMA_CONFIG_TAG,
  ]) {
    assert.ok(registered.has(tag), `no wire value codec for ${tag}`);
  }
});

test('a status chunk is the one shape every language writes', () => {
  const fixture = testdata('status_chunk.json');
  assert.equal(ACTION_STATUS_MIMETYPE, fixture.mimetype);
  assert.equal(CLOSE_STATUS_ATTRIBUTE, fixture.close_attribute);

  for (const testCase of fixture.cases) {
    const status = {code: testCase.code, message: testCase.message, details: testCase.details};
    const chunk = need(statusToChunk(status));
    assert.equal(chunk.mimetype, fixture.mimetype, testCase.name);
    assert.equal(Buffer.from(chunk.data).toString('base64'), testCase.base64, testCase.name);
    assert.equal(isCloseStatusChunk(chunk), false, testCase.name);
    const decoded = need(decodeStatusChunk(chunk));
    assert.equal(decoded.status.code, testCase.code, testCase.name);
    assert.equal(decoded.status.message, testCase.message, testCase.name);
    assert.deepEqual(decoded.status.details ?? [], testCase.details, testCase.name);
  }
});

test('a closure marker only adds the shared attribute', () => {
  const fixture = testdata('status_chunk.json');
  const ok = {code: 0, message: ''};
  const plain = need(statusToChunk(ok));
  const marker = need(statusToChunk(ok, true));

  // The marker rides on the metadata, so the payload is the plain status.
  assert.deepEqual(marker.data, plain.data);
  assert.equal(marker.mimetype, plain.mimetype);
  assert.equal(isCloseStatusChunk(marker), true);
  assert.equal(isCloseStatusChunk(plain), false);
  assert.deepEqual(
    [...marker.metadata.attributes].map(([key, value]) => [key, Buffer.from(value).toString()]),
    [[fixture.close_attribute, '1']],
  );
});

test("Python's interaction decodes into real objects, not anonymous fields", async () => {
  const { chunk } = goldenChunk();

  const interaction = need(await fromChunk(chunk));

  assert.equal(valueTag(interaction), tags.INTERACTION_TAG);
  assert.equal(interaction.model, 'golden-model');
  // The point of the exercise: what a turn *did* survives the crossing.
  assert.ok(interaction.content[0] instanceof Chunk);
  assert.equal(interaction.content[0].mimetype, 'application/json');
  assert.ok(interaction.action_calls[0] instanceof ActionMessage);
  assert.equal(interaction.action_calls[0].name, 'rename_symbol');
  assert.ok(interaction.action_inputs.p[0] instanceof NodeFragment);
  assert.equal(interaction.action_inputs.p[0].id, 'n1');
  assert.equal(valueTag(interaction.status), tags.STATUS_TAG);
  assert.equal(valueTag(interaction.usage_metadata), tags.USAGE_METADATA_TAG);
  assert.equal(interaction.usage_metadata.output_tokens, 7);
  assert.equal(valueTag(interaction.action_configs.x), tags.ACTION_CONFIG_TAG);
  assert.ok(interaction.backend_specific_metadata.stop instanceof Uint8Array);
});

test("an interaction handed back is byte-for-byte the one that arrived", async () => {
  const { chunk, data } = goldenChunk();

  const interaction = need(await fromChunk(chunk));
  const reencoded = need(await toChunk(interaction));

  assert.equal(reencoded.mimetype, `application/json;type=${tags.INTERACTION_TAG}`);
  assert.equal(Buffer.from(reencoded.data).toString(), data.toString());
});

test('an interaction built here is tagged for the strict interactions port', async () => {
  const interaction = need(await makeTextMessageInteraction('hello', 'be brief'));

  const chunk = need(await toChunk(interaction));

  assert.equal(chunk.mimetype, `application/json;type=${tags.INTERACTION_TAG}`);
});

test('an interaction built here is the one the Python SDK builds', async () => {
  // This is what a chat client sends on the `interactions` port, and the
  // backend validates it against its own Interaction model -- so `content` and
  // `system_instructions` have to be Chunks, not the bare JSON they hold.
  const golden = testdata('text_message_interaction_golden.json');
  const expected = JSON.parse(Buffer.from(golden.base64, 'base64').toString());

  const interaction = need(
    await makeTextMessageInteraction(golden.text, golden.system_prompt),
  );
  const chunk = need(await toChunk(interaction));
  const actual = JSON.parse(Buffer.from(chunk.data).toString());

  assert.equal(chunk.mimetype, golden.mimetype);
  assert.deepEqual(actual.content, expected.content);
  assert.deepEqual(actual.system_instructions, expected.system_instructions);
  // Every other field this side writes must agree too. It writes fewer of them
  // — Python spells out `status`, `created_at_millis` and `usage_metadata`
  // where this side leaves them to the model's defaults, which is why the two
  // payloads are equivalent rather than identical.
  assert.equal(typeof actual.id, 'string');
  for (const [key, value] of Object.entries(actual)) {
    if (key === 'id') continue;
    assert.deepEqual(value, expected[key], `field ${key}`);
  }
});

test('a provider config is tagged so the unary config port accepts it', async () => {
  const config = need(makeOllamaCreateChatConfig({ temperature: 0.5 }));

  const chunk = need(await toChunk(config));

  assert.equal(
    chunk.mimetype,
    `application/json;type=${tags.INTERACT_WITH_OLLAMA_CONFIG_TAG}`,
  );
});

test('an ordinary object is still ordinary data', async () => {
  // Nothing here may be claimed by a tagged codec on shape alone -- a caller's
  // own {code, message} is data, not a Status.
  const chunk = need(await toChunk({ code: 0, message: 'hi' }));

  assert.equal(chunk.mimetype, 'application/json');
  assert.equal(valueTag(need(await fromChunk(chunk))), null);
});

test('a serialized interaction carries no tags', async () => {
  // The model's own schema says what everything is; nothing repeats it. The
  // chunk's `;type=` names the payload, and from there every nested value sits
  // in a field whose declared type identifies it.
  const keys = (value) => {
    if (Array.isArray(value)) return value.flatMap(keys);
    if (value !== null && typeof value === 'object') {
      return Object.entries(value).flatMap(([key, item]) => [key, ...keys(item)]);
    }
    return [];
  };
  const interaction = need(await makeTextMessageInteraction('hi', 'be brief'));
  const chunk = need(await toChunk(interaction));

  const payload = JSON.parse(new TextDecoder().decode(chunk.data));
  assert.deepEqual(keys(payload).filter((key) => key.startsWith('!')), []);
  assert.deepEqual(payload.content[0], {
    data: 'eyJyb2xlIjoidXNlciIsImNvbnRlbnQiOlt7InR5cGUiOiJ0ZXh0IiwidGV4dCI6ImhpIn1dfQ==',
    metadata: { mimetype: 'application/json' },
  });

  // And it still comes back as Chunks, from the schema alone.
  const decoded = need(await fromChunk(chunk));
  assert.ok(decoded.content[0] instanceof Chunk);
  assert.ok(decoded.system_instructions[0] instanceof Chunk);
  assert.equal(need(await fromChunk(decoded.system_instructions[0])), 'be brief');
});

test('no key in a payload is ever read as a type', async () => {
  // A payload is data. Nothing in it names a type, so nothing is escaped.
  // A11 once wrote a nested value's type as the sole key of a one-entry object
  // ({"!a11.Chunk": {...}}), which meant a caller's own object of that shape
  // had to be escaped on the way out. The type lives in the chunk's metadata
  // now, so these go out byte-for-byte as written.
  for (const value of [
    { [`!${tags.CHUNK_TAG}`]: 'not one' },
    { '!whatever': [1, 2] },
    { '!a': 1, b: 2 },
  ]) {
    const chunk = need(await toChunk(value));
    assert.deepEqual(JSON.parse(new TextDecoder().decode(chunk.data)), value);
    assert.deepEqual(need(await fromChunk(chunk)), value);
  }
});

test('an unloadable type tag still yields the payload', async () => {
  // A peer that never imported the naming module still holds valid JSON, and
  // is entitled to read it as such rather than be refused.
  const chunk = new Chunk({
    data: new TextEncoder().encode('{"anything":1}'),
    metadata: new ChunkMetadata({ mimetype: 'application/json;type=some.other.Model' }),
  });

  assert.deepEqual(need(await fromChunk(chunk)), { anything: 1 });
});

test('a registry built before an SDK import still sees its codecs', async () => {
  // The SDK registers on import, routinely after a registry was constructed.
  const registry = new SerializationRegistry({ registerDefaults: true });
  const interaction = need(await makeTextMessageInteraction('hi'));

  const chunk = need(await registry.toChunk(interaction));

  assert.equal(chunk.mimetype, `application/json;type=${tags.INTERACTION_TAG}`);
});
