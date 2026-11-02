import {createDemoRuntime} from '../../../js/demo/wasm_runtime.ts';

const result = document.querySelector('#result');
try {
  const runtime = await createDemoRuntime();
  if (!crossOriginIsolated || runtime.backendFor('nodes') !== 'wasm') {
    throw new Error('Guide did not initialize an isolated WASM runtime');
  }
  await runtime.dispose();
  result.textContent = 'PASS';
} catch (error) {
  result.textContent = `FAIL: ${error?.message ?? error}`;
}
