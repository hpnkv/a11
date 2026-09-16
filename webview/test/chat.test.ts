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
import {beforeEach, test} from 'node:test';

import {JSDOM} from 'jsdom';
import {okStatus} from '@curiositystack/a11';

import {
  A11ChatSession,
  composeIdeSystemPrompt,
  mergeAllowedToolNames,
  parseCodingAgentDefaults,
  reportsToolFromInteraction,
} from '../src/a11client.js';
import {AssistantBubble} from '../src/chat.js';
import type {A11Config, ActionDescriptor} from '../src/bridge.js';

beforeEach(() => {
  const window = new JSDOM('<!doctype html><body></body>').window;
  Object.assign(globalThis, {
    document: window.document,
    HTMLElement: window.HTMLElement,
    cancelAnimationFrame: () => {},
    requestAnimationFrame: () => 1,
  });
});

test('Gateway coding instructions gain editor context and Flow dispatch rules', () => {
  const config: A11Config = {
    url: 'ws://gateway/a11',
    provider: 'claude',
    model: '',
    apiKey: '',
    baseUrl: '',
    allowedTools: [],
    ide: 'Visual Studio Code',
    ideVersion: '1.105.0',
    projectName: 'a11',
    projectPath: '/work/a11',
  };
  const descriptors: ActionDescriptor[] = [
    {
      name: 'ide__get_active_file',
      description: 'Read the active editor.',
      inputs: [],
      outputs: [],
    },
  ];

  const prompt = composeIdeSystemPrompt('Gateway system prompt.', config, descriptors);

  assert.ok(prompt.startsWith('Gateway system prompt.'));
  assert.match(prompt, /Before each meaningful tool call or series/);
  assert.match(prompt, /Visual Studio Code 1\.105\.0/);
  assert.match(prompt, /Project path: \/work\/a11/);
  assert.match(prompt, /ide__get_active_file: Read the active editor\./);
  assert.match(prompt, /call ide__action_name/);
  assert.match(prompt, /run action_name/);
});

test('Gateway coding-agent defaults include every model-facing tool name', () => {
  const defaults = parseCodingAgentDefaults({
    system_prompt: 'Gateway coding-agent instructions',
    tool_names: ['workspace_info', 'run_command', 'web-fetch', 'web-render', 'run_command'],
  });

  assert.deepEqual(defaults, {
    systemPrompt: 'Gateway coding-agent instructions',
    toolNames: ['workspace_info', 'run_command', 'web-fetch', 'web-render'],
  });
  assert.deepEqual(
    mergeAllowedToolNames(['ide__get_active_file'], ['shell_.*'], defaults?.toolNames ?? []),
    [
      'ide__get_active_file',
      'shell_.*',
      'workspace_info',
      'run_command',
      'web-fetch',
      'web-render',
      'request_user_input',
    ],
  );
});

test('interaction events do not duplicate locally reported IDE tool runs', () => {
  const local = ['ide__get_active_file', 'ide__apply_patch'];

  assert.equal(reportsToolFromInteraction('ide__apply_patch', local), false);
  assert.equal(reportsToolFromInteraction('run_command', local), true);
});

test('the general-Gateway fallback identifies the actual IDE', () => {
  const config: A11Config = {
    url: 'ws://gateway/a11',
    provider: 'claude',
    model: '',
    apiKey: '',
    baseUrl: '',
    allowedTools: [],
    ide: 'Cursor',
    ideVersion: '2.0',
    projectName: 'sample',
    projectPath: null,
  };

  const prompt = composeIdeSystemPrompt('', config, []);

  assert.match(prompt, /embedded in the user's IDE/);
  assert.match(prompt, /IDE: Cursor 2\.0/);
  assert.doesNotMatch(prompt, /JetBrains IDE/);
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
    arguments: {checks: 'Input is still arriving.'},
    phase: 'started',
  });

  assert.equal(host.querySelector('.completion-report'), null);
  assert.ok(host.querySelector('.tool-run'));
});

