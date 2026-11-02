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
import {existsSync} from 'node:fs';
import test from 'node:test';
import {fileURLToPath} from 'node:url';

import {Action, ActionMessage, ActionPortSchema, ActionRegistry, ActionSchema,
  AsyncNode, NodeFragment, NodeMap,
  Chunk, ChunkMetadata, FlowProgram, FlowTooling, InProcessWireStream,
  Session, SessionWithRecv,
  StatusCode, WireMessage,
  LocalChunkStore, UsageAccounting, createA11Runtime, enableUsageTracking, isOk,
  executeActionsFromInteraction, getToolDefinitions, makeInteraction,
  makeTextMessageInteraction, logRecordFromChunk, logText, okStatus,
  usageTrackingEnabled}
  from '../dist/index.js';
import {INTERACT_WITH_GEMMA_SCHEMA, INTERACT_WITH_LLM_SCHEMA,
  interactWithGemma, interactWithLlm,
  resetGemmaEngineFactory, setGemmaEngineFactory}
  from '../dist/runtime_llm.js';

const moduleUrl = new URL('../../cpp/a11/wasm/build/dist/index.mjs',
  import.meta.url);
const flowRuntimeUrl = new URL('../../cpp/a11/wasm/build/dist/native.mjs',
  import.meta.url);
const built = existsSync(fileURLToPath(moduleUrl)) &&
  existsSync(fileURLToPath(flowRuntimeUrl));

test('native session delivers typed stream callbacks', {skip: !built},
  async () => {
    const runtime = await createA11Runtime({wasm: {moduleUrl, flowRuntimeUrl}});
    assert.equal(isOk(runtime), true);
    try {
      const seen = [];
      let received;
      const receivedPromise = new Promise(resolve => { received = resolve; });
      let closed;
      const closedPromise = new Promise(resolve => { closed = resolve; });
      let finished;
      const finishedPromise = new Promise(resolve => { finished = resolve; });
      const session = await Session.create(runtime, {
        id: 'callback-session', noStreamTimeoutMs: null,
        onStreamMessage: (message, stream, owner) => {
          seen.push({message, stream, owner});
          if (message !== null) received();
          else closed();
          return okStatus();
        },
        onStreamDone: (stream, owner) => {
          assert.equal(stream, pair[0]);
          assert.equal(owner, session);
          finished();
          return okStatus();
        },
      });
      assert.ok(session instanceof Session);
      const pair = InProcessWireStream.createPair();
      assert.equal(isOk(pair), true);
      const [accepted, connected] = await Promise.all([
        pair[1].accept(() => okStatus(), () => okStatus()),
        session.addStream(pair[0]),
      ]);
      assert.equal(isOk(accepted), true);
      assert.equal(isOk(connected), true);
      assert.equal(isOk(pair[1].send(new WireMessage({nodeFragments: [
        new NodeFragment({id: 'callback-node', data: new Chunk({
          data: new TextEncoder().encode('callback'),
        })}),
      ]}))), true);
      let timer;
      try {
        await Promise.race([receivedPromise, new Promise((_, reject) => {
          timer = setTimeout(async () => {
            const diagnostic = await runtime.nativeFlow.command({operation:
              'fiber_report'});
            reject(new Error(`session callback timed out: ${diagnostic.report}`));
          }, 2000);
        })]);
      } finally {
        clearTimeout(timer);
      }
      const delivered = seen.find(event => event.message !== null);
      assert.ok(delivered?.message instanceof WireMessage);
      assert.equal(delivered.stream, pair[0]);
      assert.equal(delivered.owner, session);
      assert.equal(isOk(await session.halfClose()), true);
      assert.equal(isOk(pair[1].halfClose()), true);
      await Promise.race([closedPromise, new Promise((_, reject) =>
        setTimeout(() => reject(new Error('session close callback timed out')),
          2000))]);
      await Promise.race([finishedPromise, new Promise((_, reject) =>
        setTimeout(() => reject(new Error('session done callback timed out')),
          2000))]);
    } finally {
      assert.equal(isOk(await runtime.dispose()), true);
    }
  });

