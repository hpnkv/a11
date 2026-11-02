import { createA11WasmRuntime } from './native-index.mjs';

const runtime = await createA11WasmRuntime();
const controller = new AbortController();
setTimeout(() => controller.abort(), 30);
try {
  const result = await Promise.race([
    runtime.runFlow({source: 'flow counting { out result: number\n repeat n = 0 max 100000000 {\n n <- n + 1\n }\n 1 -> result\n}',
      flow: 'counting', timeoutMs: 5000}, {signal: controller.signal}),
    new Promise((_, reject) => setTimeout(() => reject(new Error('cancel timed out')), 2000)),
  ]);
  console.log(result);
} catch (error) {
  console.log(error.code, error.message);
} finally {
  runtime.dispose();
}
