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
import {StatusCode, isOk, okStatus, statusFromUnknown, type LogRecord, type Status} from '@curiositystack/a11';
import {
  FLOW_COMPLETION_DEBOUNCE_MS,
  FLOW_DIAGNOSTIC_DEBOUNCE_MS,
  FlowEditorController,
  RunAccumulator,
  countFlowDiagnostics,
  insertIndentedFlowNewline,
  retainRuns,
  type FlowLanguageRequest,
  type FlowServiceReply,
  type RunSnapshot,
} from '@curiositystack/a11/presentation';
import {renderMarkdown} from './markdown.js';
import {highlightFlow, requestFlowLanguage} from './bridge.js';
import {trashIcon} from './icons.js';

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
  private readonly sourceEditor = document.createElement('textarea');
  private readonly sourceGutter = document.createElement('div');
  private readonly sourceStatus = document.createElement('div');
  private readonly sourceCompletions = document.createElement('div');
  private readonly sourceDiagnostics = document.createElement('div');
  private readonly formatButton = document.createElement('button');
  private readonly inputs = document.createElement('div');
  private readonly headers = createJsonEditor({value: '{}'});
  private readonly outputs = document.createElement('div');
  private readonly logs = document.createElement('div');
  private readonly outputHeader = document.createElement('div');
  private readonly logHeader = document.createElement('div');
  private readonly inputFailure = document.createElement('div');
  private readonly runButton = document.createElement('button');
  private readonly stopButton = document.createElement('button');
  private readonly values = new Map<string, InputEditor>();
  private readonly outputPresenter = new OutputPresenter(this.outputs);
  private readonly connection = createConnectionStatus();
  private readonly session = new A11ChatSession(undefined, (notice) => this.connection.update(notice));
  private flow: RunnableFlow | null = null;
  private editor: FlowEditorController | null = null;
  private unsubscribeEditor: (() => void) | null = null;
  private checkTimer: ReturnType<typeof setTimeout> | null = null;
  private completeTimer: ReturnType<typeof setTimeout> | null = null;
  private runs: RunSnapshot[] = [];
  private selectedRunId: string | null = null;
  private activeRun: RunAccumulator | null = null;
  private cancelled = false;

  constructor(root: HTMLElement) {
    root.classList.add('flow-runner-view');
    const heading = document.createElement('header');
    this.title.textContent = 'Flow runner';
    this.location.className = 'runner-location';
    heading.append(this.title, this.location, this.connection.element);
    this.source.className = 'flow-source flow-editor-highlight';
    this.sourceEditor.className = 'flow-editor-input';
    this.sourceGutter.className = 'flow-editor-gutter';
    this.sourceEditor.setAttribute('aria-label', 'Flow source');
    this.sourceEditor.spellcheck = false;
    this.sourceStatus.className = 'flow-editor-status';
    this.sourceCompletions.className = 'flow-editor-completions';
    this.sourceDiagnostics.className = 'flow-editor-diagnostics';
    this.formatButton.type = 'button';
    this.formatButton.className = 'quiet flow-format';
    this.formatButton.textContent = 'Format';
    this.formatButton.addEventListener('click', () => void this.editor?.format());
    const editorSurface = document.createElement('div');
    editorSurface.className = 'flow-editor-surface';
    editorSurface.append(this.sourceGutter, this.source, this.sourceEditor, this.sourceCompletions);
    const editorFooter = document.createElement('footer');
    editorFooter.className = 'flow-editor-footer';
    editorFooter.append(this.sourceStatus, this.formatButton);
    const editor = document.createElement('div');
    editor.className = 'flow-editor';
    editor.append(editorSurface, editorFooter, this.sourceDiagnostics);
    this.sourceEditor.addEventListener('input', () => this.onSourceInput());
    this.sourceEditor.addEventListener('select', () => this.updateSelection());
    this.sourceEditor.addEventListener('scroll', () => {
      this.source.style.setProperty('--flow-editor-scroll-x', `${-this.sourceEditor.scrollLeft}px`);
      this.source.style.setProperty('--flow-editor-scroll-y', `${-this.sourceEditor.scrollTop}px`);
      this.sourceGutter.style.top = `${10 - this.sourceEditor.scrollTop}px`;
    });
    this.sourceEditor.addEventListener('keydown', (event) => this.onEditorKey(event));
    this.runButton.textContent = 'Run Flow';
    this.runButton.addEventListener('click', () => void this.run());
    this.stopButton.type = 'button';
    this.stopButton.className = 'ghost-button runner-stop';
    this.stopButton.textContent = 'Stop';
    this.stopButton.disabled = true;
    this.stopButton.addEventListener('click', () => {
      this.cancelled = true;
      this.session.cancelFlow();
      this.stopButton.disabled = true;
    });
    const controls = document.createElement('div');
    controls.className = 'runner-controls';
    const sourcePanel = section('Flow source', editor);
    sourcePanel.classList.add('runner-source-panel');
    sourcePanel.querySelector('h3')?.append(this.location);
    const inputPanel = document.createElement('section');
    inputPanel.className = 'runner-input-panel';
    const inputHeader = document.createElement('header');
    const inputTitle = document.createElement('div');
    const inputName = document.createElement('h3');
    inputName.textContent = 'Run flow';
    const inputSubtitle = document.createElement('p');
    inputSubtitle.textContent = 'Compiled Flow interface';
    inputTitle.append(inputName, inputSubtitle);
    const runControls = document.createElement('div');
    runControls.className = 'runner-run-controls';
    runControls.append(this.connection.element, this.runButton, this.stopButton);
    inputHeader.append(inputTitle, runControls);
    const inputBody = document.createElement('div');
    this.inputFailure.className = 'runner-input-failure';
    this.inputFailure.setAttribute('role', 'alert');
    this.inputFailure.hidden = true;
    const inputsHeading = document.createElement('h4');
    inputsHeading.textContent = 'Inputs';
    const headersHeading = document.createElement('summary');
    headersHeading.textContent = 'Headers';
    const headersPanel = document.createElement('details');
    headersPanel.className = 'runner-headers';
    headersPanel.append(headersHeading, this.headers.element);
    inputBody.append(this.inputFailure, inputsHeading, this.inputs, headersPanel);
    inputPanel.append(inputHeader, inputBody);
    controls.append(sourcePanel, inputPanel);
    const outputPanel = inspectorSection('Outputs', this.outputHeader, this.outputs);
    outputPanel.classList.add('runner-output-panel');
    this.outputs.classList.add('runner-inspector-content');
    const logPanel = inspectorSection('Logs', this.logHeader, this.logs);
    logPanel.classList.add('runner-log-panel');
    this.logs.classList.add('runner-inspector-content');
    root.append(
      heading,
      controls,
      outputPanel,
      logPanel,
    );
    this.showEmptyPanels();
    this.renderInspectorHeaders();
  }

  /** Replace the source and reconcile fields with its current native symbols. */
  openFlow(flow: RunnableFlow): void {
    this.flow = flow;
    this.title.textContent = flow.name;
    const inputName = this.runButton.parentElement?.querySelector('h3');
    if (inputName) inputName.textContent = flow.name;
    this.location.textContent = `${flow.path}:${flow.line + 1}`;
    if (!this.editor) {
      this.editor = new FlowEditorController({
        request: async (request: FlowLanguageRequest) => {
          try {
            return await requestFlowLanguage(request) as FlowServiceReply;
          } catch (error) {
            return statusFromUnknown(error, 'The IDE could not reach the Flow language service.');
          }
        },
      }, flow.source, flow.path);
      this.unsubscribeEditor = this.editor.subscribe(() => this.renderEditor());
    } else {
      this.editor.setDocument(flow.source, {start: 0, end: 0}, flow.path);
    }
    this.sourceEditor.value = flow.source;
    renderFlowSource(this.source, flow.source, flow.tokens ?? []);
    this.renderGutter(flow.source, []);
    this.renderEditor();
    this.scheduleLanguageRequests();
    const retained = new Map(this.values);
    this.values.clear();
    this.inputs.innerHTML = '';
    this.clearInputFailure();
    const inputPorts = flow.ports.filter((candidate) => candidate.direction === 'in');
    if (inputPorts.length === 0) {
      const empty = document.createElement('p');
      empty.className = 'runner-empty';
      empty.textContent = 'This Flow declares no inputs.';
      this.inputs.append(empty);
    }
    for (const port of inputPorts) {
      const label = document.createElement('label');
      label.className = 'runner-input-field';
      const description = document.createElement('span');
      description.className = 'runner-input-description';
      const name = document.createElement('span');
      name.className = 'runner-input-name';
      name.textContent = port.name;
      const type = document.createElement('span');
      type.className = 'runner-input-type';
      type.textContent = `${port.type}${port.stream ? ' stream' : ''}`;
      description.append(name, type);
      if (port.required) {
        const required = document.createElement('span');
        required.className = 'runner-input-required';
        required.textContent = 'required';
        description.append(required);
      }
      const previous = retained.get(port.name);
      const editor = previous && editorKind(port) === previous.element.dataset.kind
        ? previous
        : createInputEditor(port);
      editor.element.addEventListener('input', () => {
        editor.element.removeAttribute('aria-invalid');
        label.classList.remove('invalid');
        if (![...this.values.values()].some((candidate) => candidate.element.getAttribute('aria-invalid') === 'true')) {
          this.clearInputFailure();
        }
      });
      label.append(description, editor.element);
      this.inputs.append(label);
      this.values.set(port.name, editor);
    }
  }

  dispose(): void {
    if (this.checkTimer) clearTimeout(this.checkTimer);
    if (this.completeTimer) clearTimeout(this.completeTimer);
    this.unsubscribeEditor?.();
    this.editor?.dispose();
    this.session.halfClose();
  }

  private onSourceInput(): void {
    if (!this.editor) return;
    const selection = {
      start: this.sourceEditor.selectionStart,
      end: this.sourceEditor.selectionEnd,
    };
    const status = this.editor.setDocument(this.sourceEditor.value, selection, this.flow?.path ?? '');
    if (!isOk(status)) this.sourceStatus.textContent = status.message;
    if (this.flow) this.flow = {...this.flow, source: this.sourceEditor.value};
    this.scheduleLanguageRequests();
  }

  private updateSelection(): void {
    if (!this.editor) return;
    this.editor.setDocument(this.sourceEditor.value, {
      start: this.sourceEditor.selectionStart,
      end: this.sourceEditor.selectionEnd,
    }, this.flow?.path ?? '');
  }

  private scheduleLanguageRequests(): void {
    if (!this.editor) return;
    if (this.checkTimer) clearTimeout(this.checkTimer);
    if (this.completeTimer) clearTimeout(this.completeTimer);
    this.checkTimer = setTimeout(() => void this.editor?.check(), FLOW_DIAGNOSTIC_DEBOUNCE_MS);
    this.completeTimer = setTimeout(
      () => void this.editor?.complete(this.sourceEditor.selectionEnd),
      FLOW_COMPLETION_DEBOUNCE_MS,
    );
    const source = this.sourceEditor.value;
    void highlightFlow(source).then((tokens) => {
      if (this.sourceEditor.value === source) renderFlowSource(this.source, source, tokens);
    }).catch(() => undefined);
  }

  private onEditorKey(event: KeyboardEvent): void {
    if (!this.editor) return;
    if (event.key === 'Enter' && !event.shiftKey && !event.ctrlKey && !event.metaKey) {
      event.preventDefault();
      const edit = insertIndentedFlowNewline(
        this.sourceEditor.value,
        this.sourceEditor.selectionStart,
        this.sourceEditor.selectionEnd,
      );
      this.sourceEditor.value = edit.value;
      this.sourceEditor.setSelectionRange(edit.caret, edit.caret);
      this.onSourceInput();
      return;
    }
    if (event.key === 'Escape') {
      this.sourceCompletions.replaceChildren();
      return;
    }
    if ((event.ctrlKey || event.metaKey) && event.key === ' ') {
      event.preventDefault();
      void this.editor.complete(this.sourceEditor.selectionEnd);
    }
  }

  private renderEditor(): void {
    const snapshot = this.editor?.getSnapshot();
    if (!snapshot) return;
    if (this.sourceEditor.value !== snapshot.document.source) {
      this.sourceEditor.value = snapshot.document.source;
      this.sourceEditor.setSelectionRange(
        snapshot.document.selection.start,
        snapshot.document.selection.end,
      );
      if (this.flow) this.flow = {...this.flow, source: snapshot.document.source};
      this.scheduleLanguageRequests();
    }
    const counts = countFlowDiagnostics(snapshot.diagnostics);
    this.renderGutter(snapshot.document.source, snapshot.diagnostics);
    this.sourceStatus.className = `flow-editor-status${snapshot.failure ? ' failed' : ''}`;
    this.sourceStatus.textContent = snapshot.failure
      ? snapshot.failure.message
      : snapshot.requests.check
        ? 'Checking Flow…'
        : counts.error || counts.warning || counts.suggestion
          ? `${counts.error} errors · ${counts.warning} warnings · ${counts.suggestion} suggestions`
          : 'Flow is valid';
    this.formatButton.disabled = snapshot.requests.format;
    const proposals = snapshot.completion?.proposals.slice(0, 12) ?? [];
    this.sourceCompletions.replaceChildren(...proposals.map((proposal) => {
      const option = document.createElement('button');
      option.type = 'button';
      option.className = 'flow-completion';
      const name = document.createElement('strong');
      name.textContent = proposal.name;
      const kind = document.createElement('span');
      kind.textContent = proposal.type || proposal.kind;
      option.append(name, kind);
      option.addEventListener('mousedown', (event) => event.preventDefault());
      option.addEventListener('click', () => {
        const status = this.editor?.acceptCompletion(proposal);
        if (status && !isOk(status)) this.sourceStatus.textContent = status.message;
        this.sourceEditor.focus();
      });
      return option;
    }));
    this.sourceCompletions.classList.toggle('visible', proposals.length > 0 && document.activeElement === this.sourceEditor);
    this.sourceDiagnostics.replaceChildren(...snapshot.diagnostics.map((diagnostic) => {
      const row = document.createElement('div');
      row.className = `flow-diagnostic ${diagnostic.severity}`;
      const location = document.createElement('span');
      location.textContent = `${diagnostic.range.start.line + 1}:${diagnostic.range.start.column + 1}`;
      const message = document.createElement('span');
      message.textContent = diagnostic.message;
      row.append(location, message);
      for (const fix of diagnostic.fixes ?? []) {
        const apply = document.createElement('button');
        apply.type = 'button';
        apply.className = 'quiet';
        apply.textContent = fix.label;
        apply.addEventListener('click', () => {
          const status = this.editor?.applyFix(fix.edits);
          if (status && !isOk(status)) this.sourceStatus.textContent = status.message;
        });
        row.append(apply);
      }
      return row;
    }));
  }

  private renderGutter(
    source: string,
    diagnostics: readonly {severity: string; message: string; range: {start: {line: number}}}[],
  ): void {
    const byLine = new Map<number, typeof diagnostics[number]>();
    for (const diagnostic of diagnostics) {
      const line = diagnostic.range.start.line;
      const held = byLine.get(line);
      if (!held || (held.severity !== 'error' && diagnostic.severity === 'error')) byLine.set(line, diagnostic);
    }
    const count = Math.max(1, source.split(/\r?\n/).length);
    this.sourceGutter.replaceChildren(...Array.from({length: count}, (_, index) => {
      const line = document.createElement('span');
      line.textContent = String(index + 1);
      const diagnostic = byLine.get(index);
      if (diagnostic) {
        line.className = diagnostic.severity === 'error' ? 'error' : 'warning';
        line.title = diagnostic.message;
      }
      return line;
    }));
  }

  private async run(): Promise<void> {
    if (!this.flow) return;
    const required = this.flow.ports.filter((port) =>
      port.direction === 'in' && port.required && !this.values.get(port.name)?.text().trim(),
    );
    if (required.length) {
      const names = required.map((port) => port.name);
      this.inputFailure.textContent = names.length === 1
        ? `${names[0]} is required before this Flow can run.`
        : `${names.join(', ')} are required before this Flow can run.`;
      this.inputFailure.hidden = false;
      for (const port of required) {
        const editor = this.values.get(port.name);
        editor?.element.setAttribute('aria-invalid', 'true');
        editor?.element.closest('.runner-input-field')?.classList.add('invalid');
      }
      this.values.get(required[0]!.name)?.element.focus();
      return;
    }
    this.clearInputFailure();
    this.runButton.disabled = true;
    this.stopButton.disabled = false;
    this.cancelled = false;
    this.outputPresenter.clear();
    this.logs.replaceChildren();
    let finalStatus: Status = okStatus();
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
      const run = new RunAccumulator({
        id: typeof crypto !== 'undefined' && 'randomUUID' in crypto
          ? crypto.randomUUID() : `${Date.now()}-${Math.random()}`,
        action: `Flow · ${this.flow.name}`,
        inputs,
        headers,
        startedAt: Date.now(),
      });
      this.activeRun = run;
      this.selectedRunId = run.getSnapshot().id;
      run.subscribe(() => this.recordRun(run.getSnapshot()));
      this.recordRun(run.getSnapshot());
      const outputSinks = Object.fromEntries(
        this.flow.ports
          .filter((port) => port.direction === 'out')
          .map((port) => [port.name, (value: unknown) => {
            const mimetype = outputMimetype(port);
            this.outputPresenter.append(port.name, value, mimetype);
            run.accept({kind: 'output', port: port.name, value, mimetype, textual: mimetype.startsWith('text/')});
          }]),
      );
      run.accept({kind: 'stage', stage: 'working'});
      await this.session.runFlowSource(
        this.flow.source,
        this.flow.name,
        inputs,
        inputMimetypes,
        headers,
        outputSinks,
        (line, record) => {
          this.appendLog(line, false, record);
          run.accept({kind: 'log', log: {
            level: record.level,
            text: line,
            channel: record.channel,
            at: record.timestamp?.getTime(),
            callId: record.actionId,
          }});
        },
      );
      run.accept({kind: 'stage', stage: 'draining'});
      if (!this.outputs.childElementCount) this.appendEmpty(this.outputs, 'No output values.');
      if (!this.logs.childElementCount) this.appendEmpty(this.logs, 'Nothing logged.');
    } catch (error) {
      finalStatus = {
        code: this.cancelled ? StatusCode.CANCELLED : StatusCode.UNKNOWN,
        message: this.cancelled
          ? 'The Flow run was cancelled.'
          : error instanceof Error ? error.message : String(error),
      };
      this.appendLog(finalStatus.message, true);
    } finally {
      if (this.activeRun) {
        this.activeRun.accept({
          kind: 'ended',
          state: this.cancelled ? 'cancelled' : isOk(finalStatus) ? 'succeeded' : 'failed',
          at: Date.now(),
          status: finalStatus,
        });
      }
      this.activeRun = null;
      this.runButton.disabled = false;
      this.stopButton.disabled = true;
      this.cancelled = false;
    }
  }

  private recordRun(run: RunSnapshot): Status {
    this.runs = retainRuns(this.runs, run);
    this.renderInspectorHeaders();
    return okStatus();
  }

  private renderInspectorHeaders(): void {
    this.renderInspectorHeader(this.outputHeader, 'Outputs');
    this.renderInspectorHeader(this.logHeader, 'Logs');
  }

  private renderInspectorHeader(target: HTMLElement, name: string): void {
    const title = document.createElement('h3');
    title.textContent = name;
    const controls = document.createElement('div');
    controls.className = 'runner-inspector-controls';
    if (this.runs.length) {
      const select = document.createElement('select');
      select.className = 'runner-run-selector';
      select.setAttribute('aria-label', `${name} run`);
      for (const [index, run] of this.runs.entries()) {
        const option = document.createElement('option');
        option.value = run.id;
        const time = new Date(run.startedAt).toLocaleTimeString([], {
          hour: '2-digit', minute: '2-digit', second: '2-digit',
        });
        option.textContent = index === 0 ? `Latest · ${time}` : `Run ${this.runs.length - index} · ${time}`;
        option.selected = run.id === this.selectedRunId;
        select.append(option);
      }
      select.addEventListener('change', () => {
        const run = this.runs.find((candidate) => candidate.id === select.value);
        if (run) this.showRun(run);
      });
      const clear = document.createElement('button');
      clear.type = 'button';
      clear.className = 'runner-clear-runs';
      clear.setAttribute('aria-label', 'Clear run history');
      clear.title = 'Clear run history';
      clear.append(trashIcon());
      clear.disabled = this.activeRun !== null;
      clear.addEventListener('click', () => {
        this.runs = [];
        this.selectedRunId = null;
        this.outputPresenter.clear();
        this.logs.replaceChildren();
        this.showEmptyPanels();
        this.renderInspectorHeaders();
      });
      controls.append(select, clear);
    }
    const selected = this.runs.find((run) => run.id === this.selectedRunId);
    const status = document.createElement('div');
    status.className = `runner-inspector-status${selected ? ` ${selected.state}` : ''}`;
    if (selected) {
      const mark = document.createElement('span');
      mark.textContent = selected.state === 'running' ? '●' : selected.state === 'succeeded' ? '✓' : '×';
      const text = document.createElement('span');
      text.textContent = selected.state === 'running'
        ? selected.stage
        : `${selected.state} · ${(selected.endedAt ?? Date.now()) - selected.startedAt} ms`;
      status.append(mark, text);
    }
    target.replaceChildren(title, controls, status);
  }

  private showRun(run: RunSnapshot): void {
    this.selectedRunId = run.id;
    this.outputPresenter.show(run.outputs);
    this.logs.replaceChildren();
    for (const log of run.logs) this.appendLog(log.text, log.level === 'error' || log.level === 'critical');
    if (run.status && !isOk(run.status)) this.appendLog(run.status.message, true);
    if (!run.outputs.length) this.appendEmpty(this.outputs, 'No output values.');
    if (!run.logs.length && (!run.status || isOk(run.status))) this.appendEmpty(this.logs, 'Nothing logged.');
    this.renderInspectorHeaders();
  }

  private clearInputFailure(): void {
    this.inputFailure.hidden = true;
    this.inputFailure.textContent = '';
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
  const content = document.createElement('span');
  content.className = 'flow-source-content';
  let at = 0;
  for (const token of tokens) {
    if (token.start < at || token.end <= token.start || token.end > source.length) continue;
    content.append(document.createTextNode(source.slice(at, token.start)));
    const span = document.createElement('span');
    span.className = `flow-token flow-token-${token.kind}`;
    span.textContent = source.slice(token.start, token.end);
    content.append(span);
    at = token.end;
  }
  content.append(document.createTextNode(source.slice(at)));
  target.replaceChildren(content);
}

function section(name: string, content: HTMLElement): HTMLElement {
  const element = document.createElement('section');
  const heading = document.createElement('h3');
  heading.textContent = name;
  element.append(heading, content);
  return element;
}

function inspectorSection(name: string, header: HTMLElement, content: HTMLElement): HTMLElement {
  const element = document.createElement('section');
  header.className = 'runner-inspector-header';
  const heading = document.createElement('h3');
  heading.textContent = name;
  header.append(heading);
  element.append(header, content);
  return element;
}

export function mountFlowRunner(root: HTMLElement): MountedView {
  const runner = new FlowRunnerView(root);
  return {openFlow: (flow) => runner.openFlow(flow), dispose: () => runner.dispose()};
}
