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
import {FlowRunnerView, renderFlowSource, type RunnableFlow} from '../src/flowRunner.js';
import {OutputPresenter, renderDebugValue} from '../src/outputPresentation.js';

beforeEach(() => {
  const window = new JSDOM('<!doctype html><body></body>').window;
  Object.assign(globalThis, {
    document: window.document,
    Event: window.Event,
    HTMLElement: window.HTMLElement,
  });
});

test('runner reconciles ports while retaining values for unchanged inputs', () => {
  const root = document.createElement('main');
  const runner = new FlowRunnerView(root);
  runner.openFlow(flow(['query', 'limit']));
  const editors = root.querySelectorAll('textarea');
  editors[0].value = 'a11';
  editors[0].dispatchEvent(new Event('input'));

  runner.openFlow(flow(['query', 'path']));

  const labels = [...root.querySelectorAll('label')];
  assert.deepEqual(labels.map((label) => label.textContent?.split(' · ')[0]), ['query', 'path']);
  assert.equal(labels[0].querySelector('textarea')?.value, 'a11');
  assert.equal(labels[0].querySelector('textarea')?.dataset.kind, 'text');
});

test('runner presents bytes as base64 text rather than JSON', () => {
  const root = document.createElement('main');
  const runner = new FlowRunnerView(root);
  const spec = flow(['payload']);
  spec.ports[0]!.type = 'bytes';
  runner.openFlow(spec);

  const editor = root.querySelector<HTMLTextAreaElement>('textarea');
  assert.equal(editor?.dataset.kind, 'bytes');
  assert.match(editor?.placeholder ?? '', /base64 bytes/);
});

test('runner writes strings as text and bytes as MessagePack', async () => {
  const root = document.createElement('main');
  const runner = new FlowRunnerView(root);
  const spec = flow(['query', 'payload']);
  spec.ports[1]!.type = 'bytes';
  runner.openFlow(spec);
  const editors = root.querySelectorAll<HTMLTextAreaElement>('[data-kind]');
  editors[0]!.value = 'hello';
  editors[1]!.value = 'AAH/';
  let call: unknown[] | undefined;
  const session = (runner as unknown as {
    session: {runFlowSource: (...args: unknown[]) => Promise<Record<string, unknown>>};
  }).session;
  session.runFlowSource = async (...args) => {
    call = args;
    return {};
  };

  root.querySelector<HTMLButtonElement>('button')!.click();
  await new Promise((resolve) => setTimeout(resolve, 0));

  const inputs = call?.[2] as Record<string, unknown[]>;
  assert.deepEqual(inputs.query, ['hello']);
  assert.deepEqual(inputs.payload, [new Uint8Array([0, 1, 255])]);
  assert.deepEqual(call?.[3], {
    query: 'text/plain',
    payload: 'application/x-msgpack',
  });
});

test('Flow source highlighting consumes native semantic ranges', () => {
  const target = document.createElement('pre');
  renderFlowSource(target, 'flow demo {}', [{start: 0, end: 4, kind: 'declaration-keyword'}]);
  assert.equal(target.textContent, 'flow demo {}');
  assert.equal(target.querySelector('.flow-token-declaration-keyword')?.textContent, 'flow');
});

test('text stream output accumulates into one Studio-style port card', () => {
  const target = document.createElement('div');
  const output = new OutputPresenter(target);
  output.append('answer', 'hello ', 'text/plain');
  output.append('answer', 'world', 'text/plain');

  assert.equal(target.querySelectorAll('.runner-value-card').length, 1);
  assert.match(target.textContent ?? '', /2 values/);
  assert.match(target.querySelector('.runner-value-content')?.textContent ?? '', /hello world/);
  const chunks = [...target.querySelectorAll('button')].find((button) => button.textContent === 'Joined');
  chunks?.click();
  assert.equal(target.querySelectorAll('.runner-chunk').length, 2);
});

test('binary values use a bounded byte presentation', () => {
  const target = document.createElement('div');
  renderDebugValue(target, new Uint8Array([65, 0, 66]), 'application/octet-stream');
  assert.match(target.textContent ?? '', /3 B/);
  assert.match(target.textContent ?? '', /\\x41\\x00\\x42/);
});

function flow(inputs: string[]): RunnableFlow {
  return {
    source: 'flow inspect {}',
    name: 'inspect',
    path: '/repo/check.flow',
    line: 0,
    ports: inputs.map((name) => ({
      name,
      direction: 'in',
      type: 'string',
      stream: false,
      required: false,
    })),
  };
}
