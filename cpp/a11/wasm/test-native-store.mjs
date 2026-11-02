import assert from 'node:assert/strict';
import {createA11WasmRuntime} from './native-index.mjs';

const runtime = await createA11WasmRuntime();
try {
  const store = runtime.store('native-store-test');
  assert.equal(await store.ready(), 0);
  const pending = store.get(0, {timeoutMs: 1000});
  const fragment = {id: store.id, seq: 0, continued: true,
    data: {data: 'aGVsbG8=', metadata: {mimetype: 'text/plain'}}};
  assert.deepEqual(await store.putMany([fragment]), [0]);
  assert.deepEqual(await pending, fragment);
  assert.equal(await store.size(), 1);
  assert.equal(await store.finalSequence(), null);
  assert.deepEqual(await store.next(), [fragment]);
  assert.equal((await store.close()).code, 0);
  assert.deepEqual(await store.next(), [null]);
} finally {
  runtime.dispose();
}