test('familiar classes create C++ owned graph objects', {skip: !built},
  async () => {
    const runtime = await createA11Runtime({wasm: {moduleUrl, flowRuntimeUrl}});
    assert.equal(isOk(runtime), true);
    try {
      const map = NodeMap.create(runtime);
      assert.ok(map instanceof NodeMap);
      assert.equal(runtime.backendOf(map), 'wasm');
      const store = LocalChunkStore.create('convenience-store', {runtime});
      assert.ok(store instanceof LocalChunkStore);
      assert.equal(runtime.backendOf(store), 'wasm');
      assert.equal(LocalChunkStore.create('unowned').code,
        StatusCode.INVALID_ARGUMENT);

      const node = await AsyncNode.create('convenience-node', {runtime});
      assert.ok(node instanceof AsyncNode);
      assert.equal(runtime.backendOf(node), 'wasm');
      const reference = await node.ref();
      assert.equal(isOk(reference), true);
      const resolved = await map.resolve(reference);
      assert.ok(resolved instanceof AsyncNode);
      assert.equal(resolved.getId(), node.getId());
      const pending = node.next(1000);
      assert.equal(isOk(await node.finalize('hello')), true);
      assert.equal(await pending, 'hello');

      const schema = new ActionSchema({name: 'convenience-action',
        outputs: {result: new ActionPortSchema({name: 'result',
          type: 'string'})}});
      const action = await Action.create(schema, {runtime});
      assert.ok(action instanceof Action);
      assert.equal(runtime.backendOf(action), 'wasm');
      const streamSources = await action.getStreamSourcesNode();
      assert.ok(streamSources instanceof AsyncNode);
      assert.equal((await Action.create(schema,
        {runtime, streamId: 'missing-session'})).code,
      StatusCode.INVALID_ARGUMENT);
      assert.equal((await Action.create(schema,
        {runtime, handler: async () => undefined})).code,
      StatusCode.INVALID_ARGUMENT);
      const session = await Session.create(runtime);
      assert.ok(session instanceof Session);
      assert.ok(session.getNodeMap() instanceof NodeMap);
      const attached = await Action.create(schema, {session});
      assert.ok(attached instanceof Action);
      const receiving = await SessionWithRecv.create(runtime);
      assert.ok(receiving instanceof SessionWithRecv);
    } finally {
      assert.equal(isOk(await runtime.dispose()), true);
    }
  });

test('convenience registry delegates registration and queries to C++',
  {skip: !built}, async () => {
    const runtime = await createA11Runtime({wasm: {moduleUrl, flowRuntimeUrl}});
    assert.equal(isOk(runtime), true);
    try {
      const registry = new ActionRegistry(runtime);
      const schema = new ActionSchema({name: 'registry-greeting',
        outputs: {result: new ActionPortSchema({name: 'result',
          type: 'string'})}});
      const handler = phrase => async action => {
        const logged = await action.log(phrase);
        if (!isOk(logged)) return logged;
        const output = await action.getOutput('result');
        return isOk(output) ? output.finalize(phrase) : output;
      };
      assert.equal(isOk(await registry.register(schema.name, schema,
        handler('first'))), true);
      assert.equal(await registry.isRegistered(schema.name), true);
      const readSchema = await registry.getSchema(schema.name);
      assert.ok(readSchema instanceof ActionSchema);
      assert.equal(readSchema.outputs.get('result').type, 'string');
      const names = await registry.listRegisteredActions();
      assert.equal(isOk(names), true);
      assert.ok(names.includes(schema.name));
      const message = await registry.makeActionMessage(schema.name, 'reg-call');
      assert.equal(isOk(message), true);
      assert.equal(message.id, 'reg-call');

      assert.equal(isOk(await registry.register(schema.name, schema,
        handler('second'))), true);
      const action = await registry.makeAction(schema.name);
      assert.ok(action instanceof Action);
      const output = await action.getOutput('result');
      const log = await action.getLogNode();
      assert.equal(isOk(output), true);
      assert.equal(isOk(log), true);
      assert.equal(isOk(await action.run()), true);
      const logChunk = await log.nextChunk(1000);
      assert.equal(isOk(logChunk), true);
      assert.equal(logText(logRecordFromChunk(logChunk)), 'second');
      assert.equal(await output.next(1000), 'second');
      assert.equal(isOk(await action.wait(1000)), true);
      assert.equal(isOk(await registry.unregister(schema.name)), true);
      assert.equal(await registry.isRegistered(schema.name), false);
      assert.equal((await registry.getSchema(schema.name)).code,
        StatusCode.NOT_FOUND);

      const old = await runtime.registerAction(schema, handler('old'));
      assert.equal(isOk(old), true);
      const latest = await runtime.registerAction(schema, handler('latest'));
      assert.equal(isOk(latest), true);
      assert.equal(isOk(await old()), true);
      assert.equal(await registry.isRegistered(schema.name), true);
      const replacement = await registry.makeAction(schema.name);
      assert.ok(replacement instanceof Action);
      const replacementOutput = await replacement.getOutput('result');
      assert.equal(isOk(replacementOutput), true);
      assert.equal(isOk(await replacement.run()), true);
      assert.equal(await replacementOutput.next(1000), 'latest');
      assert.equal(isOk(await replacement.wait(1000)), true);
      assert.equal(isOk(await latest()), true);
      assert.equal(await registry.isRegistered(schema.name), false);
    } finally {
      assert.equal(isOk(await runtime.dispose()), true);
    }
  });

