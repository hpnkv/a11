import assert from 'node:assert/strict';
import {createA11WasmRuntime} from './native-index.mjs';
import {InProcessWireStream} from '../../../js/dist/in_process_wire_stream.js';
import {WireMessage} from '../../../js/dist/data.js';
import {bridgeWireStream} from '../../../js/dist/wire_stream_bridge.js';
import {isOk} from '../../../js/dist/status.js';

const pair = InProcessWireStream.createPair();
assert.ok(isOk(pair));
const [client, server] = pair;
const runtime = await createA11WasmRuntime();
try {
  const session = runtime.session('sdk-host-stream-session');
  await session.ready();
  let received;
  const incoming = new Promise(resolve => { received = resolve; });
  const serverStarted = server.accept(message => {
    if (message !== null) received(message);
  });
  await session.addStream(bridgeWireStream(client));
  assert.ok(isOk(await serverStarted));
  const outbound = {node_fragments: [{id: 'sdk-host-node',
    data: {data: 'aGVsbG8='}}]};
  await session.send(outbound);
  const observed = await Promise.race([incoming,
    new Promise((_, reject) => setTimeout(() => reject(
      new Error('native message was not delivered')), 2000))]);
  assert.deepEqual(observed.toJsonValue(), outbound);
  const reply = WireMessage.fromJson(JSON.stringify({
    node_fragments: [{id: 'sdk-host-reply', data: {data: 'd29ybGQ='}}],
  }));
  assert.ok(isOk(reply));
  assert.ok(isOk(server.send(reply)));
  const replyChunk = await runtime.node('sdk-host-reply').nextChunk({
    timeoutMs: 2000,
  });
  assert.equal(replyChunk.data, 'd29ybGQ=');
  await session.halfClose();
  assert.ok(isOk(server.halfClose()));
  assert.equal((await session.done({timeoutMs: 2000})).code, 0);
  await runtime.detachWireStream(client.getId());
} finally {
  runtime.dispose();
}
