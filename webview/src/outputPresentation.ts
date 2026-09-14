/*
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

import {
  isOk,
} from '@curiositystack/a11';
import {
  appendOutput,
  DEFAULT_OUTPUT_VIEW,
  looksLikeMarkdown,
  outputCopyText,
  presentValue,
  type OutputBlock,
  type ValuePresentation,
} from '@curiositystack/a11/presentation';

import {renderJson} from './jsonEditor.js';
import {renderMarkdown} from './markdown.js';

interface OutputState {
  block: OutputBlock;
  joined: boolean;
  markdown: boolean;
  expanded: boolean;
  card: HTMLElement;
  frame: number;
}

/** Studio-style bounded, per-port presentation of streaming action values. */
export class OutputPresenter {
  private readonly states = new Map<string, OutputState>();

  constructor(private readonly target: HTMLElement) {}

  clear(): void {
    for (const state of this.states.values()) {
      if (state.frame && typeof cancelAnimationFrame === 'function') cancelAnimationFrame(state.frame);
    }
    this.states.clear();
    this.target.replaceChildren();
  }

  /** Render an immutable shared-layer snapshot, including its exact retention counts. */
  show(blocks: readonly OutputBlock[]): void {
    this.clear();
    for (const block of blocks) {
      const state: OutputState = {
        block,
        ...DEFAULT_OUTPUT_VIEW,
        card: document.createElement('article'),
        frame: 0,
      };
      state.card.className = 'runner-value-card';
      this.states.set(block.port, state);
      this.target.append(state.card);
      this.render(state);
    }
  }

  append(port: string, value: unknown, mimetype: string): void {
    const textual = mimetype.startsWith('text/');
    let state = this.states.get(port);
    if (!state) {
      const [block] = appendOutput([], port, value, textual, mimetype);
      state = {
        block: block!,
        ...DEFAULT_OUTPUT_VIEW,
        card: document.createElement('article'),
        frame: 0,
      };
      state.card.className = 'runner-value-card';
      this.states.set(port, state);
      this.target.append(state.card);
    } else {
      const [block] = appendOutput(
        [state.block],
        port,
        value,
        textual,
        mimetype,
      );
      state.block = block!;
    }
    this.scheduleRender(state);
  }

  private scheduleRender(state: OutputState): void {
    if (typeof requestAnimationFrame !== 'function') {
      this.render(state);
      return;
    }
    if (state.frame) return;
    state.frame = requestAnimationFrame(() => {
      state.frame = 0;
      this.render(state);
    });
  }

  private render(state: OutputState): void {
    const {block} = state;
    const header = document.createElement('header');
    const name = document.createElement('code');
    name.className = 'runner-value-name';
    name.textContent = block.port;
    const media = document.createElement('span');
    media.className = 'runner-value-media';
    media.textContent = block.mimetype || 'untyped';
    const count = document.createElement('span');
    count.className = 'runner-value-count';
    count.textContent = `${block.count.toLocaleString()} ${block.count === 1 ? 'value' : 'values'}`;
    header.append(name, media, count);
    if (block.textual) {
      header.append(
        control(state.joined ? 'Joined' : 'Chunks', () => {
          state.joined = !state.joined;
          this.render(state);
        }),
        control(`Markdown ${state.markdown ? 'on' : 'off'}`, () => {
          state.markdown = !state.markdown;
          this.render(state);
        }),
        control(state.expanded ? 'Collapse' : 'Expand', () => {
          state.expanded = !state.expanded;
          this.render(state);
        }),
      );
    }
    header.append(control('Copy', () => void copyValue(state)));

    const body = document.createElement('div');
    body.className = `runner-value-body${state.expanded ? ' expanded' : ''}`;
    if (block.textual) {
      if (state.joined) renderText(body, block.text, state.markdown);
      else renderChunks(body, block.parts, block.mimetype);
    } else if (block.values.length === 1) {
      renderDebugValue(body, block.values[0], block.mimetype);
    } else {
      renderChunks(body, block.values, block.mimetype);
    }
    const children: Node[] = [header];
    if (block.omitted || block.textTruncated) {
      const limit = document.createElement('p');
      limit.className = 'runner-history-limit';
      limit.textContent = block.textTruncated
        ? `Oldest text truncated · ${block.count.toLocaleString()} updates received`
        : `Showing latest ${(block.textual ? block.parts : block.values).length} of ${block.count.toLocaleString()} values`;
      children.push(limit);
    }
    children.push(body);
    state.card.replaceChildren(...children);
  }
}

/** Render decoded JSON, text, images, or unrestricted bytes without guessing JSON. */
export function renderDebugValue(target: HTMLElement, value: unknown, mimetype = ''): void {
  target.replaceChildren();
  const presented = presentValue(value, mimetype);
  if (!isOk<ValuePresentation>(presented)) {
    const error = document.createElement('pre');
    error.className = 'runner-value-content';
    error.textContent = presented.message;
    target.append(error);
    return;
  }
  if (
    presented.kind === 'image' ||
    presented.kind === 'audio' ||
    presented.kind === 'video'
  ) {
    if (presented.kind === 'image') {
      const image = document.createElement('img');
      image.className = 'debug-image';
      image.alt = presented.mimetype;
      image.src = dataUrl(presented.bytes, presented.mimetype);
      target.append(image);
      return;
    }
    const media = document.createElement(presented.kind);
    media.className = 'debug-media';
    media.controls = true;
    media.src = dataUrl(presented.bytes, presented.mimetype);
    target.append(media);
    return;
  }
  if (presented.kind === 'binary') {
    const meta = document.createElement('p');
    meta.className = 'binary-meta';
    meta.textContent = `${presented.mimetype || 'binary data'} · ${formatBytes(presented.byteLength)}`;
    const preview = document.createElement('pre');
    preview.className = 'runner-value-content';
    preview.textContent = presented.preview;
    target.append(meta, preview);
    return;
  }
  const content = document.createElement('pre');
  content.className = 'runner-value-content';
  if (presented.kind === 'text') {
    content.textContent = presented.text;
  } else if (presented.kind === 'json') {
    renderJson(content, presented.text);
  }
  target.append(content);
}

function renderText(target: HTMLElement, text: string, markdown: boolean): void {
  if (markdown && looksLikeMarkdown(text)) {
    const content = document.createElement('div');
    content.className = 'markdown-body runner-markdown';
    content.innerHTML = renderMarkdown(text);
    target.append(content);
    return;
  }
  renderDebugValue(target, text, 'text/plain');
}

function renderChunks(target: HTMLElement, values: unknown[], mimetype: string): void {
  const feed = document.createElement('div');
  feed.className = 'runner-chunk-feed';
  for (const value of values.slice(-100)) {
    const row = document.createElement('div');
    row.className = 'runner-chunk';
    renderDebugValue(row, value, mimetype);
    feed.append(row);
  }
  target.append(feed);
}

function control(label: string, action: () => void): HTMLButtonElement {
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'runner-value-control';
  button.textContent = label;
  button.addEventListener('click', action);
  return button;
}

async function copyValue(state: OutputState): Promise<void> {
  await navigator.clipboard?.writeText(outputCopyText(state.block, state));
}

function dataUrl(bytes: Uint8Array, mimetype: string): string {
  let binary = '';
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return `data:${mimetype};base64,${btoa(binary)}`;
}

function formatBytes(bytes: number): string {
  return bytes < 1024 ? `${bytes} B` : `${(bytes / 1024).toFixed(1)} KiB`;
}