test('Flow convenience returns typed chunks from C++ execution',
  {skip: !built}, async () => {
    const runtime = await createA11Runtime({wasm: {moduleUrl, flowRuntimeUrl}});
    assert.equal(isOk(runtime), true);
    try {
      const source = 'flow typed { out result: string\n "hi" -> result\n }';
      const tooling = new FlowTooling(runtime);
      const checked = tooling.check(source);
      assert.equal(isOk(checked), true);
      assert.equal(checked.ok, true);
      const formatted = tooling.format(source);
      assert.equal(isOk(formatted), true);
      assert.equal(formatted.ok, true);
      const observed = [];
      const result = await new FlowProgram(runtime, source, 'typed').run({
        onOutput: (port, chunk) => observed.push([port, chunk]),
      });
      assert.equal(isOk(result), true);
      assert.ok(result.result[0] instanceof Chunk);
      assert.equal(observed[0][0], 'result');
      assert.ok(observed[0][1] instanceof Chunk);

      const echo = new FlowProgram(runtime, `flow echo {
        in text: string required
        out result: string
        text -> result
      }`, 'echo');
      const input = new Chunk({data: new TextEncoder().encode('"hello"'),
        metadata: new ChunkMetadata({mimetype: 'application/json'})});
      const echoed = await echo.run({inputs: {text: [input]}});
      assert.equal(isOk(echoed), true);
      assert.ok(echoed.result[0] instanceof Chunk);
      const cancelled = new AbortController();
      cancelled.abort();
      assert.equal((await echo.run({signal: cancelled.signal})).code,
        StatusCode.CANCELLED);
    } finally {
      assert.equal(isOk(await runtime.dispose()), true);
    }
  });

