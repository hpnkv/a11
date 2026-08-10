import assert from 'node:assert/strict';
import test from 'node:test';

import {
  ActionMessage,
  Chunk,
  ChunkMetadata,
  NodeFragment,
  NodeRef,
  Port,
  SerializationRegistry,
  StatusCode,
  WireMessage,
  isOk,
  packStatus,
  okStatus,
} from '../dist/index.js';

const hex = (bytes) => Buffer.from(bytes).toString('hex');

test('A11 values use concatenated MessagePack fields', () => {
  const values = [
    [new ChunkMetadata(), 'a0c080', ChunkMetadata],
    [new Chunk(), 'c0a0c400', Chunk],
    [new NodeRef({ id: 'x' }), 'a17800c0', NodeRef],
    [new Port('x', 'y'), 'a178a179', Port],
    [new WireMessage(), '01909080', WireMessage],
  ];

  for (const [value, expected, type] of values) {
    const encoded = value.toMsgpack();
    assert.equal(isOk(encoded), true);
    assert.equal(hex(encoded), expected);
    const decoded = type.fromMsgpack(encoded);
    assert.equal(isOk(decoded), true);
    assert.ok(decoded instanceof type);
  }

  const packed = packStatus(okStatus());
  assert.equal(isOk(packed), true);
  assert.equal(hex(packed), '00a24f4b90');
});

test('nested A11 wire values round-trip without exceptions', () => {
  const message = new WireMessage({
    nodeFragments: [new NodeFragment({
      id: 'node',
      data: new Chunk({
        metadata: new ChunkMetadata({ mimetype: 'text/plain' }),
        data: new TextEncoder().encode('hello'),
      }),
      seq: 7,
      continued: false,
    })],
    actions: [new ActionMessage({
      id: 'action',
      name: 'echo',
      inputs: [new Port('input', 'node')],
      outputs: [new Port('output', 'result')],
      headers: new Map([['x-a11-test', new Uint8Array([1, 2])]]),
    })],
  });
  const encoded = message.toMsgpack();
  assert.equal(isOk(encoded), true);
  const decoded = WireMessage.fromMsgpack(encoded);
  assert.equal(isOk(decoded), true);
  assert.equal(decoded.nodeFragments[0].id, 'node');
  assert.equal(decoded.actions[0].outputs[0].id, 'result');
  assert.deepEqual([...decoded.actions[0].headers.get('x-a11-test')], [1, 2]);

  const malformed = WireMessage.fromMsgpack(
    Uint8Array.from([...encoded, 0]),
  );
  assert.equal(isOk(malformed), false);
  assert.equal(malformed.code, StatusCode.INVALID_ARGUMENT);
});

test('a value JSON already describes carries no type parameter', async () => {
  const registry = new SerializationRegistry({ registerDefaults: true });
  const values = [null, true, 12, 1.5, 'hello', [1, 2], { answer: 42 }];

  for (const value of values) {
    const chunk = await registry.toChunk(value);
    assert.equal(isOk(chunk), true);
    // JSON says as much on its own; repeating it in the mimetype would only
    // stop a peer holding a bare `application/json` from being understood.
    assert.equal(chunk.mimetype, 'application/json');
    const decoded = await registry.fromChunk(chunk);
    assert.equal(isOk(decoded), true);
    assert.deepEqual(decoded, value);
  }
});

test('a bare mimetype is a complete description', async () => {
  const registry = new SerializationRegistry({ registerDefaults: true });
  const chunk = new Chunk({
    metadata: new ChunkMetadata({ mimetype: 'application/json' }),
    data: new TextEncoder().encode('{"answer":42}'),
  });

  assert.deepEqual(await registry.fromChunk(chunk), { answer: 42 });
});

test('an unloadable type tag still yields the payload', async () => {
  // A peer that never imported the naming module still holds valid JSON.
  const registry = new SerializationRegistry({ registerDefaults: true });
  const chunk = new Chunk({
    metadata: new ChunkMetadata({
      mimetype: 'application/json;type=never.imported.Model',
    }),
    data: new TextEncoder().encode('{"answer":42}'),
  });

  assert.deepEqual(await registry.fromChunk(chunk), { answer: 42 });
});

test('TypeScript-native extended values round-trip', async () => {
  const registry = new SerializationRegistry({ registerDefaults: true });
  const date = new Date('2026-08-03T10:11:12.345Z');
  const blob = new Blob([new Uint8Array([137, 80, 78, 71])], {
    type: 'image/png',
  });
  const values = [
    date,
    blob,
    new Set(['a', 'b']),
    // A Map goes out as an object, so its keys come back as the strings JSON
    // and MessagePack can spell. Nothing in the payload says otherwise.
    new Map([['one', 1]]),
    2n ** 60n,
    new Uint8Array([0, 1, 255]),
  ];
  for (const value of values) {
    const chunk = await registry.toChunk(value);
    assert.equal(isOk(chunk), true);
    const decoded = await registry.fromChunk(chunk);
    assert.equal(isOk(decoded), true);
    if (value instanceof Date) {
      assert.ok(decoded instanceof Date);
      assert.equal(decoded.toISOString(), value.toISOString());
    } else if (value instanceof Blob) {
      assert.ok(decoded instanceof Blob);
      assert.equal(decoded.type, 'image/png');
      assert.deepEqual(
        new Uint8Array(await decoded.arrayBuffer()),
        new Uint8Array(await value.arrayBuffer()),
      );
    } else {
      assert.deepEqual(decoded, value);
    }
  }

  const jsonBytes = await registry.toChunk(
    new Uint8Array([4, 5, 6]),
    'application/json',
  );
  assert.equal(isOk(jsonBytes), true);
  assert.equal(jsonBytes.mimetype, 'application/json;type=bytes');
  assert.deepEqual(await registry.fromChunk(jsonBytes), new Uint8Array([4, 5, 6]));
});

test('foreign serialization callbacks become statuses, not rejections', async () => {
  const registry = new SerializationRegistry();
  const registered = registry.register({
    tag: 'broken',
    mimetype: 'application/x-broken',
    test: (value) => typeof value === 'string',
    serialize: () => { throw new TypeError('serializer exploded'); },
    deserialize: () => { throw new TypeError('deserializer exploded'); },
  });
  assert.equal(isOk(registered), true);

  const encoded = await registry.toChunk('value');
  assert.equal(isOk(encoded), false);
  assert.equal(encoded.code, StatusCode.UNKNOWN);

  const chunk = new Chunk({
    metadata: new ChunkMetadata({
      mimetype: 'application/x-broken;type=broken',
    }),
  });
  const decoded = await registry.fromChunk(chunk);
  assert.equal(isOk(decoded), false);
  assert.equal(decoded.code, StatusCode.UNKNOWN);
});
