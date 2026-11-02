/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import {
  A11Runtime, ActionHeaderSchema, ActionPortSchema, ActionRegistry,
  ActionSchema, Chunk, ChunkMetadata, InProcessWireStream,
  GraphAction, GraphNode, GraphSession, GraphSessionWithRecv,
  RuntimeGraph,
  NodeFragment, NodeRef,
  Session, StatusCode, StreamMode, WireMessage,
  createA11Runtime, isOk, okStatus, wireMessageFromJsonValue,
  wireMessageToJsonValue,
  LocalChunkStore as NativeLocalChunkStore,
} from '../dist/index.js';

const encoded = new TextEncoder();
const wasmUrl = new URL('../../cpp/a11/wasm/build/dist/index.mjs', import.meta.url);
const nativeUrl = new URL('../../cpp/a11/wasm/build/dist/native.mjs', import.meta.url);
const builtWasm = existsSync(fileURLToPath(wasmUrl));
const builtNative = existsSync(fileURLToPath(nativeUrl));

function options() {
  return {
    backend: 'wasm',
    wasm: {moduleUrl: wasmUrl, flowRuntimeUrl: nativeUrl},
  };
}

async function storeScenario(runtime) {
  const store = runtime.createChunkStore('stream');
  assert.equal(isOk(store), true);
  assert.ok(store instanceof NativeLocalChunkStore);
  assert.equal((await store.get(0, Date.now() + 5)).code,
    StatusCode.DEADLINE_EXCEEDED);
  const pending = store.get(0, Date.now() + 1000);
  const fragment = new NodeFragment({id: 'stream',
    data: new Chunk({data: encoded.encode('hello')}), continued: false});
  assert.equal(await store.put(fragment), 0);
  const arrived = await pending;
  assert.equal(isOk(arrived), true);
  assert.ok(arrived instanceof NodeFragment);
  assert.deepEqual(arrived.data.data, encoded.encode('hello'));
  assert.deepEqual((await store.next(Date.now() + 1000)).map(value => value?.seq), [0, undefined]);
  assert.equal(isOk(await store.closeWritesWithStatus(okStatus())), true);
  assert.equal((await store.get(1, Date.now() + 100)).code, StatusCode.NOT_FOUND);
  return store;
}

test('runtime rejects the retired TypeScript backend', async () => {
  assert.equal((await createA11Runtime({backend: 'typescript'})).code,
    StatusCode.UNIMPLEMENTED);
  assert.equal((await createA11Runtime({features: {
    nodes: 'typescript'}})).code, StatusCode.UNIMPLEMENTED);
  const qualified = new WireMessage({nodeFragments: [new NodeFragment({
    id: 'alias', data: new NodeRef({id: 'source', storage: {id: 'archive'}}),
  })]});
  const json = wireMessageToJsonValue(qualified);
  assert.equal(isOk(json), true);
  const roundtrip = wireMessageFromJsonValue(json);
  assert.equal(isOk(roundtrip), true);
  assert.equal(roundtrip.nodeFragments[0].data.storage.id, 'archive');
});

test('WASM codecs, Flow tooling, fragment log, and Flow execution run natively',
  {skip: !builtWasm || !builtNative}, async t => {
  const runtime = await createA11Runtime(options());
  t.after(async () => { await runtime.dispose(); });
  assert.ok(runtime instanceof A11Runtime);
  assert.equal(runtime.backendFor('chunkStore'), 'wasm');
  assert.equal(runtime.backendFor('actions'), 'wasm');
  const store = await storeScenario(runtime);
  assert.equal(runtime.backendOf(store), 'wasm');

  const reference = new NodeRef({id: 'stream', storage: {id: 'archive'}});
  const wire = new WireMessage({nodeFragments: [new NodeFragment({
    id: 'alias', data: reference, seq: 0, continued: false,
  })]});
  const bytes = runtime.encodeWire(wire);
  assert.equal(isOk(bytes), true);
  const decoded = runtime.decodeWire(bytes);
  assert.equal(isOk(decoded), true);
  assert.ok(decoded instanceof WireMessage);
  assert.ok(decoded.nodeFragments[0].data instanceof NodeRef);
  assert.equal(decoded.nodeFragments[0].data.storage.id, 'archive');

  const checked = runtime.flowTool({method: 'check', source: 'flow hello { }'});
  assert.equal(isOk(checked), true);
  assert.equal(checked.ok, true);
  const emitted = [];
  const run = await runtime.runFlow({
    source: 'flow hello {\n out result: string\n "hi" -> result\n}',
    flow: 'hello',
  }, {onOutput: (port, chunk) => emitted.push([port, chunk])});
  assert.equal(isOk(run), true);
  assert.equal(run.result.length, 1);
  assert.equal(emitted.length, 1);
  assert.equal(emitted[0][0], 'result');
  const aborted = new AbortController();
  aborted.abort();
  assert.equal((await runtime.runFlow({source: '', flow: 'hello'},
    {signal: aborted.signal})).code, StatusCode.CANCELLED);
  const active = new AbortController();
  const waiting = runtime.runFlow({
    source: 'flow counting { out result: number\n repeat n = 0 max 100000000 {\n n <- n + 1\n }\n 1 -> result\n}',
    flow: 'counting', timeoutMs: 5000,
  }, {signal: active.signal});
  setTimeout(() => active.abort(), 30);
  assert.equal((await waiting).code, StatusCode.CANCELLED);
  await runtime.dispose();
});

