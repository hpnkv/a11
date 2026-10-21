import assert from 'node:assert/strict';
import test from 'node:test';
import {
  BlockKind, PresentationReducer, formatUsage, interactionUsageUpdates,
  rememberUsage, makeInteraction, presentConversation, valueOrThrow,
} from '../dist/index.js';

const event = (id, revision, usage) => ({
  type: 'a11.usage', version: 1, id, revision,
  usage: {usage_id: id, usage_revision: revision, ...usage},
});

test('usage revisions replace counts and preserve the final context after compaction', () => {
  const reducer = new PresentationReducer();
  for (const update of [
    event('turn', 1, {output_tokens: 10, complete: false}),
    event('compact', 1, {context_invalidated: true, context_only: true}),
    event('turn', 2, {output_tokens: 30, context_tokens: 80}),
    event('turn', 1, {output_tokens: 10}),
  ]) valueOrThrow(reducer.onUsageEvent(update));
  const blocks = reducer.blocks.filter(b => b.kind === BlockKind.USAGE);
  assert.equal(blocks.length, 2);
  assert.equal(blocks.at(-1).usage.output_tokens, 30);
  assert.equal(blocks.at(-1).usage.context_tokens, 80);
});

test('failed runs retain usage in conversation metadata without assistant text', async () => {
  const question = valueOrThrow(makeInteraction({role: 'user'}));
  const update = event('failed-request', 1, {input_tokens: 20, output_tokens: 7, total_tokens: 27});
  rememberUsage(question, update);
  rememberUsage(question, update);
  assert.equal(interactionUsageUpdates(question).length, 1);
  const turns = valueOrThrow(await presentConversation([question]));
  assert.equal(turns.flatMap(t => t.blocks).find(b => b.kind === BlockKind.USAGE).usage.total_tokens, 27);
});

test('usage labels preserve known zero, unknown counts, caches and reasoning inclusion', () => {
  assert.equal(formatUsage({complete: false}), '— input, — output');
  assert.equal(formatUsage({input_tokens: 0, output_tokens: 5, cached_input_tokens: 900, reasoning_tokens: 2}), '900 input (900 cached), 5 output');
  assert.equal(formatUsage({usage_scope: 'turn'}), '— input, — output');
  assert.equal(formatUsage({context_tokens: 100, context_source: 'request_estimate'}), '— input, — output');
});


test('presentation defers usage until text and turn completion', () => {
  const reducer = new PresentationReducer({}, 'model', true);
  valueOrThrow(reducer.onText('A'));
  valueOrThrow(reducer.onUsageEvent(event('turn', 1, {input_tokens: 100, output_tokens: 1, complete: false})));
  valueOrThrow(reducer.onText('11'));
  assert.equal(reducer.blocks.length, 1);
  assert.equal(reducer.blocks[0].text, 'A11');
  valueOrThrow(reducer.onUsageEvent(event('turn', 2, {input_tokens: 100, output_tokens: 20})));
  valueOrThrow(reducer.endTurn());
  assert.deepEqual(reducer.blocks.map(b => b.kind), [BlockKind.TEXT, BlockKind.USAGE]);
  assert.equal(formatUsage(reducer.blocks[1].usage), '100 input, 20 output');
});

test('reconciliation details never add another usage contribution', () => {
  const reducer = new PresentationReducer();
  valueOrThrow(reducer.onUsageEvent(event('total', 1, {total_tokens: 100})));
  valueOrThrow(reducer.onUsageEvent({...event('detail', 1, {total_tokens: 40}), contribution: 'reconciliation'}));
  assert.equal(reducer.blocks.length, 1);
  assert.equal(reducer.blocks[0].usage.total_tokens, 100);
});
