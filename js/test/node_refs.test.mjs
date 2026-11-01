import assert from 'node:assert/strict';
import test from 'node:test';

import {
  Chunk, ChunkStoreReader, NodeMap, NodeRef, StatusCode, isOk, okStatus,
} from '../dist/index.js';
import {ReferenceMirrorRegistry, REFERENCE_MIRROR_PREFIX} from '../dist/reference_mirror.js';

const need = value => { assert.ok(isOk(value), value.message); return value; };
const chunk = text => new Chunk({data: new TextEncoder().encode(text)});
const textOf = value => new TextDecoder().decode(value.data);

test('reference mirror replays stored fragments and follows later writes', async () => {
  const source = new NodeMap();
  const target = need(await source.get('target'));
  need(await target.putChunk(chunk('before')));
  const fragments = [];
  let finished;
  const done = new Promise(resolve => { finished = resolve; });
  const stream = {
    getId: () => 'peer',
    send(message) {
      fragments.push(...message.nodeFragments);
      if (message.nodeFragments.some(fragment => !fragment.continued)) finished();
      return okStatus();
    },
  };
  const mirrors = new ReferenceMirrorRegistry(stream, []);
  const reference = new NodeRef({id: 'target', offset: 1, length: 1});
  const [first, duplicate] = await Promise.all([
    mirrors.rewrite(reference, source), mirrors.rewrite(reference, source),
  ]);
  const forwarded = need(first);
  assert.ok(forwarded.id.startsWith(REFERENCE_MIRROR_PREFIX));
  assert.equal(forwarded.offset, 1);
  assert.equal(forwarded.length, 1);
  assert.equal(need(duplicate).id, forwarded.id);
  assert.equal(need(await mirrors.rewrite(reference, source)).id, forwarded.id);
  need(await target.finalize(chunk('after')));
  await done;
  assert.deepEqual(fragments.map(fragment => textOf(fragment.data)), ['before', 'after']);
});

test('reference mirror leaves directly readable storage untouched', async () => {
  const sent = [];
  const mirrors = new ReferenceMirrorRegistry({
    getId: () => 'peer',
    send(message) { sent.push(message); return okStatus(); },
  }, ['shared']);
  const reference = new NodeRef({id: 'target', storage: {id: 'shared'}});
  assert.equal(need(await mirrors.rewrite(reference, new NodeMap())), reference);
  assert.equal(sent.length, 0);
});

test('mirror reuse checks each reference with the host resolver', async () => {
  const backend = new NodeMap(undefined, {id: 'secured'});
  const target = need(await backend.get('target'));
  need(await target.finalize(chunk('value')));
  const host = new NodeMap();
  need(host.setStorageResolver(identity =>
    identity.credentials?.fields.token === 'ok'
      ? backend : {code: StatusCode.PERMISSION_DENIED, message: 'Access denied.'}));
  const mirrors = new ReferenceMirrorRegistry({
    getId: () => 'peer', send: () => okStatus(),
  }, []);
  const allowed = new NodeRef({
    id: 'target', storage: {id: 'secured', credentials: {scheme: 'bearer', fields: {token: 'ok'}}},
  });
  assert.ok(need(await mirrors.rewrite(allowed, host)).id.startsWith(REFERENCE_MIRROR_PREFIX));
  const denied = new NodeRef({
    id: 'target', storage: {id: 'secured', credentials: {scheme: 'bearer', fields: {token: 'denied'}}},
  });
  assert.equal((await mirrors.rewrite(denied, host)).code, StatusCode.PERMISSION_DENIED);
});

test('logical reads expand fragment windows while raw reads retain references', async () => {
  const map = new NodeMap();
  const target = need(await map.get('target'));
  const destination = need(await map.get('destination'));
  need(await target.putChunk(chunk('one')));
  need(await target.putChunk(chunk('two')));
  need(await target.putChunk(chunk('three'), null, true));
  const reference = need(await target.ref({offset: 1, length: 2, forNode: destination}));
  need(await destination.putChunk(chunk('before')));
  need(await destination.put(reference));
  need(await destination.finalize(chunk('after')));
  assert.equal(textOf(need(await destination.nextChunk())), 'before');
  assert.equal(textOf(need(await destination.nextChunk())), 'two');
  assert.equal(textOf(need(await destination.nextChunk())), 'three');
  assert.equal(textOf(need(await destination.nextChunk())), 'after');
  assert.equal(await destination.nextChunk(), null);
  assert.equal(textOf(need((await destination.nextFragment()).getChunk())), 'before');
  assert.ok((await destination.nextFragment()).data instanceof NodeRef);
});

test('reference credentials are typed, copied, and redacted', () => {
  const original = new NodeRef({id: 'target', storage: {id: 'shared', services: [{role: 'payload'}]}});
  const qualified = need(original.withCredentials({scheme: 'bearer', fields: {token: 'namespace-secret'}}));
  const service = need(qualified.withCredentials({scheme: 'bearer', fields: {token: 'service-secret'}}, 'payload'));
  assert.equal(original.storage.credentials, undefined);
  assert.equal(qualified.storage.credentials.fields.token, 'namespace-secret');
  assert.equal(service.storage.services[0].credentials.fields.token, 'service-secret');
  assert.equal(need(NodeRef.fromMsgpack(need(service.toMsgpack()))).storage.services[0].credentials.fields.token, 'service-secret');
  assert.ok(!String(service).includes('namespace-secret'));
  assert.equal(new NodeRef({id: 'local'}).withCredentials({scheme: 'bearer'}).code, StatusCode.FAILED_PRECONDITION);
});

test('read resolution opens uncached targets only when the host allows it', async () => {
  const backend = new NodeMap(undefined, {id: 'durable'});
  const host = new NodeMap();
  need(host.registerStorage('durable', backend));
  const reference = new NodeRef({id: 'uncached', storage: {id: 'durable'}});
  assert.equal((await host.resolveForRead(reference)).code, StatusCode.NOT_FOUND);
  need(backend.setAllowLazyReadResolution(true));
  assert.equal(need(await host.resolveForRead(reference)), need(await backend.get('uncached')));
});

test('a timed-out resolver resumes at the same reference', async () => {
  const map = new NodeMap();
  const target = need(await map.get('target'));
  const source = need(await map.get('source'));
  need(await target.finalize(chunk('retained')));
  need(await source.finalize(need(await target.ref({forNode: source}))));
  const reader = need(ChunkStoreReader.create(source.chunkStore));
  let complete;
  let calls = 0;
  const resolver = () => {
    ++calls;
    return new Promise(resolve => { complete = resolve; });
  };
  const timedOut = await reader.nextLogical(resolver, 5);
  assert.equal(timedOut.code, StatusCode.DEADLINE_EXCEEDED);
  complete({store: target.chunkStore, resolver});
  const value = need(await reader.nextLogical(resolver, 100));
  assert.equal(textOf(value.data), 'retained');
  assert.equal(await reader.nextLogical(resolver, 100), null);
  assert.equal(calls, 1);
});

test('cancelling a pending resolver ends the logical read', async () => {
  const map = new NodeMap();
  const target = need(await map.get('target'));
  const source = need(await map.get('source'));
  need(await source.finalize(need(await target.ref({forNode: source}))));
  const reader = need(ChunkStoreReader.create(source.chunkStore));
  let entered;
  const resolving = new Promise(resolve => { entered = resolve; });
  const pending = reader.nextLogical(() => {
    entered();
    return new Promise(() => {});
  });
  await resolving;
  reader.cancel();
  assert.equal((await pending).code, StatusCode.ABORTED);
});
