import assert from 'node:assert/strict';
import test from 'node:test';

import {
  AsyncNode,
  BytePacketType,
  ByteReassembler,
  Chunk,
  ChunkMetadata,
  ChunkStoreReader,
  ChunkStoreWriter,
  LocalChunkStore,
  NodeFragment,
  StatusCode,
  isOk,
  parseBytePacket,
  splitBytesIntoPackets,
  unavailableError,
} from '../dist/index.js';

test('sticky mimetypes compress writes and expand ordered reads', async () => {
  const store = LocalChunkStore.create('sticky-mimetype');
  assert.equal(isOk(store), true);
  const writer = ChunkStoreWriter.create(store, { stickyMimetype: true });
  assert.equal(isOk(writer), true);
  const chunk = (value, withAttribute = false) => new Chunk({
    metadata: new ChunkMetadata({
      mimetype: 'text/plain',
      attributes: withAttribute
        ? new Map([['role', new TextEncoder().encode('assistant')]])
        : new Map(),
    }),
    data: new TextEncoder().encode(value),
  });

  assert.equal(await writer.putChunk(chunk('first')), 0);
  assert.equal(await writer.putChunk(chunk('gap-anchor'), 3), 3);
  assert.equal(await writer.putChunk(chunk('details', true), 4), 4);
  assert.equal(await writer.putChunk(chunk('stripped'), 5), 5);
  assert.equal(await writer.putChunk(chunk('second-gap-anchor'), 7, true), 7);
  assert.equal(isOk(await writer.drainAndClose()), true);

  const raw = [];
  for (const seq of [0, 3, 4, 5, 7]) raw.push((await store.get(seq)).data);
  assert.equal(raw[0].mimetype, 'text/plain');
  assert.equal(raw[1].mimetype, 'text/plain');
  assert.equal(raw[2].mimetype, '');
  assert.equal(raw[2].metadata.attributes.size, 1);
  assert.equal(raw[3].metadata, null);
  assert.equal(raw[4].mimetype, 'text/plain');

  const readStore = LocalChunkStore.create('sticky-reader');
  assert.equal(isOk(readStore), true);
  const put = await readStore.putMany([
    new NodeFragment({ data: chunk('anchor'), seq: 0, continued: true }),
    new NodeFragment({
      data: new Chunk({
        metadata: new ChunkMetadata({
          attributes: new Map([['role', new TextEncoder().encode('assistant')]]),
        }),
      }),
      seq: 1,
      continued: true,
    }),
    new NodeFragment({ data: new Chunk(), seq: 2, continued: false }),
  ]);
  assert.deepEqual(put, [0, 1, 2]);
  const reader = ChunkStoreReader.create(readStore, { stickyMimetype: true });
  assert.equal(isOk(reader), true);
  const read = [await reader.next(), await reader.next(), await reader.next()];
  assert.deepEqual(read.map((fragment) => fragment.data.mimetype), [
    'text/plain',
    'text/plain',
    'text/plain',
  ]);
  assert.equal(read[1].data.metadata.attributes.size, 1);
  assert.equal(read[2].data.metadata instanceof ChunkMetadata, true);
});

test('byte chunking reassembles out-of-order interleaved messages', () => {
  const first = Uint8Array.from({ length: 137 }, (_, index) => index & 0xff);
  const second = Uint8Array.from(
    { length: 91 },
    (_, index) => (255 - index) & 0xff,
  );
  const firstPackets = splitBytesIntoPackets(first, 11n, 31);
  const secondPackets = splitBytesIntoPackets(second, 22n, 31);
  assert.equal(isOk(firstPackets), true);
  assert.equal(isOk(secondPackets), true);

  const parsedFirst = parseBytePacket(firstPackets[0]);
  assert.equal(isOk(parsedFirst), true);
  assert.equal(parsedFirst.type, BytePacketType.LENGTH_SUFFIXED_BYTE_CHUNK);
  assert.equal(parsedFirst.transientId, 11n);
  assert.equal(parsedFirst.packetCount, firstPackets.length);

  const reassembler = ByteReassembler.create({
    packetSize: 31,
    maxMessageSize: 1024,
    maxPendingMessages: 4,
    maxPendingBytes: 2048,
  });
  assert.equal(isOk(reassembler), true);

  const delivery = [];
  const schedule = [
    ...firstPackets.slice(1).reverse().map((packet) => ['first', packet]),
    ...secondPackets.slice(1).reverse().map((packet) => ['second', packet]),
    ['first', firstPackets[0]],
    ['second', secondPackets[0]],
  ];
  for (const [name, packet] of schedule) {
    const result = reassembler.feed(packet);
    assert.equal(isOk(result), true);
    if (result !== null) delivery.push([name, result]);
  }
  assert.deepEqual(delivery, [['first', first], ['second', second]]);
  assert.equal(reassembler.pendingMessageCount, 0);
  assert.equal(reassembler.pendingByteCount, 0);
});

