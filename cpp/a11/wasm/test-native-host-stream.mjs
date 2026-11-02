import assert from 'node:assert/strict';
import {createA11WasmRuntime} from './native-index.mjs';
import {StatusCode} from './status.mjs';

const runtime = await createA11WasmRuntime();
try {
  const session = runtime.session('host-wire-session');
  await session.ready();
  let receive;
  const messages = [];
  const host = {
    id: 'host-wire-1',
    start(onMessage) {
      receive = onMessage;
      return {code: StatusCode.OK, message: '', details: []};
    },
    send(message) {
      messages.push(message);
      return {code: StatusCode.OK, message: '', details: []};
    },
    halfClose() {
      return {code: StatusCode.OK, message: '', details: []};
    },
    abort(status) { return status; },
    getStatus() { return {code: StatusCode.OK, message: '', details: []}; },
  };
  assert.equal(await session.addStream(host), host.id);
  await assert.rejects(runtime.command({operation: 'host_stream_prepare',
    id: host.id}), error => error.code === StatusCode.ALREADY_EXISTS);
  const state = await session.state();
  assert.equal(state.status.code, StatusCode.OK);
  const outgoing = {node_fragments: [{id: 'host-wire-node',
    data: {data: 'aGVsbG8='}}]};
  await session.send(outgoing, {streamId: host.id});
  for (let i = 0; i < 100 && messages.length === 0; i++) {
    await new Promise(resolve => setTimeout(resolve, 10));
  }
  assert.deepEqual(messages[0], outgoing, JSON.stringify(await session.state()));
  assert.equal((await receive({node_fragments: [{id: 'host-wire-reply',
    data: {data: 'd29ybGQ='}}]})).code,
    StatusCode.OK);
  const reply = await runtime.node('host-wire-reply').nextChunk({timeoutMs: 2000});
  assert.equal(reply.data, 'd29ybGQ=');
  await session.halfClose();
  assert.equal((await receive(null)).code, StatusCode.OK);
  assert.equal((await session.done({timeoutMs: 2000})).code, StatusCode.OK);
  await runtime.detachWireStream(host.id);

  const receiving = runtime.session('host-wire-receiving', {withRecv: true});
  await receiving.ready();
  let deliver;
  await receiving.addStream({...host, id: 'host-wire-receiving-stream',
    start(onMessage) {
      deliver = onMessage;
      return {code: StatusCode.OK, message: '', details: []};
    }});
  const inbound = {node_fragments: [{id: 'received-node',
    data: {data: 'cmVjZWl2ZWQ='}}]};
  const pendingReceive = receiving.receiveWithStreamId({timeoutMs: 2000});
  assert.equal((await deliver(inbound)).code, StatusCode.OK);
  assert.deepEqual(await pendingReceive, {message: inbound,
    streamId: 'host-wire-receiving-stream'});
  await receiving.halfClose();
  assert.equal((await deliver(null)).code, StatusCode.OK);
  await assert.rejects(receiving.receiveWithStreamId({timeoutMs: 20}),
    error => error.code === StatusCode.DEADLINE_EXCEEDED);
  await receiving.abort({code: StatusCode.CANCELLED,
    message: 'receiving session finished'});
  await runtime.detachWireStream('host-wire-receiving-stream');

  const failing = runtime.session('host-wire-failure-session');
  await failing.ready();
  let aborted;
  await failing.addStream({...host, id: 'host-wire-failure',
    send: () => ({code: StatusCode.INVALID_ARGUMENT,
      message: 'host rejected fragment', details: [{field: 'data'}]}),
    abort: status => { aborted = status; return {
      code: StatusCode.OK, message: '', details: []}; }});
  await failing.send({node_fragments: [{id: 'bad-node',
    data: {data: 'YQ=='}}]});
  for (let i = 0; i < 100; i++) {
    if (aborted) break;
    await new Promise(resolve => setTimeout(resolve, 10));
  }
  assert.equal(aborted.code, StatusCode.INVALID_ARGUMENT);
  assert.deepEqual(aborted.details, [{field: 'data'}]);
  await runtime.detachWireStream('host-wire-failure');

  let startupAborted;
  await assert.rejects(runtime.attachWireStream('missing-session',
    {...host, id: 'host-wire-startup-failure',
      abort: status => { startupAborted = status; return status; }}),
  error => error.code === StatusCode.NOT_FOUND);
  await Promise.resolve();
  assert.equal(startupAborted.code, StatusCode.NOT_FOUND);

  const early = runtime.session('host-wire-early-detach');
  await early.ready();
  let earlyAbort;
  await early.addStream({...host, id: 'host-wire-early',
    abort: status => { earlyAbort = status; return status; }});
  await runtime.detachWireStream('host-wire-early');
  assert.equal(earlyAbort.code, StatusCode.CANCELLED);

  const orphan = runtime.session('host-wire-dispose-session');
  await orphan.ready();
  let stopped;
  await orphan.addStream({...host, id: 'host-wire-dispose',
    abort: status => { stopped = status; return status; }});
  runtime.dispose();
  await Promise.resolve();
  assert.equal(stopped.code, StatusCode.CANCELLED);
} finally {
  runtime.dispose();
}
