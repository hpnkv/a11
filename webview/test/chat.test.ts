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
import { beforeEach, test } from 'node:test';

import { JSDOM } from 'jsdom';

import { AssistantBubble } from '../src/chat.js';

beforeEach(() => {
  const window = new JSDOM('<!doctype html><body></body>').window;
  Object.assign(globalThis, {
    document: window.document,
    HTMLElement: window.HTMLElement,
    cancelAnimationFrame: () => {},
    requestAnimationFrame: () => 1,
  });
});

test('report_completion renders as a structured completion report', () => {
  const host = document.createElement('div');
  const bubble = new AssistantBubble(host, () => {});

  bubble.addToolRun({
    id: 'completion-1',
    tool: 'report_completion',
    arguments: {
      summary: 'Implemented the requested IDE presentation.',
      checks: 'Webview tests passed.',
      remaining: 'No remaining work.',
    },
    phase: 'started',
  });

  const report = host.querySelector<HTMLElement>('.completion-report');
  assert.ok(report);
  assert.match(report.textContent ?? '', /Task completed/);
  assert.match(report.textContent ?? '', /Implemented the requested IDE presentation/);
  assert.match(report.textContent ?? '', /Verification/);
  assert.match(report.textContent ?? '', /Webview tests passed/);
  assert.match(report.textContent ?? '', /Remaining/);
  assert.match(report.textContent ?? '', /No remaining work/);
  assert.match(report.textContent ?? '', /Finalising/);

  bubble.addToolRun({
    id: 'completion-1',
    tool: 'report_completion',
    log: 'Implemented the requested IDE presentation.',
    phase: 'finished',
  });
  assert.match(report.textContent ?? '', /Recorded/);
});

test('report_completion stays a regular tool until summary is available', () => {
  const host = document.createElement('div');
  const bubble = new AssistantBubble(host, () => {});

  bubble.addToolRun({
    id: 'completion-2',
    tool: 'report_completion',
    arguments: { checks: 'Input is still arriving.' },
    phase: 'started',
  });

  assert.equal(host.querySelector('.completion-report'), null);
  assert.ok(host.querySelector('.tool-run'));
});

test('request_user_input renders choices and answers through its host', () => {
  const answers: Array<[string, string]> = [];
  const host = document.createElement('div');
  const bubble = new AssistantBubble(host, () => {}, (id, answer) => {
    answers.push([id, answer]);
  });

  bubble.addToolRun({
    id: 'request-1',
    tool: 'request_user_input',
    arguments: {
      question: 'Which implementation should I use?',
      options: [
        { label: 'Native', description: 'Use the A11-native implementation.' },
        { label: 'Adapter', description: 'Keep the compatibility adapter.' },
      ],
      allow_free_text: true,
    },
    phase: 'started',
  });

  const request = host.querySelector<HTMLElement>('.input-request-report');
  assert.ok(request);
  assert.match(request.textContent ?? '', /Input required/);
  assert.match(request.textContent ?? '', /Which implementation should I use/);
  assert.match(request.textContent ?? '', /1\. Native/);
  assert.match(request.textContent ?? '', /Use the A11-native implementation/);
  assert.match(request.textContent ?? '', /custom response is also accepted/);

  const native = [...request.querySelectorAll('button')].find((button) =>
    button.textContent?.includes('Native'),
  );
  assert.ok(native);
  native.click();
  assert.deepEqual(answers, [['request-1', 'Native']]);

  bubble.addToolRun({
    id: 'request-1',
    tool: 'request_user_input',
    phase: 'finished',
  });
  assert.match(request.textContent ?? '', /Input received/);
  assert.equal(native.disabled, true);
});
