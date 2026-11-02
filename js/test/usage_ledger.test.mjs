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
import test from 'node:test';
import {UsageLedger, isOk} from '../dist/index.js';
const need = value => { assert.ok(isOk(value), value.message); return value; };
const update = (id, count, revision = 1, covers = []) => ({type: 'a11.usage', version: 1, id, producer_id: id, revision, covers, usage: {usage_id: id, usage_revision: revision, total_tokens: count}});

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