test('native usage accounting reads registered stream references',
  {skip: !built}, async () => {
    const runtime = await createA11Runtime({wasm: {moduleUrl, flowRuntimeUrl}});
    assert.equal(isOk(runtime), true);
    try {
      const schema = new ActionSchema({name: 'usage-native'});
      const childSchema = new ActionSchema({name: 'usage-native-child'});
      const unregisterChild = await runtime.registerAction(childSchema,
        async running => {
          const tracking = await usageTrackingEnabled(running);
          if (!isOk(tracking)) return tracking;
          if (!tracking) return okStatus();
          const reference = await running.registerStream('llm_usage',
            {type: 'external_tool', id: 'lookup'},
            'child-usage', 'root-usage');
          if (!isOk(reference)) return reference;
          const node = await running.resolveNode(reference.node);
          if (!isOk(node)) return node;
          const written = await node.put({type: 'a11.usage', version: 1,
            id: 'child-usage', producer_id: 'child-usage', revision: 1,
            usage: {usage_id: 'child-usage', usage_revision: 1,
              total_tokens: 7}});
          return isOk(written) ? okStatus() : written;
        });
      assert.equal(isOk(unregisterChild), true);
      const unregister = await runtime.registerAction(schema, async running => {
        const reference = await running.registerStream('llm_usage',
          {type: 'a11_action', id: running.getId()}, 'root-usage');
        if (!isOk(reference)) return reference;
        const node = await running.resolveNode(reference.node);
        if (!isOk(node)) return node;
        const update = {type: 'a11.usage', version: 1, id: 'root-usage',
          producer_id: 'root-usage', revision: 1,
          usage: {usage_id: 'root-usage', usage_revision: 1,
            total_tokens: 31}};
        const written = await node.put(update);
        if (!isOk(written)) return written;
        const child = await running.makeNested('usage-native-child');
        if (!isOk(child)) return child;
        const started = await child.run();
        if (!isOk(started)) return started;
        const completed = await child.wait(2000);
        return isOk(completed) ? okStatus() : completed;
      });
      assert.equal(isOk(unregister), true);
      const action = await Action.create(schema, {runtime});
      assert.equal(isOk(action), true);
      const accounting = await UsageAccounting.create(action);
      assert.equal(isOk(accounting), true);
      const collected = accounting.collect();
      assert.equal(isOk(await action.run()), true);
      assert.equal(isOk(await action.wait(2000)), true);
      assert.equal(isOk(await collected), true);
      assert.equal(accounting.sources.size, 2);
      assert.equal(accounting.directUsage().total_tokens, 31);
      assert.equal(accounting.descendantUsage().total_tokens, 7);
      assert.equal(accounting.totalUsage().total_tokens, 38);
      assert.equal(accounting.coverage, 'reported_sources');
      assert.equal(isOk(await unregister()), true);

      const disabledSchema = new ActionSchema({name: 'usage-disabled'});
      const unregisterDisabled = await runtime.registerAction(disabledSchema,
        async running => {
          const child = await running.makeNested('usage-native-child');
          if (!isOk(child)) return child;
          const started = await child.run();
          if (!isOk(started)) return started;
          const completed = await child.wait(2000);
          return isOk(completed) ? okStatus() : completed;
        });
      assert.equal(isOk(unregisterDisabled), true);
      const disabled = await Action.create(disabledSchema, {runtime});
      assert.equal(isOk(disabled), true);
      assert.equal(isOk(await enableUsageTracking(disabled, false)), true);
      assert.equal(await usageTrackingEnabled(disabled), false);
      const skipped = await UsageAccounting.create(disabled);
      assert.equal(isOk(skipped), true);
      assert.equal(isOk(await disabled.run()), true);
      assert.equal(isOk(await disabled.wait(2000)), true);
      assert.equal(isOk(await skipped.collect()), true);
      assert.equal(skipped.coverage, 'disabled');
      assert.equal(isOk(await unregisterDisabled()), true);
      assert.equal(isOk(await unregisterChild()), true);
    } finally {
      assert.equal(isOk(await runtime.dispose()), true);
    }
  });

