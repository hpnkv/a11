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
import {readFile} from 'node:fs/promises';
import test from 'node:test';

const manifest = JSON.parse(
  await readFile(new URL('../package.json', import.meta.url), 'utf8'),
);
const defaults =
  manifest.contributes.configuration.properties['a11.extraAllowedTools']
    .default;

test('default Gateway tools include the coding-agent Flow and input actions', () => {
  for (const name of [
    'workspace_info',
    'run_command',
    'run_flow',
    'flow_guide',
    'request_user_input',
    'report_completion',
  ]) {
    assert.ok(defaults.includes(name), `${name} is not enabled by default`);
  }
});

test('administrative actions are not offered to the model', () => {
  for (const name of [
    'coding_agent_info',
    'configure_coding_agent',
    'respond_user_input',
  ]) {
    assert.equal(defaults.includes(name), false, `${name} is model-facing`);
  }
});