test('byte reassembly enforces duplicate and pending-message bounds', () => {
  const a = splitBytesIntoPackets(new Uint8Array(100), 1n, 31);
  const b = splitBytesIntoPackets(new Uint8Array(100), 2n, 31);
  assert.equal(isOk(a), true);
  assert.equal(isOk(b), true);
  const reassembler = ByteReassembler.create({
    packetSize: 31,
    maxMessageSize: 1024,
    maxPendingMessages: 1,
    maxPendingBytes: 2048,
  });
  assert.equal(isOk(reassembler), true);
  assert.equal(reassembler.feed(a[0]), null);
  const duplicate = reassembler.feed(a[0]);
  assert.equal(isOk(duplicate), false);
  assert.equal(duplicate.code, StatusCode.ALREADY_EXISTS);
  const exhausted = reassembler.feed(b[0]);
  assert.equal(isOk(exhausted), false);
  assert.equal(exhausted.code, StatusCode.RESOURCE_EXHAUSTED);
});

test('AsyncNode streams typed values through shared reader/writer pumps', async () => {
  const node = await AsyncNode.create('node-stream');
  assert.equal(isOk(node), true);
  assert.equal(isOk(await node.put('first')), true);
  assert.equal(isOk(await node.finalize({ answer: 42 }, { wait: true })), true);

  assert.equal(await node.next(), 'first');
  assert.deepEqual(await node.next(), { answer: 42 });
  assert.equal(await node.next(), null);
});

test('AsyncNode finalize ends a stream without waiting for the store', async () => {
  // The ordinary producer's ending: one call, and nothing awaited for it. The
  // write and the closure are the writer pump's work.
  const node = await AsyncNode.create('finalize-async');
  assert.equal(isOk(await node.put('token')), true);
  assert.equal(isOk(await node.finalize()), true);

  assert.equal(await node.next(), 'token');
  assert.equal(await node.next(), null);
  assert.equal(await node.isWritable(), false);
});

test('AsyncNode finalize can leave the writer open, and close ends it', async () => {
  // Finality and closure are still two facts; `close: false` writes only one.
  const node = await AsyncNode.create('finalize-open');
  assert.equal(isOk(await node.finalize(undefined, { wait: true, close: false })), true);
  assert.equal(node.writer.isWritable(), true);
  assert.equal(await node.next(), null);
  assert.equal(isOk(await node.close()), true);
  assert.equal(node.writer.isWritable(), false);
});

test('AsyncNode close ends a stream that has no final value', async () => {
  // The specialised half: a log can say "no more", not "this was the last".
  const node = await AsyncNode.create('close-only');
  assert.equal(isOk(await node.put('line')), true);
  assert.equal(isOk(await node.close()), true);
  assert.equal(await node.next(), 'line');
  assert.equal(await node.next(), null);
  const refused = await node.consume();
  assert.equal(isOk(refused), false);
  assert.equal(refused.code, StatusCode.FAILED_PRECONDITION);
});

test('AsyncNode consume accepts both spellings of a single value', async () => {
  const asFinal = await AsyncNode.create('consume-final');
  assert.equal(isOk(await asFinal.finalize({ value: 1 }, { wait: true })), true);
  assert.deepEqual(await asFinal.consume(), { value: 1 });

  const thenNull = await AsyncNode.create('consume-then-null');
  assert.equal(isOk(await thenNull.put({ value: 2 })), true);
  assert.equal(isOk(await thenNull.finalize(undefined, { wait: true })), true);
  assert.deepEqual(await thenNull.consume(), { value: 2 });
});

test('AsyncNode consume reads a node holding no value as none', async () => {
  // A null chunk marks the end of a node; it is not a value in it. A caller
  // closing an optional port it has nothing to put on writes either nothing at
  // all or a bare null final, and both must read back the same -- this is how a
  // unary `config` port arrives when the caller wants the backend's defaults.
  const closedEmpty = await AsyncNode.create('consume-closed-empty');
  assert.equal(isOk(await closedEmpty.close()), true);
  assert.equal(await closedEmpty.consume({ allowNone: true }), null);

  const nullOnly = await AsyncNode.create('consume-null-only');
  assert.equal(isOk(await nullOnly.finalize(undefined, { wait: true })), true);
  assert.equal(await nullOnly.consume({ allowNone: true }), null);

  const strict = await AsyncNode.create('consume-null-only-strict');
  assert.equal(isOk(await strict.finalize(undefined, { wait: true })), true);
  const refused = await strict.consume();
  assert.equal(isOk(refused), false);
  assert.equal(refused.code, StatusCode.FAILED_PRECONDITION);
});

