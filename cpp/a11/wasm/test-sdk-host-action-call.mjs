import assert from 'node:assert/strict';
import {createA11WasmRuntime} from './native-index.mjs';
import {ActionPortSchema, ActionRegistry, ActionSchema, InProcessWireStream,
  Session, StatusCode, StreamMode, createA11Runtime, isOk, okStatus} from
  '../../../js/dist/index.js';
import {bridgeWireStream} from
  '../../../js/dist/internal/wasm/wire_stream_bridge.js';

const schema = new ActionSchema({name: 'remote_echo',
  inputs: {input: new ActionPortSchema({name: 'input', type: 'string'})},
  outputs: {output: new ActionPortSchema({name: 'output', type: 'string'})},
});
const serverRuntime = await createA11Runtime();
assert.ok(isOk(serverRuntime));
const registry = new ActionRegistry(serverRuntime);
assert.ok(isOk(await registry.register('remote_echo', schema, async action => {
  const input = await action.getInput('input');
  if (!isOk(input)) return input;
  const value = await input.next(2000);
  if (!isOk(value)) return value;
  const output = await action.getOutput('output');
  if (!isOk(output)) return output;
  const ended = await output.finalize(`reply:${value}`, {wait: true});
  return isOk(ended) ? okStatus() : ended;
})));
const server = await Session.create(serverRuntime, {id: 'sdk-action-server',
  noStreamTimeoutMs: null});
assert.ok(isOk(server));
const pair = InProcessWireStream.createPair();
assert.ok(isOk(pair));
const runtime = await createA11WasmRuntime();
let stage = 'setup';
try {
  const nativeSession = runtime.session('sdk-action-client');
  await nativeSession.ready();
  const unregister = await runtime.registerRunnableAction({
    name: 'remote_echo',
    inputs: [{name: 'input', type: 'string'}],
    outputs: [{name: 'output', type: 'string'}],
  }, async () => ({code: StatusCode.UNIMPLEMENTED,
    message: 'remote call used local handler'}));
  const [accepted, clientStreamId] = await Promise.all([
    server.addStream(pair[1], StreamMode.ACCEPT),
    nativeSession.addStream(bridgeWireStream(pair[0])),
  ]);
  assert.ok(isOk(accepted));
  stage = 'call';
  const action = nativeSession.action('remote_echo', 'sdk-native-remote-call',
    clientStreamId);
  await action.ready();
  const input = await action.input('input');
  const output = await action.output('output');
  await action.call();
  stage = 'input';
  await input.putChunk({data: Buffer.from(JSON.stringify('request')).toString('base64'),
    metadata: {mimetype: 'application/json'}});
  await input.finalize();
  stage = 'wait';
  await action.wait({timeoutMs: 2000});
  stage = 'output';
  assert.equal((await action.state()).status.code, 0);
  let chunk;
  try { chunk = await output.nextChunk({timeoutMs: 2000}); }
  catch (error) {
    throw new Error(JSON.stringify({action: await action.state(),
      nativeSession: await nativeSession.state(), server: await server.getStatus()}) +
      ': ' + error.message);
  }
  assert.equal(Buffer.from(chunk.data, 'base64').toString(), 'reply:request');
  assert.equal((await action.state()).status.code, 0);
  await nativeSession.halfClose();
  assert.ok(isOk(await server.halfClose()));
  assert.equal((await nativeSession.done({timeoutMs: 2000})).code, 0);
  await runtime.detachWireStream(clientStreamId);

  const ambiguous = runtime.session('sdk-action-ambiguous');
  await ambiguous.ready();
  const hostStream = id => ({id,
    start: () => okStatus(),
    send: () => okStatus(),
    halfClose: () => okStatus(),
    abort: () => okStatus(),
    getStatus: () => okStatus(),
  });
  await ambiguous.addStream(hostStream('sdk-action-first'));
  await ambiguous.addStream(hostStream('sdk-action-second'));
  const unspecified = ambiguous.action('remote_echo', 'sdk-action-unspecified');
  await unspecified.ready();
  await assert.rejects(unspecified.call(),
    error => error.code === StatusCode.FAILED_PRECONDITION);
  await runtime.detachWireStream('sdk-action-first');
  await runtime.detachWireStream('sdk-action-second');
  await unregister();
} catch (error) {
  const state = await runtime.action('remote_echo',
    'sdk-native-remote-call').state().catch(() => null);
  throw new Error(`${stage}: ${error.stack ?? error}; action: ${JSON.stringify(state)}`);
} finally {
  await Promise.all([runtime.dispose(), serverRuntime.dispose()]);
}
