import { createA11WasmRuntime } from './native-index.mjs';

const runtime = await createA11WasmRuntime();
try {
  const result = await runtime.runFlow({
    source: 'flow hello {\n  out result: string\n  "hi" -> result\n}',
    flow: 'hello',
  });
  console.log(result);
} finally {
  runtime.dispose();
}
