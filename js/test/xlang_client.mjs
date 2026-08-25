// Copyright 2026 The A11 Authors.

// Drives the Python echo peer in xlang_peer.py from the TypeScript client, with
// several messages in flight so the sender folds them into one frame. What is
// being checked is that a Python peer reads a folded frame correctly: every
// fragment arrives, in order, with its mimetype.
//
// Run via xlang.test.mjs, which owns the Python subprocess.

import {
  Chunk,
  ChunkMetadata,
  HttpSseClientWireStream,
  NodeFragment,
  WireMessage,
  WireStreamWithRecv,
  isOk,
} from '../dist/index.js';
import { WebSocketWireStream } from '../dist/index.js';

export async function driveEcho(transport, port, count) {
  const stream = transport === 'websocket'
    ? WebSocketWireStream.createClient(`ws://127.0.0.1:${port}/xlang`, { enableH2: false })
    : HttpSseClientWireStream.create(`http://127.0.0.1:${port}`);
  if (!isOk(stream)) return stream;
  const endpoint = WireStreamWithRecv.create(stream);
  if (!isOk(endpoint)) return endpoint;
  const started = await endpoint.start();
  if (!isOk(started)) return started;

  // Sent without awaiting, so they queue together and the sender folds them.
  for (let index = 0; index < count; index += 1) {
    const sent = endpoint.send(new WireMessage({
      nodeFragments: [new NodeFragment({
        id: 'probe',
        data: new Chunk({
          metadata: new ChunkMetadata({ mimetype: 'application/json' }),
          data: new TextEncoder().encode(`{"i":${index}}`),
        }),
        seq: index,
        continued: true,
      })],
    }));
    if (!isOk(sent)) return sent;
  }

  const payloads = [];
  while (payloads.length < count) {
    const message = await endpoint.receive(10000);
    if (message === null) break;
    if (!isOk(message)) return message;
    for (const fragment of message.nodeFragments) {
      payloads.push({
        text: new TextDecoder().decode(fragment.data.data),
        mimetype: fragment.data.mimetype,
      });
    }
  }
  endpoint.halfClose();
  await endpoint.drainOutgoingMessages();
  return payloads;
}
