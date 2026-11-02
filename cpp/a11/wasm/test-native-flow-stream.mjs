import assert from 'node:assert/strict';
import {createA11WasmRuntime} from './native-index.mjs';
import {StatusCode} from './status.mjs';

const runtime = await createA11WasmRuntime();
try {
  runtime.registerAction({
    name: 'two_parts',
    outputs: [{name: 'result', type: 'string', unary: false}],
  }, async action => {
    const output = await action.output('result');
    await output.putChunk({data: Buffer.from('first').toString('base64'),
      metadata: {mimetype: 'text/plain'}});
    await new Promise(resolve => setTimeout(resolve, 150));
    await output.putChunk({data: Buffer.from('second').toString('base64'),
      metadata: {mimetype: 'text/plain'}});
    await output.finalize();
  });
  const observed = [];
  let firstOutput;
  const first = new Promise(resolve => { firstOutput = resolve; });
  let finished = false;
  const running = runtime.runFlow({source: `flow streaming {
    out result: string stream
    called = run two_parts()
    called.result -> result
  }`, flow: 'streaming'}, {onOutput(port, chunk) {
    observed.push([port, Buffer.from(chunk.data, 'base64').toString()]);
    firstOutput();
  }}).finally(() => { finished = true; });
  await first;
  assert.equal(finished, false);
  const output = await running;
  assert.deepEqual(observed, [['result', 'first'], ['result', 'second']]);
  assert.deepEqual(output.result.map(chunk =>
    Buffer.from(chunk.data, 'base64').toString()), ['first', 'second']);
  await assert.rejects(runtime.runFlow({source: `flow limited {
    out result: string stream
    called = run two_parts()
    called.result -> result
  }`, flow: 'limited', maxOutputChunks: 1}),
  error => error.code === StatusCode.RESOURCE_EXHAUSTED);
} finally {
  runtime.dispose();
}
