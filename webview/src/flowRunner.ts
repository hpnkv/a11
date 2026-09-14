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

import {A11ChatSession} from './a11client.js';
import type {MountedView} from './mount.js';
import {createJsonEditor} from './jsonEditor.js';
import {createConnectionStatus} from './connectionStatus.js';
import {OutputPresenter} from './outputPresentation.js';
import type {LogRecord} from '@curiositystack/a11';
import {renderMarkdown} from './markdown.js';

interface InputEditor {
  readonly element: HTMLElement;
  text(): string;
  read(): unknown;
}

export interface FlowPort {
  name: string;
  direction: 'in' | 'out';
  type: string;
  stream: boolean;
  required: boolean;
}

export interface RunnableFlow {
  source: string;
  name: string;
  path: string;
  line: number;
  ports: FlowPort[];
  /** Classifications from the native Flow language service. */
  tokens?: Array<{start: number; end: number; kind: string}>;
}

/** A document-backed Flow runner, updated whenever its editor source changes. */
export class FlowRunnerView {
  private readonly title = document.createElement('h2');
  private readonly location = document.createElement('span');
  private readonly source = document.createElement('pre');
  private readonly inputs = document.createElement('div');
  private readonly headers = createJsonEditor({value: '{}'});
  private readonly outputs = document.createElement('div');
  private readonly logs = document.createElement('div');
  private readonly runButton = document.createElement('button');
  private readonly values = new Map<string, InputEditor>();
  private readonly outputPresenter = new OutputPresenter(this.outputs);
  private readonly connection = createConnectionStatus();
  private readonly session = new A11ChatSession(undefined, (notice) => this.connection.update(notice));
  private flow: RunnableFlow | null = null;

  constructor(root: HTMLElement) {
    root.classList.add('flow-runner-view');
    const heading = document.createElement('header');
    this.title.textContent = 'Flow runner';
    this.location.className = 'runner-location';
    heading.append(this.title, this.location, this.connection.element);
    this.source.className = 'flow-source';
    this.runButton.textContent = 'Run Flow';
    this.runButton.addEventListener('click', () => void this.run());
    const controls = document.createElement('div');
    controls.className = 'runner-controls';
    const sourcePanel = section('Flow source', this.source);
    sourcePanel.classList.add('runner-source-panel');
    const inputPanel = document.createElement('section');
    inputPanel.className = 'runner-input-panel';
    const inputHeader = document.createElement('header');
    const inputTitle = document.createElement('div');
    const inputName = document.createElement('h3');
    inputName.textContent = 'Run flow';
    const inputSubtitle = document.createElement('p');
    inputSubtitle.textContent = 'Compiled Flow interface';
    inputTitle.append(inputName, inputSubtitle);
    inputHeader.append(inputTitle, this.runButton);
    const inputBody = document.createElement('div');
    const inputsHeading = document.createElement('h4');
    inputsHeading.textContent = 'Inputs';
    const headersHeading = document.createElement('h4');
    headersHeading.textContent = 'Headers';
    inputBody.append(inputsHeading, this.inputs, headersHeading, this.headers.element);
    inputPanel.append(inputHeader, inputBody);
    controls.append(sourcePanel, inputPanel);
    const outputPanel = section('Outputs', this.outputs);
    outputPanel.classList.add('runner-output-panel');
    const logPanel = section('Logs', this.logs);
    logPanel.classList.add('runner-log-panel');
    root.append(
      heading,
      controls,
      outputPanel,
      logPanel,
    );
    this.showEmptyPanels();
  }

  /** Replace the source and reconcile fields with its current native symbols. */
  openFlow(flow: RunnableFlow): void {
    this.flow = flow;
    this.title.textContent = flow.name;
    const inputName = this.runButton.parentElement?.querySelector('h3');
    if (inputName) inputName.textContent = flow.name;
    this.location.textContent = `${flow.path}:${flow.line + 1}`;
    renderFlowSource(this.source, flow.source, flow.tokens ?? []);
    const retained = new Map(this.values);
    this.values.clear();
    this.inputs.innerHTML = '';
    const inputPorts = flow.ports.filter((candidate) => candidate.direction === 'in');
    if (inputPorts.length === 0) {
      const empty = document.createElement('p');
      empty.className = 'runner-empty';
      empty.textContent = 'This Flow declares no inputs.';
      this.inputs.append(empty);
    }
    for (const port of inputPorts) {
      const label = document.createElement('label');
      label.textContent = `${port.name} · ${port.type}${port.stream ? ' stream' : ''}`;
      const previous = retained.get(port.name);
      const editor = previous && editorKind(port) === previous.element.dataset.kind
        ? previous
        : createInputEditor(port);
      label.append(editor.element);
      this.inputs.append(label);
      this.values.set(port.name, editor);
    }
  }

