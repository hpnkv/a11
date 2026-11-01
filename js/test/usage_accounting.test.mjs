import assert from 'node:assert/strict';
import test from 'node:test';
import {
  Action, ActionSchema, NodeRef, NodeMap, UsageAccounting, UsageLedger,
  enableUsageTracking, okStatus, isOk, usageTrackingEnabled,
} from '../dist/index.js';

const need = value => { assert.ok(isOk(value), value.message); return value; };
const schema = name => new ActionSchema({name});
const update = (id, count, revision = 1, covers = []) => ({
  type: 'a11.usage', version: 1, id, producer_id: id, revision, covers,
  usage: {usage_id: id, usage_revision: revision, total_tokens: count},
});

test('usage sources cross unaware actions and private node maps', async () => {
  const action = need(Action.create(schema('root'), {handler: async action => {
    const root = need(await action.registerStream('llm_usage', {type: 'a11_action', id: action.getId()}, 'root'));
    const rootNode = need(await action.getNodeMap().resolve(root.node));
    need(await rootNode.put(update('root', 5)));
    const child = need(action.makeNested(schema('unaware'), false));
    need(child.bindHandler(async middle => {
      const leaf = need(middle.makeNested(schema('leaf')));
      need(leaf.bindHandler(async leaf => {
        const ref = need(await leaf.registerStream('llm_usage', {type: 'external_tool', id: 'search'}, 'child', 'root'));
        const node = need(await leaf.getNodeMap().resolve(ref.node));
        need(await node.put(update('child', 40)));
        return okStatus();
      }));
      return leaf.run();
    }));
    need(child.run());
    need(await rootNode.put(update('root', 100, 2)));
    return okStatus();
  }}));
  const accounting = need(UsageAccounting.create(action));
  const collecting = accounting.collect();
  need(action.run());
  need(await action.wait(5000));
  need(await collecting);
  assert.equal(accounting.sources.size, 2);
  assert.ok([...accounting.sources.values()].every(source => source.node instanceof NodeRef));
  assert.equal(need(accounting.directUsage()).total_tokens, 100);
  assert.equal(need(accounting.descendantUsage()).total_tokens, 40);
  assert.equal(need(accounting.totalUsage()).total_tokens, 140);
  assert.equal(accounting.coverage, 'reported_sources');
  const replay = need(UsageAccounting.create(action));
  need(await replay.collect());
  assert.equal(need(replay.totalUsage()).total_tokens, 140);
});

test('an explicit off suppresses a tracking request', async () => {
  const action = need(Action.create(schema('off'), {handler: async () => okStatus()}));
  need(enableUsageTracking(action, false));
  assert.equal(need(usageTrackingEnabled(action)), false);
  const accounting = need(UsageAccounting.create(action));
  need(action.run());
  need(await action.wait(5000));
  need(await accounting.collect());
  assert.equal(accounting.coverage, 'disabled');
  assert.equal(action.getNodeMap().getIfExists(action.getId() + '#__streams__'), null);
});

test('aggregate coverage replaces child contributions and detects cycles', () => {
  const ledger = new UsageLedger();
  ledger.observe(update('child', 40));
  ledger.observe(update('parent', 100, 1, ['child']));
  assert.equal(need(ledger.totals()).total_tokens, 100);
  ledger.observe(update('parent', 120, 2, ['child']));
  assert.equal(need(ledger.totals()).total_tokens, 120);
  ledger.observe(update('child', 40, 2, ['parent']));
  assert.equal(isOk(ledger.totals()), false);
});

test('NodeRef imports preserve the node identity', async () => {
  const first = new NodeMap();
  const second = new NodeMap();
  const ref = new NodeRef({id: 'shared'});
  const original = need(await first.resolve(ref));
  assert.equal(need(await second.importReference(ref, first)), original);
  assert.equal(need(await second.resolve(ref)), original);
});
