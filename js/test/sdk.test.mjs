import assert from 'node:assert/strict';
import test from 'node:test';

import {
  Action,
  ActionPortSchema,
  ActionRegistry,
  ActionSchema,
  AsyncNode,
  Chunk,
  ChunkMetadata,
  INTERACT_WITH_GEMMA_SCHEMA,
  NodeFragment,
  StatusCode,
  ToolAdapter,
  actionNameMatchesAllowed,
  a11PeerFromString,
  a11PeerToString,
  interactWithGemma,
  isOk,
  makeInteraction,
  makeTextMessageInteraction,
  parseInteraction,
  setGemmaEngineFactory,
  resetGemmaEngineFactory,
} from '../dist/index.js';

const need = (value) => {
  if (!isOk(value)) {
    throw new Error(`${StatusCode[value.code]}: ${value.message}`);
  }
  return value;
};

function autofilledSchema() {
  const secret = new NodeFragment({
    data: new Chunk({
      metadata: new ChunkMetadata({ mimetype: 'text/plain' }),
      data: new TextEncoder().encode('secret'),
    }),
    continued: false,
  });
  return new ActionSchema({
    name: 'tool_with_autofill',
    inputs: {
      visible: new ActionPortSchema({ name: 'visible', type: 'application/json' }),
      hidden: new ActionPortSchema({
        name: 'hidden',
        type: 'text/plain',
        autofills: [secret],
      }),
    },
  });
}

test('ToolAdapter hides autofilled inputs from the LLM', () => {
  const adapter = new ToolAdapter(autofilledSchema());
  const schema = need(adapter.getInputSchema());
  assert.ok('visible' in schema.properties);
  assert.ok(!('hidden' in schema.properties));
});

test('actionNameMatchesAllowed matches regex patterns', () => {
  const patterns = ['get_.*', 'list_users'];
  assert.equal(need(actionNameMatchesAllowed('get_weather', patterns)), true);
  assert.equal(need(actionNameMatchesAllowed('list_users', patterns)), true);
  assert.equal(need(actionNameMatchesAllowed('delete_everything', patterns)), false);
  // A plain name is still matched exactly, preserving the old behaviour.
  assert.equal(need(actionNameMatchesAllowed('list_users', ['list_users'])), true);
  assert.equal(need(actionNameMatchesAllowed('list_users_v2', ['list_users'])), false);
});

test('actionNameMatchesAllowed rejects invalid patterns with INVALID_ARGUMENT', () => {
  const result = actionNameMatchesAllowed('anything', ['(']);
  assert.equal(isOk(result), false);
  assert.equal(result.code, StatusCode.INVALID_ARGUMENT);
});

test('A11Peer round-trips through its URL form', () => {
  const peer = need(a11PeerFromString('mcp://example.com/mcp'));
  assert.equal(peer.protocol, 'mcp');
  assert.equal(peer.scheme, 'http');
  assert.equal(peer.endpoint, 'example.com/mcp');
  const session = need(a11PeerFromString('a11://$sender'));
  assert.equal(session.identity, '$sender');
  assert.equal(a11PeerToString(session), 'a11://$sender');
});

test('Interaction round-trips through an AsyncNode', async () => {
  const node = need(await AsyncNode.create('interaction_node'));
  const original = need(makeInteraction({ role: 'user', content: [{ text: 'hi' }] }));
  need(await node.put(original, { final: true }));
  const decoded = need(await node.next());
  const parsed = need(parseInteraction(decoded));
  assert.equal(parsed.id, original.id);
  assert.equal(parsed.role, 'user');
});

test('interact_with_gemma streams tokens and produces an assistant turn', async () => {
  setGemmaEngineFactory(async () => ({
    async generate(prompt, onToken) {
      assert.ok(prompt.includes('<start_of_turn>model'));
      // The model runs on past its turn; everything from <end_of_turn> on must
      // be truncated and never surface.
      for (const piece of ['Hel', 'lo', ' wor', 'ld', '<end_of_turn>', '\n<start_of_turn>user\nnope']) {
        onToken(piece);
      }
      return 'Hello world<end_of_turn>\n<start_of_turn>user\nnope';
    },
  }));
  try {
    const registry = new ActionRegistry();
    need(registry.register('interact_with_gemma', INTERACT_WITH_GEMMA_SCHEMA, interactWithGemma));
    const action = need(
      Action.create(INTERACT_WITH_GEMMA_SCHEMA, { handler: interactWithGemma, registry }),
    );
    need(action.run());

    const interactions = need(await action.getInput('interactions'));
    const user = need(await makeTextMessageInteraction('Hi there'));
    need(await interactions.put(user, { final: true }));
    const config = need(await action.getInput('config'));
    need(await config.putFinal({}));

    const output = need(await action.getOutput('text_output', false));
    let text = '';
    while (true) {
      const value = need(await output.next());
      if (value === null) break;
      text += String(value);
    }
    assert.equal(text, 'Hello world');

    const newInteractions = need(await action.getOutput('new_interactions', false));
    const assistant = need(parseInteraction(need(await newInteractions.next())));
    assert.equal(assistant.role, 'model');
    assert.equal(assistant.backend_specific_metadata.backend, 'gemma');

    need(await action.wait(5_000));
  } finally {
    resetGemmaEngineFactory();
  }
});
