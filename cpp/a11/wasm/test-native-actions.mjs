import assert from 'node:assert/strict';
import { createA11WasmRuntime } from './native-index.mjs';
import {StatusCode} from './status.mjs';

const runtime = await createA11WasmRuntime();
try {
  const session = runtime.session('native-session-test', {
    maxConcurrentRootActions: 2, noStreamTimeoutMs: null,
  });
  await session.ready();
  await assert.rejects(runtime.session('invalid-session-limits', {
    maxConcurrentRootActions: 0,
  }).ready(), error => error.code === StatusCode.INVALID_ARGUMENT);
  assert.equal((await session.state()).status.code, StatusCode.OK);
  const action = session.action('sleep_for', 'native-sleep-test');
  await action.ready();
  await action.setHeader('x-a11-test', 'aGVsbG8=');
  assert.equal((await action.headers())['x-a11-test'], 'aGVsbG8=');
  await action.removeHeader('x-a11-test');
  assert.equal((await action.headers())['x-a11-test'], undefined);
  assert.equal((await action.state()).run, false);
  const input = await action.input('duration');
  const output = await action.output('woke');
  await action.run();
  assert.equal((await action.state()).run, true);
  const data = Buffer.from(JSON.stringify('10ms')).toString('base64');
  await input.putChunk({data, metadata: {mimetype: 'application/json'}});
  await input.finalize();
  await action.wait({timeoutMs: 2000});
  assert.ok(await output.nextChunk({timeoutMs: 2000}));
  assert.equal(await output.nextChunk({timeoutMs: 2000}), null);
  await session.cancelActions();
  assert.equal((await session.state()).status.code, StatusCode.OK);
  await assert.rejects(session.done({timeoutMs: 1}),
    error => error.code === StatusCode.DEADLINE_EXCEEDED);

  const unregister = await runtime.registerRunnableAction({
    name: 'echo',
    inputs: [{name: 'text', type: 'string', required: true, unary: true}],
    outputs: [{name: 'result', type: 'string', unary: true}],
  }, async called => {
    const source = await called.input('text');
    const target = await called.output('result');
    const chunk = await source.nextChunk({timeoutMs: 2000});
    await target.putChunk(chunk);
    await target.finalize();
  });
  const echo = runtime.action('echo', 'native-host-action-test');
  await echo.ready();
  const echoInput = await echo.input('text');
  const echoOutput = await echo.output('result');
  await echo.run();
  const expected = Buffer.from(JSON.stringify('hello')).toString('base64');
  await echoInput.putChunk({data: expected,
    metadata: {mimetype: 'application/json'}});
  await echoInput.finalize();
  await echo.wait({timeoutMs: 2000});
  assert.equal((await echoOutput.nextChunk({timeoutMs: 2000})).data, expected);
  await unregister();
  await assert.rejects(runtime.action('echo', 'unregistered').ready(),
    error => error.code === StatusCode.NOT_FOUND);

  const unregisterFailure = await runtime.registerRunnableAction({
    name: 'reject', outputs: [],
  }, async () => ({code: StatusCode.INVALID_ARGUMENT, message: 'invalid input',
    details: [{field: 'text'}]}));
  const rejected = runtime.action('reject', 'native-rejected-action');
  await rejected.ready();
  await rejected.run();
  await assert.rejects(rejected.wait({timeoutMs: 2000}),
    error => error.code === StatusCode.INVALID_ARGUMENT &&
      error.details?.[0]?.field === 'text');
  await unregisterFailure();

  let markStarted;
  const started = new Promise(resolve => { markStarted = resolve; });
  const unregisterWaiting = await runtime.registerRunnableAction({
    name: 'waiting', outputs: [],
  }, async (_called, {signal}) => {
    markStarted();
    await new Promise(resolve => signal.addEventListener('abort', resolve,
      {once: true}));
    return {code: StatusCode.CANCELLED, message: 'cancelled by caller'};
  });
  const waiting = runtime.action('waiting', 'native-cancelled-action');
  await waiting.ready();
  await waiting.run();
  await started;
  await waiting.cancel();
  await assert.rejects(waiting.wait({timeoutMs: 2000}),
    error => error.code === StatusCode.CANCELLED);
  await unregisterWaiting();

  let markSessionStarted;
  const sessionStarted = new Promise(resolve => {
    markSessionStarted = resolve;
  });
  let sessionHandlerCancelled = false;
  const unregisterSessionAction = await runtime.registerRunnableAction({
    name: 'session_waiting', outputs: [],
  }, async (_called, {signal}) => {
    markSessionStarted();
    await new Promise(resolve => signal.addEventListener('abort', resolve,
      {once: true}));
    sessionHandlerCancelled = true;
    return {code: StatusCode.CANCELLED, message: 'session cancelled'};
  });
  const cancellingSession = runtime.session('native-cancelling-session');
  await cancellingSession.ready();
  const sessionAction = cancellingSession.action('session_waiting',
    'native-session-action');
  await sessionAction.ready();
  await sessionAction.run();
  await sessionStarted;
  await cancellingSession.cancelActions();
  await assert.rejects(sessionAction.wait({timeoutMs: 2000}),
    error => error.code === StatusCode.CANCELLED);
  assert.equal(sessionHandlerCancelled, true);
  await unregisterSessionAction();
} finally {
  runtime.dispose();
}
