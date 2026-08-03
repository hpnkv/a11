import assert from 'node:assert/strict';
import test from 'node:test';

import {
  AsyncNode,
  BytePacketType,
  ByteReassembler,
  Chunk,
  ChunkStoreReader,
  ChunkStoreWriter,
  LocalChunkStore,
  StatusCode,
  isOk,
  parseBytePacket,
  splitBytesIntoPackets,
  unavailableError,
} from '../dist/index.js';

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
  assert.equal(isOk(await node.putFinal({ answer: 42 })), true);
  assert.equal(isOk(await node.drainAndClose()), true);

  assert.equal(await node.next(), 'first');
  assert.deepEqual(await node.next(), { answer: 42 });
  assert.equal(await node.next(), null);
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