test('runtime creates native Action and node objects',
  {skip: !builtNative}, async () => {
  const runtime = await createA11Runtime(options());
  assert.ok(runtime instanceof A11Runtime);
  const node = await runtime.createNode('standalone');
  assert.ok(node instanceof GraphNode);
  assert.equal(runtime.backendOf(node), 'wasm');

  const schema = new ActionSchema({name: 'greet', outputs: {
    result: new ActionPortSchema({name: 'result', type: 'string'}),
  }});
  const unregister = await runtime.registerAction(schema, async running => {
    const output = await running.getOutput('result');
    if (!isOk(output)) return output;
    const ended = await output.finalize('hello', {wait: true});
    return isOk(ended) ? okStatus() : ended;
  });
  assert.equal(isOk(unregister), true);
  const standaloneSchema = new ActionSchema({name: 'standalone_remote',
    inputs: {request: new ActionPortSchema({name: 'request', type: 'string'})}});
  const standalone = await runtime.createAction(standaloneSchema);
  assert.equal(isOk(standalone), true);
  assert.equal((await standalone.getSchema()).name, 'standalone_remote');
  assert.equal(isOk(await standalone.getInput('request', false)), true);
  const invalidAction = await runtime.createAction(schema, {id: 'greet-settings',
    settings: {clearInputsAfterRun: 'invalid'}});
  assert.equal(invalidAction.code, StatusCode.INVALID_ARGUMENT);
  const recoveredAction = await runtime.createAction(schema, {
    id: 'greet-settings', settings: {clearInputsAfterRun: false}});
  assert.equal(isOk(recoveredAction), true);
  assert.equal((await recoveredAction.release()).code,
    StatusCode.FAILED_PRECONDITION);
  const action = await runtime.createAction(schema, {settings: {
    clearInputsAfterRun: true,
  }});
  assert.ok(action instanceof GraphAction);
  assert.equal(runtime.backendOf(action), 'wasm');
  const nativeSchema = await action.getSchema();
  assert.ok(nativeSchema instanceof ActionSchema);
  assert.equal(nativeSchema.name, 'greet');
  assert.equal(nativeSchema.outputs.get('result').type, 'string');
  assert.equal((await action.getSettings()).clearInputsAfterRun, true);
  assert.equal(isOk(await action.setSettings({clearInputsAfterRun: false})), true);
  assert.equal((await action.getSettings()).clearInputsAfterRun, false);
  assert.equal(await action.containsPort('result'), true);
  assert.equal(await action.containsPort('missing'), false);
  assert.equal(isOk(await action.getPort('result')), true);
  assert.equal(isOk(await action.getOutput('result', false)), true);
  assert.equal(isOk(await action.getLogNode()), true);
  assert.equal(await action.hasHeader('x-test'), false);
  assert.equal(await action.getHeader('x-test'), null);
  assert.equal(isOk(await action.setHeader('x-test', 'hello')), true);
  assert.equal(await action.hasHeader('x-test'), true);
  assert.deepEqual(await action.getHeader('x-test'), encoded.encode('hello'));
  assert.equal(isOk(await action.run()), true);
  assert.equal(isOk(await action.wait(1000)), true);
  const output = await action.getOutput('result');
  assert.equal(isOk(output), true);
  assert.equal(await output.next(1000), 'hello');
  assert.equal(isOk(await action.release()), true);
  assert.equal((await action.getStatus()).code, StatusCode.NOT_FOUND);
  assert.equal(isOk(await unregister()), true);
  await runtime.dispose();
});

