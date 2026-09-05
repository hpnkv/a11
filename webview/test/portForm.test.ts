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

import { createPortInput, describeSchema } from '../src/portForm.js';

beforeEach(() => {
  const window = new JSDOM('<!doctype html><body></body>').window;
  Object.assign(globalThis, {
    document: window.document,
    HTMLElement: window.HTMLElement,
    Option: window.Option,
  });
});

const nullableInteger = {
  anyOf: [{ type: 'integer' }, { type: 'null' }],
  default: null,
  description: 'An optional integer.',
};

test('a nullable integer uses an initially unset integer editor', () => {
  const port = createPortInput({
    name: 'request',
    type: 'application/json',
    required: false,
    unary: true,
    schema: {
      type: 'object',
      properties: { seed: nullableInteger },
    },
  });

  const input = port.element.querySelector<HTMLInputElement>('input[type="number"]');
  const set = [...port.element.querySelectorAll('button')].find((button) => button.textContent === 'Set value');
  const clear = [...port.element.querySelectorAll('button')].find((button) => button.textContent === 'Clear');
  assert.ok(input);
  assert.ok(set);
  assert.ok(clear);
  assert.equal(port.element.querySelector('.json-editor'), null);
  assert.equal(input.hidden, true);
  assert.equal(port.read(), undefined);

  set.click();
  input.value = '42';
  assert.deepEqual(port.read(), { seed: 42 });

  clear.click();
  assert.equal(input.hidden, true);
  assert.deepEqual(port.read(), { seed: null });
});

test('a required nullable value reads as null until set', () => {
  const port = createPortInput({
    name: 'seed',
    type: 'application/json',
    required: true,
    unary: true,
    schema: nullableInteger,
  });
  assert.equal(port.read(), null);
});

test('nullable schema descriptions name both the concrete and null types', () => {
  const description = describeSchema({
    type: 'object',
    properties: { seed: nullableInteger },
  });
  assert.equal(description?.querySelector('.schema-field-type')?.textContent, 'integer | null');
});

test('a union of concrete types remains a free-form JSON editor', () => {
  const port = createPortInput({
    name: 'value',
    type: 'application/json',
    required: false,
    unary: true,
    schema: { anyOf: [{ type: 'integer' }, { type: 'string' }] },
  });
  assert.ok(port.element.querySelector('.json-editor'));
  assert.equal(port.element.querySelector('input[type="number"]'), null);
});