  private async run(): Promise<void> {
    if (!this.flow) return;
    this.runButton.disabled = true;
    this.outputPresenter.clear();
    this.logs.replaceChildren();
    try {
      const inputs: Record<string, unknown[]> = {};
      const inputMimetypes: Record<string, string> = {};
      for (const [name, editor] of this.values) {
        if (!editor.text().trim()) continue;
        const value = editor.read();
        const port = this.flow.ports.find((candidate) => candidate.name === name);
        inputs[name] = port?.stream && Array.isArray(value) ? value : [value];
        const mimetype = port && inputMimetype(port);
        if (mimetype) inputMimetypes[name] = mimetype;
      }
      const parsedHeaders = this.headers.read();
      if (!parsedHeaders || Array.isArray(parsedHeaders) || typeof parsedHeaders !== 'object') {
        throw new Error('Headers must be a JSON object of names and string values.');
      }
      const headers = Object.fromEntries(
        Object.entries(parsedHeaders).map(([name, value]) => [name, String(value)]),
      );
      const outputSinks = Object.fromEntries(
        this.flow.ports
          .filter((port) => port.direction === 'out')
          .map((port) => [port.name, (value: unknown) => {
            this.outputPresenter.append(port.name, value, outputMimetype(port));
          }]),
      );
      await this.session.runFlowSource(
        this.flow.source,
        this.flow.name,
        inputs,
        inputMimetypes,
        headers,
        outputSinks,
        (line, record) => this.appendLog(line, false, record),
      );
      if (!this.outputs.childElementCount) this.appendEmpty(this.outputs, 'No output values.');
      if (!this.logs.childElementCount) this.appendEmpty(this.logs, 'Nothing logged.');
    } catch (error) {
      this.appendLog(error instanceof Error ? error.message : String(error), true);
    } finally {
      this.runButton.disabled = false;
    }
  }

  private appendLog(line: string, failed = false, record?: LogRecord): void {
    const entry = document.createElement('article');
    entry.className = `runner-log-entry${failed ? ' failed' : ''}`;
    if (record) {
      const header = document.createElement('header');
      const level = document.createElement('span');
      level.className = `runner-log-level ${record.level}`;
      level.textContent = record.level;
      const channel = document.createElement('code');
      channel.textContent = record.channel;
      const where = document.createElement('span');
      where.className = 'runner-log-where';
      where.textContent = record.file
        ? `${record.file.split('/').at(-1)}${record.lineno === null ? '' : `:${record.lineno}`}`
        : '';
      const time = document.createElement('time');
      time.textContent = record.timestamp?.toLocaleTimeString() ?? '';
      header.append(level, channel, where, time);
      entry.append(header);
    }
    const content = document.createElement(line.includes('```') ? 'div' : 'pre');
    if (content.tagName === 'DIV') {
      content.className = 'runner-log-markdown markdown-body';
      content.innerHTML = renderMarkdown(line);
    } else {
      content.textContent = line;
    }
    entry.append(content);
    this.logs.append(entry);
  }

  private appendEmpty(target: HTMLElement, text: string): void {
    const empty = document.createElement('p');
    empty.className = 'runner-empty runner-panel-empty';
    empty.textContent = text;
    target.append(empty);
  }

  private showEmptyPanels(): void {
    this.appendEmpty(this.outputs, 'Outputs will appear here when the Flow runs.');
    this.appendEmpty(this.logs, 'Logs will appear here when the Flow runs.');
  }
}

function createInputEditor(port: FlowPort): InputEditor {
  const kind = editorKind(port);
  if (kind === 'json') {
    const editor = createJsonEditor({
      placeholder: port.required ? 'Required JSON value' : 'Optional JSON value',
    });
    editor.element.dataset.kind = kind;
    return editor;
  }
  const textarea = document.createElement('textarea');
  textarea.className = 'field-input runner-text-input';
  textarea.dataset.kind = kind;
  textarea.placeholder = kind === 'bytes'
    ? (port.required ? 'Required base64 bytes' : 'Optional base64 bytes')
    : (port.required ? 'Required text' : 'Optional text');
  return {
    element: textarea,
    text: () => textarea.value,
    read: () => kind === 'bytes' ? decodeBase64(textarea.value) : textarea.value,
  };
}

function editorKind(port: FlowPort): 'text' | 'bytes' | 'json' {
  const type = port.type.trim().toLowerCase();
  if (type === 'string' || type === 'text' || type.startsWith('text/')) return 'text';
  if (type === 'bytes') return 'bytes';
  return 'json';
}

function inputMimetype(port: FlowPort): string {
  const kind = editorKind(port);
  if (kind === 'text') return 'text/plain';
  if (kind === 'bytes') return 'application/x-msgpack';
  return '';
}

function outputMimetype(port: FlowPort): string {
  const type = port.type.trim().toLowerCase();
  if (type === 'string' || type === 'text') return 'text/plain';
  if (type === 'bytes') return 'application/x-msgpack';
  return port.type;
}

function decodeBase64(value: string): Uint8Array {
  const packed = value.replace(/\s+/g, '');
  if (!/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(packed)) {
    throw new Error('Bytes inputs must be valid base64.');
  }
  const binary = atob(packed);
  return Uint8Array.from(binary, (character) => character.charCodeAt(0));
}

/** Paint native semantic ranges without reimplementing Flow in the frontend. */
export function renderFlowSource(
  target: HTMLElement,
  source: string,
  tokens: Array<{start: number; end: number; kind: string}>,
): void {
  target.replaceChildren();
  let at = 0;
  for (const token of tokens) {
    if (token.start < at || token.end <= token.start || token.end > source.length) continue;
    target.append(document.createTextNode(source.slice(at, token.start)));
    const span = document.createElement('span');
    span.className = `flow-token flow-token-${token.kind}`;
    span.textContent = source.slice(token.start, token.end);
    target.append(span);
    at = token.end;
  }
  target.append(document.createTextNode(source.slice(at)));
}

function section(name: string, content: HTMLElement): HTMLElement {
  const element = document.createElement('section');
  const heading = document.createElement('h3');
  heading.textContent = name;
  element.append(heading, content);
  return element;
}

export function mountFlowRunner(root: HTMLElement): MountedView {
  const runner = new FlowRunnerView(root);
  return {openFlow: (flow) => runner.openFlow(flow)};
}