test('native nested tool calls retain provider ids and stream outputs',
  {skip: !built}, async () => {
    const runtime = await createA11Runtime({wasm: {moduleUrl, flowRuntimeUrl}});
    assert.equal(isOk(runtime), true);
    try {
      const childSchema = new ActionSchema({name: 'tool_echo',
        inputs: {value: new ActionPortSchema({name: 'value', type: 'string'})},
        outputs: {result: new ActionPortSchema({name: 'result',
          type: 'string'})}});
      const registry = new ActionRegistry(runtime);
      assert.equal(isOk(await registry.register('tool_echo', childSchema,
        async child => {
          const input = await child.getInput('value');
          if (!isOk(input)) return input;
          const value = await input.next(2000);
          if (!isOk(value)) return value;
          const output = await child.getOutput('result');
          return isOk(output) ? output.finalize(`echo:${value}`) : output;
        })), true);
      const definitions = await getToolDefinitions(registry, ['tool_echo']);
      assert.equal(isOk(definitions), true);
      assert.equal(definitions[0].name, 'tool_echo');

      let gathered;
      const parentSchema = new ActionSchema({name: 'tool_parent'});
      const unregister = await runtime.registerAction(parentSchema,
        async parent => {
          const interaction = makeInteraction({action_calls: [
            new ActionMessage({id: 'provider-call-1', name: 'tool_echo'}),
          ], action_inputs: {'provider-call-1': [new NodeFragment({
            id: 'value', data: new Chunk({
              data: new TextEncoder().encode('"hello"'),
              metadata: new ChunkMetadata({mimetype: 'application/json'}),
            }), continued: false,
          })]}});
          if (!isOk(interaction)) return interaction;
          gathered = await executeActionsFromInteraction(interaction,
            parent, registry, {timeoutMs: 2000});
          return isOk(gathered) ? okStatus() : gathered;
        });
      assert.equal(isOk(unregister), true);
      const parent = await Action.create(parentSchema, {runtime});
      assert.equal(isOk(parent), true);
      assert.equal(isOk(await parent.setHeader(
        'x-a11-allowed-llm-actions', 'tool_echo')), true);
      assert.equal(isOk(await parent.run()), true);
      assert.equal(isOk(await parent.wait(2000)), true);
      assert.equal(isOk(gathered), true);
      assert.equal(gathered['provider-call-1'].length, 1);
      assert.equal(isOk(await unregister()), true);
      assert.equal(isOk(await registry.unregister('tool_echo')), true);
    } finally {
      assert.equal(isOk(await runtime.dispose()), true);
    }
  });

test('browser Gemma adapter runs as a C++ action with host inference',
  {skip: !built}, async () => {
    const runtime = await createA11Runtime({wasm: {moduleUrl, flowRuntimeUrl}});
    assert.equal(isOk(runtime), true);
    setGemmaEngineFactory(async () => ({
      async generate(_prompt, token) {
        token('Hello<end_of_turn>hidden output');
        return 'Hello<end_of_turn>hidden output';
      },
      usage: () => ({input_tokens: 20, output_tokens: 7,
        total_tokens: 27}),
    }));
    try {
      const unregister = await runtime.registerAction(
        INTERACT_WITH_GEMMA_SCHEMA, interactWithGemma);
      assert.equal(isOk(unregister), true);
      const action = await Action.create(INTERACT_WITH_GEMMA_SCHEMA,
        {runtime});
      assert.equal(isOk(action), true);
      const text = await action.getOutput('text_output', false);
      assert.equal(isOk(text), true);
      const interactions = await action.getInput('interactions');
      const config = await action.getInput('config');
      assert.equal(isOk(interactions), true);
      assert.equal(isOk(config), true);
      assert.equal(isOk(await action.run()), true);
      const message = await makeTextMessageInteraction('Hello');
      assert.equal(isOk(message), true);
      assert.equal(isOk(await interactions.finalize(message)), true);
      assert.equal(isOk(await config.finalize({})), true);
      assert.equal(isOk(await action.wait(5000)), true);
      assert.equal(await text.next(2000), 'Hello');
      assert.equal(isOk(await unregister()), true);

      const unregisterRouter = await runtime.registerAction(
        INTERACT_WITH_LLM_SCHEMA, interactWithLlm);
      assert.equal(isOk(unregisterRouter), true);
      const routed = await Action.create(INTERACT_WITH_LLM_SCHEMA,
        {runtime});
      assert.equal(isOk(routed), true);
      assert.equal(isOk(await routed.setHeader('x-a11-llm-provider',
        'gemma')), true);
      const routedText = await routed.getOutput('text_output', false);
      const routedInteractions = await routed.getInput('interactions');
      const routedConfig = await routed.getInput('config');
      assert.equal(isOk(routedText) && isOk(routedInteractions) &&
        isOk(routedConfig), true);
      assert.equal(isOk(await routed.run()), true);
      assert.equal(isOk(await routedInteractions.finalize(message)), true);
      assert.equal(isOk(await routedConfig.finalize({})), true);
      assert.equal(isOk(await routed.wait(5000)), true);
      assert.equal(await routedText.next(2000), 'Hello');
      assert.equal(isOk(await unregisterRouter()), true);
    } finally {
      resetGemmaEngineFactory();
      assert.equal(isOk(await runtime.dispose()), true);
    }
  });
