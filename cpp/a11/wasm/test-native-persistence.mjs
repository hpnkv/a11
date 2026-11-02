import assert from 'node:assert/strict';
import {createA11WasmRuntime} from './native-index.mjs';
import {StatusCode} from './status.mjs';

const source = await createA11WasmRuntime({storageId: 'browser-local'});
let bytes;
try {
  const node = source.node('persisted-node');
  await node.putChunk({data: 'aGVsbG8=',
    metadata: {mimetype: 'text/plain'}});
  await node.finalize();
  const ref = source.node('persisted-reference');
  await ref.putFragment({id: ref.id, data: {id: node.id, length: 1,
    storage: {id: 'remote-store'}}, seq: 0});
  bytes = (await source.command({operation: 'snapshot_export',
    storageId: 'browser-local'})).data;
  assert.ok(bytes.length > 0);
} finally {
  source.dispose();
}

const restored = await createA11WasmRuntime({storageId: 'browser-local'});
try {
  await assert.rejects(restored.command({operation: 'snapshot_import',
    storageId: 'another-store', data: bytes}),
  error => error.code === StatusCode.INVALID_ARGUMENT);
  await restored.command({operation: 'snapshot_import',
    storageId: 'browser-local', data: bytes});
  assert.equal((await restored.node('persisted-node').nextChunk({
    timeoutMs: 2000})).data, 'aGVsbG8=');
  const fragment = await restored.node('persisted-reference').nextFragment({
    timeoutMs: 2000});
  assert.deepEqual(fragment.data, {id: 'persisted-node', length: 1,
    storage: {id: 'remote-store'}});
  assert.equal(await restored.store('persisted-node').finalSequence(), 1);
  assert.equal(await restored.store('persisted-node').size(), 2);
  await assert.rejects(restored.command({operation: 'snapshot_import',
    storageId: 'browser-local', data: bytes}),
  error => error.code === StatusCode.FAILED_PRECONDITION);
} finally {
  restored.dispose();
}

const active = await createA11WasmRuntime({storageId: 'browser-local'});
try {
  await active.session('active-session').ready();
  await assert.rejects(active.command({operation: 'snapshot_export',
    storageId: 'browser-local'}),
  error => error.code === StatusCode.FAILED_PRECONDITION);
} finally {
  active.dispose();
}

let saved = null;
const persistence = {
  load: async () => saved,
  save: async bytes => { saved = bytes; },
};
const writer = await createA11WasmRuntime({storageId: 'durable-local',
  persistence});
try {
  assert.deepEqual(await writer.node('durable-node').ref({storage: 'include'}),
    {id: 'durable-node', storage: {id: 'durable-local'}});
  await writer.node('durable-node').putChunk({data: 'c3RvcmVk',
    metadata: {mimetype: 'text/plain'}});
  await writer.node('durable-node').finalize();
  await writer.flushPersistence();
  assert.ok(saved instanceof Uint8Array && saved.length > 0);
} finally {
  writer.dispose();
}
const reader = await createA11WasmRuntime({storageId: 'durable-local',
  persistence});
try {
  assert.deepEqual(await reader.node('durable-node').ref({storage: 'include'}),
    {id: 'durable-node', storage: {id: 'durable-local'}});
  assert.equal((await reader.node('durable-node').nextChunk({
    timeoutMs: 2000})).data, 'c3RvcmVk');
} finally {
  reader.dispose();
}
