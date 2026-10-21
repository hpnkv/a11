import assert from 'node:assert/strict';
import test from 'node:test';
import {
  Action, INTERACT_WITH_GEMMA_SCHEMA, interactWithGemma, isOk,
  makeTextMessageInteraction, setGemmaEngineFactory, resetGemmaEngineFactory,
} from '../dist/index.js';

const need = value => { assert.ok(isOk(value), value.message); return value; };

for (const fails of [false, true]) {
  test(`Gemma retains engine counters before teardown (${fails ? 'failure' : 'stop filtering'})`, async () => {
    let closed = false;
    setGemmaEngineFactory(async () => ({
      async generate(_prompt, token) {
        token('Hello<end_of_turn>hidden output');
        if (fails) throw new Error('Interrupted after generation');
        return 'Hello<end_of_turn>hidden output';
      },
      usage() {
        assert.equal(closed, false);
        return {input_tokens: 20, output_tokens: 7, total_tokens: 27};
      },
      close() { closed = true; },
    }));
    try {
      const action = need(Action.create(INTERACT_WITH_GEMMA_SCHEMA, {handler: interactWithGemma}));
      need(action.run());
      const events = [];
      const drain = async name => {
        const node = need(await action.getOutput(name, false));
        const values = [];
        for (;;) {
          const value = await node.next();
          if (!isOk(value) || value === null) break;
          values.push(value);
          if (name === 'event_stream') events.push(value);
        }
        return values;
      };
      const readers = ['event_stream', 'text_output', 'thoughts', 'new_interactions'].map(drain);
      need(await need(await action.getInput('interactions')).finalize(need(await makeTextMessageInteraction('Hello'))));
      need(await need(await action.getInput('config')).finalize({}));
      const status = await action.wait(5_000);
      const [, text] = await Promise.all(readers);
      assert.equal(isOk(status), !fails);
      assert.equal(closed, true);
      const usage = events.filter(e => e.type === 'a11.usage');
      assert.equal(usage.length, 2);
      assert.equal(usage[0].usage.complete, false);
      assert.equal(usage[1].usage.total_tokens, 27);
      assert.equal(usage[1].usage.output_tokens, 7);
      assert.equal(usage[1].usage.complete, !fails);
      if (!fails) assert.equal(text.join(''), 'Hello');
    } finally { resetGemmaEngineFactory(); }
  });
}