test('native Action creates nested registered actions',
  {skip: !builtNative}, async () => {
  const runtime = await createA11Runtime(options());
  assert.ok(runtime instanceof A11Runtime);
  try {
    const childSchema = new ActionSchema({name: 'nested_child', inputs: {
      value: new ActionPortSchema({name: 'value', type: 'string'}),
    }, outputs: {
      result: new ActionPortSchema({name: 'result', type: 'string'}),
    }});
    const unregisterChild = await runtime.registerAction(childSchema,
      async child => {
        const input = await child.getInput('value');
        if (!isOk(input)) return input;
        const value = await input.next(2000);
        if (!isOk(value)) return value;
        const output = await child.getOutput('result');
        return isOk(output) ? output.finalize(`child:${value}`) : output;
      });
    assert.equal(isOk(unregisterChild), true);
    const parentSchema = new ActionSchema({name: 'nested_parent', outputs: {
      result: new ActionPortSchema({name: 'result', type: 'string'}),
    }});
    const unregisterParent = await runtime.registerAction(parentSchema,
      async parent => {
        const child = await parent.makeNested('nested_child');
        if (!isOk(child)) return child;
        const input = await child.getInput('value');
        if (!isOk(input)) return input;
        const output = await child.getOutput('result');
        if (!isOk(output)) return output;
        const started = await child.run();
        if (!isOk(started)) return started;
        const written = await input.finalize('hello');
        if (!isOk(written)) return written;
        const completed = await child.wait(2000);
        if (!isOk(completed)) return completed;
        const value = await output.next(2000);
        if (!isOk(value)) return value;
        const parentOutput = await parent.getOutput('result');
        return isOk(parentOutput) ? parentOutput.finalize(value) : parentOutput;
      });
    assert.equal(isOk(unregisterParent), true);
    const parent = await runtime.createAction(parentSchema);
    assert.equal(isOk(parent), true);
    assert.equal(isOk(await parent.run()), true);
    assert.equal(isOk(await parent.wait(2000)), true);
    const result = await parent.getOutput('result');
    assert.equal(isOk(result), true);
    assert.equal(await result.next(2000), 'child:hello');
    assert.equal(isOk(await unregisterParent()), true);
    assert.equal(isOk(await unregisterChild()), true);
  } finally {
    await runtime.dispose();
  }
});

test('native Action receives binary header defaults and input autofills',
  {skip: !builtNative}, async () => {
  const runtime = await createA11Runtime(options());
  assert.ok(runtime instanceof A11Runtime);
  try {
    const schema = new ActionSchema({name: 'schema_defaults', inputs: {
      secret: new ActionPortSchema({name: 'secret', type: 'text/plain',
        autofills: [new NodeFragment({data: new Chunk({
          data: encoded.encode('secret'),
          metadata: new ChunkMetadata({mimetype: 'text/plain'}),
        }), continued: false})]}),
    }, headers: {
      'x-binary-default': new ActionHeaderSchema({name: 'x-binary-default',
        defaultValue: new Uint8Array([0, 255, 1])}),
    }, outputs: {
      result: new ActionPortSchema({name: 'result', type: 'string'}),
    }});
    const unregister = await runtime.registerAction(schema, async action => {
      const header = await action.getHeader('x-binary-default');
      if (!isOk(header)) return header;
      assert.deepEqual(header, new Uint8Array([0, 255, 1]));
      const secret = await action.getInput('secret');
      if (!isOk(secret)) return secret;
      const value = await secret.next(2000);
      if (!isOk(value)) return value;
      const result = await action.getOutput('result');
      return isOk(result) ? result.finalize(value) : result;
    });
    assert.equal(isOk(unregister), true);
    const action = await runtime.createAction(schema);
    assert.equal(isOk(action), true);
    const nativeSchema = await action.getSchema();
    assert.ok(nativeSchema instanceof ActionSchema);
    assert.deepEqual(nativeSchema.headers.get('x-binary-default').defaultValue,
      new Uint8Array([0, 255, 1]));
    assert.equal(nativeSchema.inputs.get('secret').autofills.length, 1);
    assert.ok(nativeSchema.inputs.get('secret').autofills[0]
      instanceof NodeFragment);
    assert.equal(isOk(await action.run()), true);
    assert.equal(isOk(await action.wait(2000)), true);
    const result = await action.getOutput('result');
    assert.equal(isOk(result), true);
    assert.equal(await result.next(2000), 'secret');
    assert.equal(isOk(await unregister()), true);
  } finally {
    await runtime.dispose();
  }
});

