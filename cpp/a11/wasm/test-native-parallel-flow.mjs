import assert from 'node:assert/strict';
import {createA11WasmRuntime} from './native-index.mjs';
import {StatusCode} from './status.mjs';

const runtime = await createA11WasmRuntime();
const source = `flow example {
  in id: string required
  out result: string
  called = run barrier(id: take id)
  called.result -> result
}`;
let started = 0;
let release;
const bothStarted = new Promise(resolve => {release = resolve;});
const signals = new Map();

runtime.registerAction({
  name: 'barrier',
  inputs: [{name: 'id', type: 'string', unary: true, required: true}],
  outputs: [{name: 'result', type: 'string', unary: true}],
}, async (action, context) => {
  const idInput = await action.input('id');
  const input = await idInput.nextChunk({timeoutMs: 2000});
  const id = JSON.parse(Buffer.from(input.data, 'base64').toString());
  signals.set(id, context.signal);
  if (++started === 2) release();
  await bothStarted;
  if (id === 'cancel') {
    await new Promise(resolve => context.signal.addEventListener('abort', resolve,
      {once: true}));
    return {code: StatusCode.CANCELLED, message: 'cancelled'};
  }
  const output = await action.output('result');
  await output.putChunk({
    data: Buffer.from(JSON.stringify(id)).toString('base64'),
    metadata: {mimetype: 'application/json'},
  });
  await output.finalize();
});

const chunk = value => ({
  data: Buffer.from(JSON.stringify(value)).toString('base64'),
  metadata: {mimetype: 'application/json'},
});

try {
  const controller = new AbortController();
  const cancelled = runtime.runFlow({source, flow: 'example',
    inputs: {id: [chunk('cancel')]}, timeoutMs: 5000},
  {signal: controller.signal});
  const successful = runtime.runFlow({source, flow: 'example',
    inputs: {id: [chunk('keep')]}, timeoutMs: 5000});
  await Promise.race([bothStarted,
    new Promise((_, reject) => setTimeout(() => reject(new Error(
      'Flow runs did not start in parallel')), 3000))]);
  controller.abort();
  await assert.rejects(cancelled, error => error.code === StatusCode.CANCELLED);
  const result = await successful;
  assert.equal(JSON.parse(Buffer.from(result.result[0].data, 'base64').toString()),
    'keep');
  assert.equal(signals.get('keep').aborted, false);
} finally {
  await runtime.dispose();
}