test('AsyncNode iteration skips a null marker rather than failing', async () => {
  const node = await AsyncNode.create('iterate-null');
  assert.equal(isOk(await node.put('first')), true);
  assert.equal(isOk(await node.put('second')), true);
  assert.equal(isOk(await node.finalize(undefined, { wait: true })), true);

  const values = [];
  for (;;) {
    const value = await node.next();
    if (value === null) break;
    values.push(value);
  }
  assert.deepEqual(values, ['first', 'second']);
});

test('ChunkStoreReader and Writer convert foreign promise failures to statuses', async () => {
  const rejectingStore = {
    get: async () => { throw new TypeError('get rejected'); },
    getByArrivalOrder: async () => { throw new TypeError('arrival rejected'); },
    next: async () => { throw new TypeError('next rejected'); },
    put: async () => { throw new TypeError('put rejected'); },
    putMany: async () => { throw new TypeError('putMany rejected'); },
    clearData: async () => { throw new TypeError('clear rejected'); },
    getSeqForArrivalOrder: async () => { throw new TypeError('seq rejected'); },
    getFinalSeq: async () => { throw new TypeError('final rejected'); },
    closeWritesWithStatus: async () => { throw new TypeError('close rejected'); },
    size: async () => { throw new TypeError('size rejected'); },
    getId: () => 'rejecting-store',
  };
  const reader = ChunkStoreReader.create(rejectingStore);
  const writer = ChunkStoreWriter.create(rejectingStore);
  assert.equal(isOk(reader), true);
  assert.equal(isOk(writer), true);

  const read = await reader.next(1000);
  assert.equal(isOk(read), false);
  assert.equal(read.code, StatusCode.UNKNOWN);

  const write = await writer.putChunk(new Chunk(), 0, true);
  assert.equal(isOk(write), false);
  assert.equal(write.code, StatusCode.UNKNOWN);
});

test('ChunkStore pumps reject malformed fulfilled values without hanging', async () => {
  const store = {
    get: async () => ({ unexpected: true }),
    getByArrivalOrder: async () => ({ unexpected: true }),
    next: async () => [],
    put: async () => 0,
    putMany: async () => ({ unexpected: true }),
    clearData: async () => ({ unexpected: true }),
    getSeqForArrivalOrder: async () => 0,
    getFinalSeq: async () => null,
    closeWritesWithStatus: async () => undefined,
    size: async () => 0,
    getId: () => 'malformed-store',
  };
  const reader = ChunkStoreReader.create(store);
  const writer = ChunkStoreWriter.create(store);
  assert.equal(isOk(reader), true);
  assert.equal(isOk(writer), true);

  const read = await reader.next(1000);
  assert.equal(isOk(read), false);
  assert.equal(read.code, StatusCode.DATA_LOSS);

  const write = await writer.putChunk(new Chunk(), 0, true);
  assert.equal(isOk(write), false);
  assert.equal(write.code, StatusCode.DATA_LOSS);

  const closingWriter = ChunkStoreWriter.create({
    ...store,
    putMany: async () => [0],
  });
  assert.equal(isOk(closingWriter), true);
  const closed = await closingWriter.drainAndClose();
  assert.equal(isOk(closed), false);
  assert.equal(closed.code, StatusCode.DATA_LOSS);
});

test('store abort status reaches pending readers without a rejection', async () => {
  const store = LocalChunkStore.create('aborted-node');
  assert.equal(isOk(store), true);
  const reader = ChunkStoreReader.create(store);
  const writer = ChunkStoreWriter.create(store);
  assert.equal(isOk(reader), true);
  assert.equal(isOk(writer), true);
  const failure = unavailableError('upstream disappeared', [{ retry: true }]);
  assert.equal(isOk(await writer.abortWithStatus(failure)), true);
  const received = await reader.next(1000);
  assert.equal(isOk(received), false);
  assert.equal(received.code, StatusCode.UNAVAILABLE);
  assert.deepEqual(received.details, [{ retry: true }]);
});