test('WASM store snapshots reload through a host adapter',
  {skip: !builtNative}, async () => {
  let snapshot = null;
  const config = options();
  config.wasm.persistence = {
    load: async () => snapshot,
    save: async bytes => { snapshot = new Uint8Array(bytes); },
  };
  const first = await createA11Runtime(config);
  assert.ok(first instanceof A11Runtime);
  const store = first.createChunkStore('saved');
  assert.equal(isOk(store), true);
  assert.equal(await store.put(new NodeFragment({id: 'saved',
    data: new Chunk({data: encoded.encode('persisted')}), continued: false})), 0);
  await first.dispose();
  const second = await createA11Runtime(config);
  assert.ok(second instanceof A11Runtime);
  const reopened = second.createChunkStore('saved');
  assert.equal(isOk(reopened), true);
  const value = await reopened.get(0);
  assert.equal(isOk(value), true);
  assert.deepEqual(value.data.data, encoded.encode('persisted'));
  await second.dispose();
});

test('WASM persistence failures return an A11 status on disposal',
  {skip: !builtNative}, async () => {
  const config = options();
  config.wasm.persistence = {
    load: async () => null,
    save: async () => { throw new Error('snapshot host unavailable'); },
  };
  const runtime = await createA11Runtime(config);
  assert.ok(runtime instanceof A11Runtime);
  const store = runtime.createChunkStore('failing-save');
  assert.equal(isOk(store), true);
  assert.equal(await store.put(new NodeFragment({id: 'failing-save',
    data: new Chunk({data: encoded.encode('value')})})), 0);
  const status = await runtime.dispose();
  assert.equal(status.code, StatusCode.UNKNOWN);
  assert.equal(status.message, 'WASM persistence failed.');
  assert.match(status.cause.message, /snapshot host unavailable/);
});

