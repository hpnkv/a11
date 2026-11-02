/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import assert from 'node:assert/strict';
import test from 'node:test';
import {BytePacketType, ByteReassembler, StatusCode, isOk, parseBytePacket, splitBytesIntoPackets} from '../dist/index.js';

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
