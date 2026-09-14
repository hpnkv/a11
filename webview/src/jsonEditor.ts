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

/**
 * A small JSON editor with syntax highlighting, used wherever a value has no
 * JSON Schema to build a form from.
 *
 * Highlighting is the standard overlay trick: a `<pre>` holding tokenized markup
 * sits in the same grid cell as a transparent-text `<textarea>`, so the real
 * caret and selection stay native while the colors come from the `<pre>`. Both
 * share font, padding, and wrapping, so the two layers cannot drift; the `<pre>`
 * drives the box height, which makes the editor grow with its content.
 */

import { indentJson, jsonTokens, readJson } from '@curiositystack/a11/presentation';

const CLASSES = {
  key: 'json-key',
  string: 'json-string',
  keyword: 'json-keyword',
  number: 'json-number',
  punctuation: 'json-punct',
} as const;

function escapeHtml(text: string): string {
  return text.replace(/[&<>]/g, (c) => (c === '&' ? '&amp;' : c === '<' ? '&lt;' : '&gt;'));
}

/** Wrap every recognized JSON token of `text` in a class-carrying span. */
function highlight(text: string): string {
  let html = '';
  let last = 0;
  for (const token of jsonTokens(text)) {
    html += escapeHtml(text.slice(last, token.from));
    html += `<span class="${CLASSES[token.kind]}">${escapeHtml(text.slice(token.from, token.to))}</span>`;
    last = token.to;
  }
  // The trailing newline keeps the box tall enough for a caret on the last line.
  return `${html + escapeHtml(text.slice(last))}\n`;
}

/** Render already-serialized JSON with the same colours as editable values. */
export function renderJson(target: HTMLElement, text: string): void {
  target.innerHTML = highlight(text).trimEnd();
}

export interface JsonEditor {
  readonly element: HTMLElement;
  /** The parsed value, or `undefined` when blank. Throws on malformed JSON. */
  read(): unknown;
  setText(text: string): void;
  text(): string;
}

/** Build a syntax-highlighted JSON editor, optionally seeded with `value`. */
export function createJsonEditor(options: { value?: string; placeholder?: string } = {}): JsonEditor {
  const element = document.createElement('div');
  element.className = 'json-editor';

  const highlighted = document.createElement('pre');
  highlighted.className = 'json-highlight';
  highlighted.setAttribute('aria-hidden', 'true');

  const input = document.createElement('textarea');
  input.className = 'json-input';
  input.spellcheck = false;
  input.rows = 1;
  if (options.placeholder) input.placeholder = options.placeholder;

  const hint = document.createElement('p');
  hint.className = 'json-hint';

  element.append(highlighted, input);

  const sync = (): void => {
    highlighted.innerHTML = highlight(input.value);
    const message = readJson(input.value).error ?? '';
    hint.textContent = message;
    hint.classList.toggle('visible', message !== '');
    element.classList.toggle('invalid', message !== '');
  };

  input.addEventListener('input', sync);
  // Keep Tab as indentation: leaving the field mid-object is rarely the intent.
  input.addEventListener('keydown', (event) => {
    if (event.key !== 'Tab') return;
    event.preventDefault();
    const edit = indentJson(input.value, input.selectionStart, input.selectionEnd);
    input.value = edit.text;
    input.selectionStart = edit.selectionStart;
    input.selectionEnd = edit.selectionEnd;
    sync();
  });

  const setText = (text: string): void => {
    input.value = text;
    sync();
  };
  setText(options.value ?? '');

  const wrapper = document.createElement('div');
  wrapper.className = 'json-editor-wrapper';
  wrapper.append(element, hint);

  return {
    element: wrapper,
    read(): unknown {
      const text = input.value.trim();
      if (text === '') return undefined;
      try {
        return JSON.parse(text);
      } catch (error) {
        throw new Error(`invalid JSON (${error instanceof Error ? error.message : String(error)})`);
      }
    },
    setText,
    text: () => input.value,
  };
}