test('SDK graph conveniences use C++ nodes, actions, and sessions',
  {skip: !builtNative}, async () => {
  const runtime = await createA11Runtime(options());
  assert.ok(runtime instanceof A11Runtime);
  try {
    const graph = runtime.graph();
    assert.equal(isOk(graph), true);
    assert.equal(await graph.getIfExists('graph-values'), null);
    const node = await graph.node('graph-values');
    assert.ok(node instanceof GraphNode);
    assert.equal(await graph.getIfExists('graph-values'), node);
    assert.equal(runtime.backendOf(node), 'wasm');
    const nodeReference = await node.ref({storage: 'include'});
    assert.ok(nodeReference instanceof NodeRef);
    assert.equal(nodeReference.storage.id, 'local');
    assert.deepEqual(await node.finalize('hello'), okStatus());
    assert.equal(await node.next(2000), 'hello');
    assert.equal(await node.next(2000), null);
    const iterated = await graph.node('graph-iterated');
    assert.equal(isOk(await iterated.put('one')), true);
    assert.equal(isOk(await iterated.finalize('two')), true);
    assert.equal(isOk(await iterated.waitForBufferToDrain(2000)), true);
    const collected = [];
    for await (const value of iterated) collected.push(value);
    assert.deepEqual(collected, ['one', 'two']);
    const configured = await graph.node('graph-configured');
    assert.equal((await configured.getReaderOptions()).offset, 0);
    assert.equal((await configured.setReaderOptions({maxRefDepth: -1}))
      .code, StatusCode.INVALID_ARGUMENT);
    assert.equal(isOk(await configured.setWriterOptions({
      maxChunksToWriteAtOnce: 2})), true);
    assert.equal((await configured.getWriterOptions())
      .maxChunksToWriteAtOnce, 2);
    assert.equal(isOk(await configured.put('first')), true);
    assert.equal(isOk(await configured.finalize('second')), true);
    assert.equal(isOk(await configured.resetReader({offset: 1})), true);
    assert.equal(await configured.next(2000), 'second');
    const idsBeforeDiscard = await graph.entries();
    assert.equal(isOk(idsBeforeDiscard), true);
    assert.equal(idsBeforeDiscard.some(([id]) => id === 'graph-configured'),
      true);
    const sizeBeforeDiscard = await graph.size();
    assert.equal(isOk(sizeBeforeDiscard), true);
    assert.equal(await graph.discard('graph-configured'), true);
    assert.equal(await graph.getIfExists('graph-configured'), null);
    assert.equal(await graph.size(), sizeBeforeDiscard - 1);
    const cancelledReader = await graph.node('graph-cancelled-reader');
    assert.equal(isOk(await cancelledReader.cancelReader()), true);
    const cancelledWriter = await graph.node('graph-cancelled-writer');
    assert.equal(isOk(await cancelledWriter.cancelWriter()), true);
    const sharedStore = runtime.createChunkStore('graph-values');
    assert.equal(isOk(sharedStore), true);
    const sharedFragment = await sharedStore.get(0);
    assert.equal(isOk(sharedFragment), true);
    assert.equal(new TextDecoder().decode(sharedFragment.data.data), 'hello');
    const referenceNode = await graph.node('graph-reference');
    assert.equal(isOk(referenceNode), true);
    assert.equal(isOk(await referenceNode.finalize(new NodeRef({
      id: 'graph-values', length: 1}))), true);
    assert.equal(await referenceNode.next(2000), 'hello');
    const abortedNode = await graph.node('graph-aborted');
    assert.equal(isOk(abortedNode), true);
    assert.equal(await abortedNode.isWritable(), true);
    assert.equal(isOk(await abortedNode.abortWithStatus({
      code: StatusCode.CANCELLED, message: 'cancelled by test'})), true);
    const abortedState = await abortedNode.state();
    assert.equal(isOk(abortedState), true);
    assert.equal(abortedState.writerAbortStatus.code, StatusCode.CANCELLED);
    assert.equal((await abortedNode.getWriterAbortStatus()).code,
      StatusCode.CANCELLED);
    const waitingNode = await graph.node('graph-waiting-read');
    assert.equal(isOk(waitingNode), true);
    const controller = new AbortController();
    const pendingRead = waitingNode.next(2000, controller.signal);
    setTimeout(() => controller.abort(), 10);
    assert.equal((await pendingRead).code, StatusCode.CANCELLED);

    const schema = new ActionSchema({name: 'graph_echo', inputs: {
      text: new ActionPortSchema({name: 'text', type: 'string', required: true,
        unary: true}),
    }, outputs: {
      result: new ActionPortSchema({name: 'result', type: 'string', unary: true}),
    }});
    const unregister = await runtime.registerAction(schema, async action => {
      assert.ok(action instanceof GraphAction);
      const input = await action.getInput('text');
      if (!isOk(input)) return input;
      const value = await input.next(2000);
      if (!isOk(value)) return value;
      const output = await action.getOutput('result');
      if (!isOk(output)) return output;
      const written = await output.put(`reply:${value}`);
      if (!isOk(written)) return written;
      return output.finalize();
    });
    assert.equal(isOk(unregister), true);
    const session = await graph.session('graph-session', {
      noStreamTimeoutMs: null,
    });
    assert.ok(session instanceof GraphSession);
    assert.equal(runtime.backendOf(session), 'wasm');
    assert.equal(session.getNodeMap(), graph);
    assert.equal(await session.isClosed(), false);
    assert.equal(await session.isDone(), false);
    assert.equal(await session.getDeadline(), null);
    const deadline = Date.now() + 60_000;
    assert.equal(isOk(await session.setDeadline(deadline)), true);
    assert.equal(await session.getDeadline(), deadline);
    assert.equal(isOk(await session.setDeadline(null)), true);
    assert.equal(await session.getDeadline(), null);
    const action = await session.createAction('graph_echo', 'graph-echo-call');
    assert.ok(action instanceof GraphAction);
    const input = await action.getInput('text');
    const output = await action.getOutput('result');
    assert.equal(isOk(input), true);
    assert.equal(isOk(output), true);
    const nativePort = await graph.getIfExists(output.getId());
    assert.ok(nativePort instanceof GraphNode);
    assert.equal(isOk(await action.run()), true);
    const activeActions = await session.actions();
    assert.equal(isOk(activeActions), true);
    assert.equal(activeActions.some(([id]) => id === action.getId()), true);
    const activeAction = await session.getAction(action.getId());
    assert.ok(activeAction instanceof GraphAction);
    assert.equal(activeAction.getId(), action.getId());
    assert.equal(await input.put('request'), 0);
    assert.equal(isOk(await input.finalize()), true);
    assert.equal(isOk(await action.wait(2000)), true);
    assert.equal(isOk(await session.awaitAllActions(2000)), true);
    assert.equal((await session.cancelAction('missing-action')).code,
      StatusCode.NOT_FOUND);
    assert.equal(await output.next(2000), 'reply:request');
    assert.equal(isOk(await session.abort({code: StatusCode.CANCELLED,
      message: 'test ended', details: [{source: 'sdk'}]})), true);
    const sessionStatus = await session.getStatus();
    assert.equal(sessionStatus.code, StatusCode.CANCELLED);
    assert.deepEqual(sessionStatus.details, [{source: 'sdk'}]);
    assert.equal(isOk(await unregister()), true);

    let started;
    const running = new Promise(resolve => { started = resolve; });
    const waitingSchema = new ActionSchema({name: 'graph_waiting'});
    const unregisterWaiting = await graph.registerAction(waitingSchema,
      async (_action, {signal}) => {
        started();
        await new Promise(resolve => signal.addEventListener('abort', resolve,
          {once: true}));
        return {code: StatusCode.CANCELLED, message: 'cancelled'};
      });
    assert.equal(isOk(unregisterWaiting), true);
    const waiting = await graph.action('graph_waiting', 'graph-waiting-call');
    assert.ok(waiting instanceof GraphAction);
    assert.equal(isOk(await waiting.run()), true);
    await running;
    assert.equal(isOk(await waiting.cancel()), true);
    assert.equal((await waiting.wait(2000)).code, StatusCode.CANCELLED);
    assert.equal(isOk(await unregisterWaiting()), true);
  } finally {
    await runtime.dispose();
  }
});

