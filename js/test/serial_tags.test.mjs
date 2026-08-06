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
  ActionMessage,
  Chunk,
  ChunkMetadata,
  NodeFragment,
  SerializationRegistry,
  fromChunk,
  isOk,
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

test("Python's interaction decodes into real objects, not anonymous fields", async () => {
  const { chunk } = goldenChunk();

  const interaction = need(await fromChunk(chunk));

  assert.equal(valueTag(interaction), tags.INTERACTION_TAG);
  assert.equal(interaction.model, 'golden-model');
  // The point of the exercise: what a turn *did* survives the crossing.
  assert.ok(interaction.content[0] instanceof Chunk);
  assert.equal(interaction.content[0].mimetype, 'application/json;type=object');
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

  assert.equal(chunk.mimetype, 'application/json;type=object');
  assert.equal(valueTag(need(await fromChunk(chunk))), null);
});

test('a payload written before the tags were unified still reads', async () => {
  const legacy = {
    id: 'x',
    role: 'user',
    status: {
      __a11_serialized_type__: 'a11.value',
      value: { code: 0, message: '' },
      class_name: 'Status',
    },
    content: [
      {
        __a11_serialized_type__: 'a11.value',
        value: {
          data: { __a11_serialized_type__: 'bytes', value: 'ImhpIg==' },
          metadata: { mimetype: 'application/json;type=string' },
        },
        class_name: 'Chunk',
      },
    ],
    usage_metadata: {
      __a11_serialized_type__: 'pydantic',
      value: { input_tokens: 2 },
      class_name: 'a11.sdk.llm.UsageMetadata',
    },
  };
  const chunk = new Chunk({
    data: new TextEncoder().encode(JSON.stringify(legacy)),
    metadata: new ChunkMetadata({ mimetype: 'application/json;type=a11.sdk.llm.Interaction' }),
  });

  const interaction = need(await fromChunk(chunk));

  assert.ok(interaction.content[0] instanceof Chunk);
  assert.equal(valueTag(interaction.status), tags.STATUS_TAG);
  assert.equal(interaction.usage_metadata.input_tokens, 2);
});

test('an unknown class_name is reported rather than silently passed through', async () => {
  const payload = {
    content: [
      {
        __a11_serialized_type__: 'pydantic',
        value: { anything: 1 },
        class_name: 'some.other.Model',
      },
    ],
  };
  const chunk = new Chunk({
    data: new TextEncoder().encode(JSON.stringify(payload)),
    metadata: new ChunkMetadata({ mimetype: `application/json;type=${tags.INTERACTION_TAG}` }),
  });

  const result = await fromChunk(chunk);

  assert.ok(!isOk(result));
  assert.match(result.message, /some\.other\.Model/);
});

test('a registry built before an SDK import still sees its codecs', async () => {
  // The SDK registers on import, routinely after a registry was constructed.
  const registry = new SerializationRegistry({ registerDefaults: true });
  const interaction = need(await makeTextMessageInteraction('hi'));

  const chunk = need(await registry.toChunk(interaction));

  assert.equal(chunk.mimetype, `application/json;type=${tags.INTERACTION_TAG}`);
});
