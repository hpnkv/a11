import assert from 'node:assert/strict';
import {createA11WasmRuntime} from './native-index.mjs';
import {StatusCode} from './status.mjs';

const runtime = await createA11WasmRuntime();
const source = `flow host_action {
  out result: string
  called = run echo(text: "hello")
  called.result -> result
}`;
let called = 0;
runtime.registerAction({
  name: 'echo',
  inputs: [{name: 'text', type: 'string', required: true, unary: true}],
  outputs: [{name: 'result', type: 'string', unary: true}],
}, async action => {
  ++called;
  const input = await action.input('text');
  const output = await action.output('result');
  const chunk = await input.nextChunk({timeoutMs: 2000});
  assert.equal(JSON.parse(Buffer.from(chunk.data, 'base64').toString()), 'hello');
  await output.putChunk({data: Buffer.from(JSON.stringify('received')).toString('base64'),
    metadata: {mimetype: 'application/json'}});
  await output.finalize();
});
try {
  const output = await runtime.runFlow({source, flow: 'host_action'});
  assert.equal(called, 1);
  assert.equal(JSON.parse(Buffer.from(output.result[0].data, 'base64').toString()),
    'received');
  runtime.registerAction({
    name: 'fail_host',
    outputs: [{name: 'result', type: 'string'}],
  }, async () => ({code: StatusCode.INVALID_ARGUMENT,
    message: 'host rejected the call',
    details: [{field: 'input'}]}));
  const failingSource = `flow failing {
    out result: string
    called = run fail_host()
    called.result -> result
  }`;
  await assert.rejects(runtime.runFlow({source: failingSource, flow: 'failing'}),
    error => error.code === StatusCode.INVALID_ARGUMENT &&
      /host rejected the call/.test(error.message) &&
      error.details?.[0]?.field === 'input');
  let entered;
  const started = new Promise(resolve => { entered = resolve; });
  let callbackSignal;
  runtime.registerAction({name: 'waiting_host',
    outputs: [{name: 'result', type: 'string'}]},
  async (_action, context) => {
    callbackSignal = context.signal;
    entered();
    await new Promise(resolve => context.signal.addEventListener('abort',
      resolve, {once: true}));
    return {code: StatusCode.CANCELLED, message: 'cancelled'};
  });
  const controller = new AbortController();
  const waiting = runtime.runFlow({source: `flow waiting {
    out result: string
    called = run waiting_host()
    called.result -> result
  }`, flow: 'waiting'}, {signal: controller.signal});
  await started;
  controller.abort();
  await assert.rejects(waiting,
    error => error.code === StatusCode.CANCELLED);
  assert.equal(callbackSignal.aborted, true);
} finally {
  runtime.dispose();
}