test('one WASM selection creates native node, action, and session objects',
  {skip: !builtWasm || !builtNative}, async () => {
  const runtime = await createA11Runtime({backend: 'wasm',
    wasm: {moduleUrl: wasmUrl, flowRuntimeUrl: nativeUrl}});
  assert.ok(runtime instanceof A11Runtime);
  try {
    for (const feature of ['codecs', 'flowTooling', 'chunkStore', 'nodes',
      'actions', 'sessions', 'flowExecution']) {
      assert.equal(runtime.backendFor(feature), 'wasm');
    }
    const map = runtime.createNodeMap();
    assert.ok(map instanceof RuntimeGraph);
    const node = await runtime.createNode('selected-node');
    assert.ok(node instanceof GraphNode);
    assert.equal(await map.get('selected-node'), node);
    assert.equal(isOk(await node.finalize('native')), true);
    assert.equal(await node.next(2000), 'native');

    const schema = new ActionSchema({name: 'selected_action', outputs: {
      result: new ActionPortSchema({name: 'result', type: 'string'}),
    }});
    const unregister = await map.registerAction(schema, async action => {
      const output = await action.getOutput('result');
      if (!isOk(output)) return output;
      return output.finalize('from C++');
    });
    assert.equal(isOk(unregister), true);
    const action = await runtime.createAction(schema, {id: 'selected-action'});
    assert.ok(action instanceof GraphAction);
    assert.equal(await action.isDone(), false);
    assert.equal(isOk(await action.setHeader('x-a11-test', 'hello')), true);
    const headers = await action.getHeaders();
    assert.equal(isOk(headers), true);
    assert.equal(new TextDecoder().decode(headers.get('x-a11-test')), 'hello');
    assert.equal(isOk(await action.removeHeader('x-a11-test')), true);
    assert.equal(isOk(await action.run()), true);
    assert.equal(isOk(await action.wait(2000)), true);
    assert.equal(await action.isDone(), true);
    assert.equal(await action.hasBeenRun(), true);
    const output = await action.getOutput('result');
    assert.equal(isOk(output), true);
    assert.equal(await output.next(2000), 'from C++');
    const session = await runtime.createSession({id: 'selected-session',
      noStreamTimeoutMs: null});
    assert.ok(session instanceof GraphSession);
    assert.equal(runtime.backendOf(session), 'wasm');
    assert.equal(isOk(await session.abort()), true);
    assert.equal(isOk(await unregister()), true);
  } finally {
    await runtime.dispose();
  }
});

