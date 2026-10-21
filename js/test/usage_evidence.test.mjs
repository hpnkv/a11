import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';
import {usageUpdate, summarizeUsage} from '../dist/index.js';

const evidence = JSON.parse(fs.readFileSync(new URL('../../testdata/token_accounting/harness_usage.json', import.meta.url), 'utf8'));
for (const scenario of evidence.cases) {
  test(`usage replay preserves reported counts: ${scenario.name}`, () => {
    const latest = new Map();
    for (const event of [...scenario.events, ...scenario.events]) {
      const update = usageUpdate(event);
      if (!update || update.contribution === 'reconciliation' || update.usage.context_only || update.usage.context_invalidated) continue;
      const before = latest.get(update.id);
      if (!before || update.revision > before.revision) latest.set(update.id, update);
    }
    assert.equal(latest.size, scenario.comparison.length);
    const total = summarizeUsage([...latest.values()].map(update => update.usage));
    for (const name of ['input_tokens', 'cached_input_tokens', 'cache_write_tokens', 'output_tokens']) {
      const counters = scenario.comparison.map(row => row.actual[name]);
      const expected = counters.every(v => v != null) ? counters.reduce((a, b) => a + b, 0) : undefined;
      assert.equal(total[name], expected, name);
    }
  });
}
