import assert from 'node:assert/strict';
import { createA11WasmRuntime } from './native-index.mjs';
import {StatusCode} from './status.mjs';

const runtime = await createA11WasmRuntime();
try {
  const node = runtime.node('native-object-test');
  assert.deepEqual(await node.ready(), {id: 'native-object-test'});
  assert.deepEqual(await node.ref(), {id: 'native-object-test'});
  assert.deepEqual(await node.ref({offset: 1, length: 2,
    storage: 'include'}), {id: 'native-object-test', offset: 1,
    length: 2, storage: {id: 'local'}});
  await assert.rejects(node.ref({offset: 4294967295, length: 2}),
    error => error.code === StatusCode.INVALID_ARGUMENT);
  const waiting = node.nextChunk({timeoutMs: 2000});
  const chunk = {data: 'ImhpIg==', metadata: {mimetype: 'application/json'}};
  assert.deepEqual(await node.putChunk(chunk), {sequence: 0});
  assert.deepEqual(await waiting, chunk);
  await node.finalize();
  assert.equal(await node.nextChunk({timeoutMs: 2000}), null);
  const referenceNode = runtime.node('native-reference-test');
  const fragment = {id: referenceNode.id, data: {id: 'other-node',
    offset: 1, length: 2, storage: {id: 'remote-store'}}, seq: 0};
  assert.deepEqual(await referenceNode.putFragment(fragment), {sequence: 0});
  assert.deepEqual(await referenceNode.nextFragment({timeoutMs: 2000}),
    fragment);
  assert.equal(await referenceNode.nextFragment({timeoutMs: 2000}), null);
  const localReference = runtime.node('native-local-reference-test');
  const unqualified = {id: localReference.id,
    data: {id: 'other-node', length: 1}, seq: 0};
  await localReference.putFragment(unqualified);
  assert.deepEqual(await localReference.nextFragment({timeoutMs: 2000}),
    unqualified);
  const cancelledNode = runtime.node('native-cancel-test');
  const controller = new AbortController();
  const pending = cancelledNode.nextChunk({signal: controller.signal});
  controller.abort();
  await assert.rejects(pending,
    error => error.code === StatusCode.CANCELLED);
  await cancelledNode.cancel();
} finally {
  runtime.dispose();
}