test('native SDK session calls a second native peer over a host WireStream',
  {skip: !builtWasm || !builtNative}, async () => {
  const runtime = await createA11Runtime({backend: 'wasm',
    wasm: {moduleUrl: wasmUrl, flowRuntimeUrl: nativeUrl}});
  assert.ok(runtime instanceof A11Runtime);
  const serverRuntime = await createA11Runtime(options());
  assert.ok(serverRuntime instanceof A11Runtime);
  try {
    const schema = new ActionSchema({name: 'remote_graph_echo', inputs: {
      text: new ActionPortSchema({name: 'text', type: 'string'}),
    }, outputs: {
      result: new ActionPortSchema({name: 'result', type: 'string'}),
    }});
    const registry = new ActionRegistry(serverRuntime);
    assert.equal(isOk(await registry.register(schema.name, schema, async action => {
      const input = await action.getInput('text');
      if (!isOk(input)) return input;
      const value = await input.next(2000);
      if (!isOk(value)) return value;
      const output = await action.getOutput('result');
      return isOk(output) ? output.finalize(`server:${value}`,
        {wait: true}) : output;
    })), true);
    const server = await Session.create(serverRuntime, {
      id: 'remote-graph-server', noStreamTimeoutMs: null});
    assert.ok(server instanceof Session);
    const pair = InProcessWireStream.createPair();
    assert.equal(isOk(pair), true);
    const client = await runtime.createSession({id: 'remote-graph-client',
      noStreamTimeoutMs: null});
    assert.ok(client instanceof GraphSession);
    const [accepted, connected] = await Promise.all([
      server.addStream(pair[1], StreamMode.ACCEPT),
      client.addStream(pair[0], StreamMode.START),
    ]);
    assert.equal(isOk(accepted), true);
    assert.equal(isOk(connected), true);
    const call = await client.createAction(schema, 'remote-graph-call');
    assert.ok(call instanceof GraphAction);
    const input = await call.getInput('text');
    const output = await call.getOutput('result');
    assert.equal(isOk(input), true);
    assert.equal(isOk(output), true);
    assert.equal(isOk(await call.call()), true);
    assert.equal(isOk(await input.finalize('request')), true);
    assert.equal(isOk(await call.waitForDispatch(2000)), true);
    assert.equal(isOk(await call.wait(2000)), true);
    assert.equal(await output.next(2000), 'server:request');
    assert.equal(isOk(await client.halfClose()), true);
    assert.equal(isOk(await server.halfClose()), true);
    assert.equal(isOk(await client.done(2000)), true);
  } finally {
    await runtime.dispose();
    await serverRuntime.dispose();
  }
});

test('native receiving session exposes typed inbound messages',
  {skip: !builtNative}, async () => {
  const runtime = await createA11Runtime({backend: 'wasm',
    wasm: {moduleUrl: wasmUrl, flowRuntimeUrl: nativeUrl}});
  assert.ok(runtime instanceof A11Runtime);
  const peerRuntime = await createA11Runtime(options());
  assert.ok(peerRuntime instanceof A11Runtime);
  try {
    const session = await runtime.createSessionWithRecv({id: 'sdk-receive',
      noStreamTimeoutMs: null,
      headers: {'x-a11-test': encoded.encode('session value')}});
    assert.ok(session instanceof GraphSessionWithRecv);
    assert.equal(runtime.backendOf(session), 'wasm');
    const pair = InProcessWireStream.createPair();
    assert.equal(isOk(pair), true);
    const peer = await Session.create(peerRuntime, {id: 'sdk-receive-peer',
      noStreamTimeoutMs: null});
    assert.ok(peer instanceof Session);
    const [accepted, connected] = await Promise.all([
      peer.addStream(pair[1], StreamMode.ACCEPT),
      session.addStream(pair[0]),
    ]);
    assert.equal(isOk(accepted), true);
    assert.equal(isOk(connected), true);
    const sent = new WireMessage({nodeFragments: [new NodeFragment({
      id: 'sdk-received-node', data: new Chunk({data: encoded.encode('hello')}),
    })]});
    assert.equal(isOk(await peer.send(sent)), true);
    const received = await session.receiveWithStreamId(2000);
    assert.equal(isOk(received), true);
    assert.ok(received.message instanceof WireMessage);
    assert.equal(received.message.nodeFragments[0].id, 'sdk-received-node');
    assert.ok(received.streamId.startsWith(`${pair[0].getId()}-`));
    assert.equal(isOk(await session.dispatchWireMessage(received.message,
      received.streamId)), true);
    const dispatched = await session.getNodeMap().node('sdk-received-node');
    assert.equal(isOk(dispatched), true);
    const stored = await dispatched.nextChunk(2000);
    assert.equal(isOk(stored), true);
    assert.deepEqual(stored.data, encoded.encode('hello'));
    assert.equal(isOk(await peer.halfClose()), true);
    assert.equal(await session.receive(2000), null);
    assert.equal(isOk(await session.halfClose()), true);
    assert.equal(isOk(await session.done(2000)), true);
  } finally {
    await runtime.dispose();
    await peerRuntime.dispose();
  }
});