test('request_user_input renders choices and answers through its host', () => {
  const answers: Array<[string, string]> = [];
  const host = document.createElement('div');
  const bubble = new AssistantBubble(
    host,
    () => {},
    (id, answer) => {
      answers.push([id, answer]);
    },
  );

  bubble.addToolRun({
    id: 'request-1',
    tool: 'request_user_input',
    arguments: {
      question: 'Which implementation should I use?',
      options: [
        {label: 'Native', description: 'Use the A11-native implementation.'},
        {label: 'Adapter', description: 'Keep the compatibility adapter.'},
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

test('tool cards expose inputs, outputs, logs, and status as they stream', () => {
  const host = document.createElement('div');
  const bubble = new AssistantBubble(host, () => {});

  bubble.addToolRun({
    id: 'tool-1',
    tool: 'search_text',
    arguments: {query: 'Flow', path: 'cpp'},
    phase: 'started',
  });
  bubble.addToolRun({
    id: 'tool-1',
    tool: 'search_text',
    outputs: {matches: [{path: 'cpp/a11/flow/values.cc', line: 1400}]},
    log: 'Found one match.',
    phase: 'finished',
  });

  const card = host.querySelector<HTMLElement>('.tool-run');
  assert.ok(card);
  assert.match(card.textContent ?? '', /Inputs/);
  assert.match(card.textContent ?? '', /"query": "Flow"/);
  assert.match(card.textContent ?? '', /Outputs/);
  assert.match(card.textContent ?? '', /values\.cc/);
  assert.match(card.textContent ?? '', /Logs/);
  assert.match(card.textContent ?? '', /Found one match/);
  assert.match(card.textContent ?? '', /completed/);
});

test('patch tools render a numbered theme-aware diff result', () => {
  const host = document.createElement('div');
  const bubble = new AssistantBubble(host, () => {});
  bubble.addToolRun({
    id: 'patch-1',
    tool: 'ide__apply_patch',
    arguments: {
      path: 'src/example.ts',
      patch: '@@ -1 +1 @@\n-const n = 1;\n+const n = 2;\n',
    },
    outputs: {
      result: {path: 'src/example.ts', hunks: 1, added: 1, removed: 1},
    },
    phase: 'finished',
  });

  const card = host.querySelector<HTMLDetailsElement>('.tool-run.patch-run');
  assert.ok(card?.open);
  assert.match(card.textContent ?? '', /src\/example\.ts · \+1 −1/);
  assert.equal(card.querySelector('.patch-line.removed code')?.textContent, 'const n = 1;');
  assert.equal(card.querySelector('.patch-line.added code')?.textContent, 'const n = 2;');
  assert.equal(card.querySelector('.patch-line.added .patch-line-number')?.textContent, '1');
  assert.doesNotMatch(card.textContent ?? '', /"patch"/);
});

test('a collapsed failed tool shows its error message', () => {
  const host = document.createElement('div');
  const bubble = new AssistantBubble(host, () => {});
  bubble.addToolRun({
    id: 'failed-1',
    tool: 'run_command',
    arguments: {command: 'false'},
    status: {
      code: 13,
      message: 'Permission denied by the workspace boundary',
    },
    phase: 'finished',
  });
  const card = host.querySelector<HTMLDetailsElement>('.tool-run.failed');
  assert.ok(card);
  assert.equal(card.open, false);
  assert.match(card.querySelector('summary')?.textContent ?? '', /Permission denied/);
});

test('run_flow has a dedicated source preview without duplicating it in inputs', () => {
  const host = document.createElement('div');
  const bubble = new AssistantBubble(host, () => {});
  const source = 'flow inspect {\n  out result: string\n  "ok" -> result\n}';

  bubble.addToolRun({
    id: 'flow-1',
    tool: 'run_flow',
    arguments: {source, timeout_seconds: 60},
    phase: 'started',
  });

  const preview = host.querySelector<HTMLElement>('.flow-preview');
  assert.equal(preview?.textContent, source);
  assert.ok(host.querySelector('.tool-run.flow-run'));
  assert.equal(host.querySelector('.tool-run-name')?.textContent, 'run_flow');
  const inputs = host.querySelector<HTMLElement>('.tool-detail-section');
  assert.match(inputs?.textContent ?? '', /timeout_seconds/);
  assert.doesNotMatch(inputs?.textContent ?? '', /flow inspect/);
});

test('assistant fenced code receives token-level syntax highlighting', () => {
  const host = document.createElement('div');
  const bubble = new AssistantBubble(host, () => {});
  bubble.appendToken('```python\ndef answer():\n    return 42\n```');
  bubble.finish();
  assert.ok(host.querySelector('code.language-python.hljs'));
  assert.match(host.querySelector('code')?.innerHTML ?? '', /hljs-keyword/);
});

test('assistant Flow fences use Studio semantic token roles', () => {
  const host = document.createElement('div');
  const bubble = new AssistantBubble(host, () => {});
  bubble.appendToken('```flow\nflow ask {\n  in question: string\n}\n```');
  bubble.finish();
  assert.ok(host.querySelector('code.language-flow.hljs'));
  assert.equal(host.querySelector('.flow-token-keyword')?.textContent, 'flow');
  assert.equal(host.querySelector('.flow-token-type')?.textContent, 'string');
});

test('unlabelled Flow fences from native run logs use Studio semantic tokens', () => {
  const host = document.createElement('div');
  const bubble = new AssistantBubble(host, () => {});
  bubble.appendToken('```\nflow ask {\n  in question: string\n}\n```');
  bubble.finish();
  assert.equal(host.querySelector('.flow-token-keyword')?.textContent, 'flow');
  assert.equal(host.querySelector('.flow-token-type')?.textContent, 'string');
});

test('assistant fenced code without a language is highlighted automatically', () => {
  const host = document.createElement('div');
  const bubble = new AssistantBubble(host, () => {});
  bubble.appendToken('```\nconst answer = 42;\n```');
  bubble.finish();
  const code = host.querySelector<HTMLElement>('pre code.hljs');
  assert.ok(code);
  assert.match(code.innerHTML, /class="hljs-/);
});

test('interrupt cooperatively cancels the active conversation action', () => {
  let cancelled = 0;
  const session = new A11ChatSession();
  const internals = session as unknown as {
    activeCall: {cancel(): ReturnType<typeof okStatus>} | null;
  };
  internals.activeCall = {
    cancel: () => {
      cancelled += 1;
      return okStatus();
    },
  };

  assert.equal(session.interrupt(), true);
  assert.equal(cancelled, 1);
  internals.activeCall = null;
  assert.equal(session.interrupt(), false);
});

test('a closed Gateway session is replaced before the next action', async () => {
  const notices: string[] = [];
  const client = new A11ChatSession(undefined, ({state}) => notices.push(state));
  let closed = 0;
  const internals = client as unknown as {
    session: unknown;
    stream: unknown;
    refreshConfig: () => Promise<void>;
    connect: () => Promise<void>;
    ensureConnected: () => Promise<void>;
  };
  internals.session = {
    isClosed: () => true,
    getStatus: okStatus,
    halfClose: () => {
      closed += 1;
    },
  };
  internals.stream = {getStatus: okStatus};
  internals.refreshConfig = async () => {};
  internals.connect = async () => {
    internals.session = {isClosed: () => false, getStatus: okStatus};
    internals.stream = {getStatus: okStatus};
  };

  await internals.ensureConnected();

  assert.equal(closed, 1);
  assert.deepEqual(notices, ['connecting', 'connected']);
  client.halfClose();
});
