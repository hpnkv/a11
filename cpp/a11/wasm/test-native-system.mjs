import assert from 'node:assert/strict';
import {createA11WasmRuntime} from './native-index.mjs';
import {StatusCode} from './status.mjs';

const source = `flow system {
  out ids: string stream
  out values: object
  out bytes: bytes stream
  generated = run new_uuid(options: {count: 2})
  generated.ids -> ids
  read = run env_get(names: ["A11_WASM_TEST"])
  read.values -> values
  random = run random_bytes(options: {count: 16})
  random.bytes -> bytes
}`;

const runtime = await createA11WasmRuntime({
  environment: {A11_WASM_TEST: 'present'},
  allowedEnvironmentNames: ['A11_WASM_TEST'],
});
try {
  const output = await runtime.runFlow({source, flow: 'system'});
  assert.equal(output.ids.length, 2);
  assert.equal(output.values.length, 1);
  assert.equal(output.bytes.length, 1);
  assert.equal(JSON.parse(Buffer.from(output.values[0].data, 'base64').toString())
    .A11_WASM_TEST, 'present');
  const direct = runtime.action('env_get', 'direct-env-get');
  await direct.ready();
  const names = await direct.input('names');
  const options = await direct.input('options');
  const values = await direct.output('values');
  await direct.run();
  await names.putChunk({data: Buffer.from(JSON.stringify(['A11_WASM_TEST']))
    .toString('base64'), metadata: {mimetype: 'application/json'}});
  await names.finalize();
  await options.finalize();
  try {
    await direct.wait({timeoutMs: 2000});
  } catch (error) {
    const diagnostic = await runtime.command({operation: 'fiber_report'});
    throw new Error(diagnostic.report, {cause: error});
  }
  assert.equal(JSON.parse(Buffer.from((await values.nextChunk({timeoutMs: 2000}))
    .data, 'base64').toString()).A11_WASM_TEST, 'present');
  await runtime.action('make_http_request', 'direct-http').ready();
  await runtime.action('web-fetch', 'direct-fetch').ready();
} finally {
  runtime.dispose();
}

const disabled = await createA11WasmRuntime({actions: {env_get: false,
  make_http_request: false, 'web-fetch': false}});
try {
  await assert.rejects(disabled.runFlow({source, flow: 'system'}),
    error => error.code === StatusCode.NOT_FOUND);
  for (const name of ['env_get', 'make_http_request', 'web-fetch']) {
    await assert.rejects(disabled.action(name, `disabled-${name}`).ready(),
      error => error.code === StatusCode.NOT_FOUND);
  }
} finally {
  disabled.dispose();
}