test('SDK WASM host settings apply to Flow and direct actions',
  {skip: !builtWasm || !builtNative}, async () => {
  const runtime = await createA11Runtime({backend: 'wasm', wasm: {
    moduleUrl: wasmUrl, flowRuntimeUrl: nativeUrl,
    environment: {A11_SDK_ENV_TEST: 'visible'},
    allowedEnvironmentNames: ['A11_SDK_ENV_TEST'],
    actions: {'web-fetch': false},
  }});
  assert.ok(runtime instanceof A11Runtime);
  try {
    const graph = runtime.graph();
    assert.equal(isOk(graph), true);
    const excluded = await graph.action('web-fetch', 'excluded-web-fetch');
    assert.equal(excluded.code, StatusCode.NOT_FOUND);
    const environment = await graph.action('env_get', 'sdk-env-get');
    assert.ok(environment instanceof GraphAction);
    const names = await environment.getInput('names');
    const optionsNode = await environment.getInput('options');
    const values = await environment.getOutput('values');
    assert.equal(isOk(names), true);
    assert.equal(isOk(optionsNode), true);
    assert.equal(isOk(values), true);
    assert.equal(isOk(await environment.run()), true);
    assert.equal(isOk(await names.finalize(['A11_SDK_ENV_TEST'])), true);
    assert.equal(isOk(await optionsNode.finalize()), true);
    assert.equal(isOk(await environment.wait(2000)), true);
    assert.deepEqual(await values.next(2000), {A11_SDK_ENV_TEST: 'visible'});
    const flow = await runtime.runFlow({source: `flow env_sdk {
      out values: object
      found = run env_get(names: ["A11_SDK_ENV_TEST"])
      found.values -> values
    }`, flow: 'env_sdk'});
    assert.equal(isOk(flow), true);
    assert.deepEqual(JSON.parse(Buffer.from(flow.values[0].data,
      'base64').toString()), {A11_SDK_ENV_TEST: 'visible'});
  } finally {
    await runtime.dispose();
  }
});

test('native Flow invokes a registered TypeScript callback on native Action',
  {skip: !builtWasm || !builtNative}, async () => {
  const runtime = await createA11Runtime(options());
  assert.ok(runtime instanceof A11Runtime);
  const schema = new ActionSchema({name: 'echo', inputs: {
    text: new ActionPortSchema({name: 'text', type: 'string', required: true,
      unary: true}),
  }, outputs: {
    result: new ActionPortSchema({name: 'result', type: 'string', unary: true}),
  }});
  let calls = 0;
  const unregister = await runtime.registerAction(schema, async action => {
    ++calls;
    assert.ok(action instanceof GraphAction);
    assert.equal(runtime.backendOf(action), 'wasm');
    const input = await action.getInput('text');
    assert.equal(isOk(input), true);
    assert.equal(runtime.backendOf(input), 'wasm');
    assert.equal(await input.next(2000), 'hello');
    const output = await action.getOutput('result');
    assert.equal(isOk(output), true);
    return output.finalize('received', {wait: true});
  });
  assert.equal(isOk(unregister), true);
  try {
    const result = await runtime.runFlow({source: `flow host_action {
      out result: string
      called = run echo(text: "hello")
      called.result -> result
    }`, flow: 'host_action'});
    assert.equal(isOk(result), true);
    assert.equal(calls, 1);
    assert.equal(Buffer.from(result.result[0].data, 'base64').toString(),
      'received');
    const failSchema = new ActionSchema({name: 'reject', outputs: {
      result: new ActionPortSchema({name: 'result', type: 'string'}),
    }});
    const unregisterFailure = await runtime.registerAction(failSchema,
      async () => ({code: StatusCode.INVALID_ARGUMENT,
        message: 'host input failed', details: [{field: 'text'}]}));
    assert.equal(isOk(unregisterFailure), true);
    const failed = await runtime.runFlow({source: `flow failing {
      out result: string
      called = run reject()
      called.result -> result
    }`, flow: 'failing'});
    assert.equal(failed.code, StatusCode.INVALID_ARGUMENT);
    assert.deepEqual(failed.details, [{field: 'text'}]);
    assert.equal(isOk(await unregisterFailure()), true);
  } finally {
    assert.equal(isOk(await unregister()), true);
    await runtime.dispose();
  }
});

test('default runtime loads the packaged WASM backend',
  {skip: !builtWasm || !builtNative}, async () => {
  const runtime = await createA11Runtime();
  assert.ok(runtime instanceof A11Runtime);
  assert.equal(runtime.backendFor('nodes'), 'wasm');
  const node = await runtime.createNode('default-native-node');
  assert.ok(node instanceof GraphNode);
  assert.equal(isOk(await runtime.dispose()), true);
});

test('WASM asset directory must be an unambiguous URL', async () => {
  const value = await createA11Runtime({wasm: {
    assetsBaseUrl: new URL('https://example.test/a11-assets'),
  }});
  assert.equal(value.code, StatusCode.INVALID_ARGUMENT);
});
