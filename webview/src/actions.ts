/** Copyright 2026 The A11 Authors. Licensed under the Apache License, Version 2.0. */

import {
  StatusCode,
  isOk,
  okStatus,
  type LogRecord,
  type SchemaEntry,
  type Status,
} from '@curiositystack/a11';
import {
  ActionCatalogueController,
  RunAccumulator,
  headerFieldsFor,
  retainRuns,
  type RunSnapshot,
} from '@curiositystack/a11/presentation';

import {A11ChatSession} from './a11client.js';
import {listActions, runAction, type ActionDescriptor, type PortDescriptor} from './bridge.js';
import {createConnectionStatus} from './connectionStatus.js';
import type {MountedView} from './mount.js';
import {OutputPresenter} from './outputPresentation.js';
import {createPortInput, describeSchema, type PortInput} from './portForm.js';
import {trashIcon} from './icons.js';

type ActionSource = 'gateway' | 'ide';
interface CatalogueEntry { schema: SchemaEntry; source: ActionSource }

/** Studio-shaped Actions workspace rendered with IDE-native DOM and styling. */
class ActionExplorer {
  private readonly list = document.createElement('div');
  private readonly detail = document.createElement('div');
  private readonly search = document.createElement('input');
  private readonly connection = createConnectionStatus();
  private readonly session = new A11ChatSession(undefined, (notice) => this.connection.update(notice));
  private catalogue: ActionCatalogueController | null = null;
  private entries = new Map<string, CatalogueEntry>();
  private runs: RunSnapshot[] = [];
  private accumulators = new Map<string, RunAccumulator>();
  private selectedRunId: string | null = null;
  private disposeCatalogue: (() => void) | null = null;
  private activeRunId: string | null = null;
  private activeCancelled = false;

  constructor(root: HTMLElement) {
    root.classList.add('actions-view');
    this.list.className = 'action-list';
    this.detail.className = 'action-detail';
    this.search.type = 'search';
    this.search.className = 'action-search';
    this.search.placeholder = 'Filter actions';
    this.search.setAttribute('aria-label', 'Filter actions');
    this.search.addEventListener('input', () => {
      const status = this.catalogue?.setQuery(this.search.value) ?? okStatus();
      if (!isOk(status)) this.showFailure(status.message);
    });
    root.append(this.list, this.detail);
  }

  async load(): Promise<void> {
    this.renderLoading();
    let remote: SchemaEntry[] = [];
    let remoteFailure = '';
    try {
      remote = (await this.session.listGatewayActions()).actions;
    } catch (error) {
      remoteFailure = errorMessage(error);
    }
    let local: ActionDescriptor[] = [];
    try {
      local = await listActions();
    } catch (error) {
      if (!remoteFailure) remoteFailure = errorMessage(error);
    }
    for (const schema of remote) this.entries.set(schema.name, {schema, source: 'gateway'});
    for (const descriptor of local) {
      if (!this.entries.has(descriptor.name)) {
        const schema = descriptorToSchema(descriptor);
        this.entries.set(schema.name, {schema, source: 'ide'});
      }
    }
    if (this.entries.size === 0) {
      this.showFailure(remoteFailure || 'No actions are available.');
      return;
    }
    this.catalogue = new ActionCatalogueController({
      format: 'a11.actions/v1',
      actions: [...this.entries.values()].map((entry) => entry.schema),
    });
    this.disposeCatalogue = this.catalogue.subscribe(() => {
      this.renderList(remoteFailure);
      this.renderSelectedAction();
    });
    this.renderList(remoteFailure);
    this.renderSelectedAction();
  }

  dispose(): void {
    this.disposeCatalogue?.();
    this.catalogue?.dispose();
    for (const accumulator of this.accumulators.values()) accumulator.dispose();
    this.session.halfClose();
  }

  private renderLoading(): void {
    const status = document.createElement('p');
    status.className = 'action-loading';
    status.textContent = 'Connecting to A11…';
    this.list.replaceChildren(status);
    this.detail.replaceChildren();
  }

  private renderList(remoteFailure = ''): void {
    const snapshot = this.catalogue?.getSnapshot();
    if (!snapshot) return;
    const header = document.createElement('header');
    const title = document.createElement('strong');
    title.textContent = 'A11 Actions';
    header.append(title, this.connection.element);
    if (remoteFailure) {
      const notice = document.createElement('p');
      notice.className = 'action-catalogue-notice';
      notice.textContent = 'Gateway catalogue unavailable; showing IDE actions.';
      notice.title = remoteFailure;
      header.append(notice);
    }
    const items = snapshot.visible.map((schema) => {
      const item = document.createElement('button');
      item.type = 'button';
      item.className = 'action-item';
      item.classList.toggle('selected', schema.name === snapshot.selected);
      const source = this.entries.get(schema.name)?.source;
      const name = document.createElement('span');
      name.className = 'action-item-name';
      name.textContent = schema.name;
      const badge = document.createElement('span');
      badge.className = 'action-source';
      badge.textContent = source === 'ide' ? 'IDE' : 'A11';
      const description = document.createElement('span');
      description.className = 'action-item-desc';
      description.textContent = schema.description ?? '';
      item.append(name, badge, description);
      item.addEventListener('click', () => {
        const status = this.catalogue?.select(schema.name) ?? okStatus();
        if (!isOk(status)) this.showFailure(status.message);
      });
      return item;
    });
    const empty = emptyLine('No actions match this filter.');
    empty.classList.add('action-empty');
    this.list.replaceChildren(header, this.search, ...(items.length ? items : [empty]));
  }

  private renderSelectedAction(): void {
    const name = this.catalogue?.getSnapshot().selected;
    const entry = name ? this.entries.get(name) : undefined;
    if (!entry) {
      this.detail.replaceChildren();
      return;
    }
    const schema = entry.schema;
    const heading = document.createElement('header');
    heading.className = 'action-detail-heading';
    const title = document.createElement('h2');
    title.textContent = schema.name;
    const source = document.createElement('span');
    source.className = 'action-detail-source';
    source.textContent = entry.source === 'ide' ? 'IDE action' : 'Gateway action';
    heading.append(title, source);
    const description = document.createElement('p');
    description.className = 'action-description';
    description.textContent = schema.description ?? '';

    const inputsPanel = document.createElement('section');
    inputsPanel.className = 'action-editor-panel';
    const inputsTitle = document.createElement('h3');
    inputsTitle.textContent = 'Inputs';
    inputsPanel.append(inputsTitle);
    const editors = new Map<string, PortInput>();
    for (const port of schema.inputs ?? []) {
      const editor = createPortInput(portToDescriptor(port));
      editors.set(port.name, editor);
      inputsPanel.append(editor.element);
    }
    if (!schema.inputs?.length) inputsPanel.append(emptyLine('This action takes no inputs.'));

    const headersPanel = document.createElement('details');
    headersPanel.className = 'action-editor-panel action-headers-panel';
    const headersTitle = document.createElement('summary');
    headersTitle.textContent = 'Headers';
    headersPanel.append(headersTitle);
    const headersBody = document.createElement('div');
    headersBody.className = 'action-header-fields';
    const headerEditors = new Map<string, HTMLInputElement>();
    const fields = headerFieldsFor(schema);
    if (isOk(fields)) {
      for (const field of fields) {
        const row = document.createElement('label');
        row.className = 'action-header-row';
        const label = document.createElement('span');
        label.textContent = field.name;
        const input = document.createElement('input');
        input.className = 'field-input';
        input.type = /key|token|secret|password|authorization/i.test(field.name) ? 'password' : 'text';
        input.value = field.fallback ?? '';
        input.placeholder = field.description;
        row.append(label, input);
        headerEditors.set(field.name, input);
        headersBody.append(row);
      }
    }
    const extraHeaderEditors: Array<{row: HTMLElement; name: HTMLInputElement; value: HTMLInputElement}> = [];
    const addHeader = document.createElement('button');
    addHeader.type = 'button';
    addHeader.className = 'quiet action-add-header';
    addHeader.textContent = '+ Add header';
    addHeader.addEventListener('click', () => {
      const row = document.createElement('div');
      row.className = 'action-extra-header-row';
      const name = document.createElement('input');
      name.className = 'field-input mono';
      name.placeholder = 'Header name';
      name.setAttribute('aria-label', 'Header name');
      const value = document.createElement('input');
      value.className = 'field-input';
      value.placeholder = 'Value';
      value.setAttribute('aria-label', 'Header value');
      const remove = document.createElement('button');
      remove.type = 'button';
      remove.className = 'quiet';
      remove.textContent = '×';
      remove.setAttribute('aria-label', 'Remove header');
      remove.addEventListener('click', () => {
        const index = extraHeaderEditors.findIndex((candidate) => candidate.row === row);
        if (index >= 0) extraHeaderEditors.splice(index, 1);
        row.remove();
      });
      row.append(name, value, remove);
      headersBody.insertBefore(row, addHeader);
      extraHeaderEditors.push({row, name, value});
      name.focus();
    });
    headersBody.append(addHeader);
    headersPanel.append(headersBody);

    const controls = document.createElement('div');
    controls.className = 'action-controls';
    const run = document.createElement('button');
    run.type = 'button';
    run.className = 'run-button';
    run.textContent = 'Run';
    const timing = document.createElement('span');
    timing.className = 'timing';
    controls.append(run, timing);
    run.addEventListener('click', () => void this.run(entry, editors, () => {
      const declared = [...headerEditors].map(([name, input]) => [name, input.value] as const);
      const extra = extraHeaderEditors.map(({name, value}) => [name.value.trim(), value.value] as const);
      return Object.fromEntries([...declared, ...extra].filter(([name, value]) => name && value));
    }, run, timing));

    this.detail.replaceChildren(
      heading, description, inputsPanel, headersPanel, outputsSection(schema), controls,
    );
    this.renderRunInspector();
  }

  private async run(
    entry: CatalogueEntry,
    editors: ReadonlyMap<string, PortInput>,
    readHeaders: () => Record<string, string>,
    button: HTMLButtonElement,
    timing: HTMLElement,
  ): Promise<void> {
    const inputs: Record<string, unknown> = {};
    try {
      for (const [name, editor] of editors) {
        let value: unknown;
        try {
          value = editor.read();
        } catch (error) {
          editor.element.classList.add('invalid');
          editor.element.setAttribute('aria-invalid', 'true');
          editor.element.querySelector<HTMLElement>('input, select, textarea, button')?.focus();
          throw error;
        }
        if (value !== undefined) inputs[name] = value;
      }
    } catch (error) {
      this.showInlineFailure(errorMessage(error));
      return;
    }
    const headers = readHeaders();
    const recorded = Object.fromEntries(
      Object.entries(inputs).map(([name, value]) => [name, Array.isArray(value) ? value : [value]]),
    );
    const id = typeof crypto !== 'undefined' && 'randomUUID' in crypto
      ? crypto.randomUUID() : `${Date.now()}-${Math.random()}`;
    const accumulator = new RunAccumulator({
      id, action: entry.schema.name, inputs: recorded, headers, startedAt: Date.now(),
    });
    this.accumulators.set(id, accumulator);
    accumulator.subscribe(() => this.recordRun(accumulator.getSnapshot()));
    this.activeRunId = id;
    this.activeCancelled = false;
    this.selectedRunId = id;
    this.recordRun(accumulator.getSnapshot());
    button.disabled = true;
    const cancel = document.createElement('button');
    cancel.type = 'button';
    cancel.className = 'ghost-button action-cancel';
    cancel.textContent = 'Stop';
    cancel.addEventListener('click', () => {
      this.activeCancelled = true;
      this.session.cancelGatewayAction();
      cancel.disabled = true;
      timing.textContent = 'stopping…';
    });
    button.after(cancel);
    timing.textContent = 'dispatching…';
    let finalStatus: Status = okStatus();
    try {
      if (entry.source === 'ide') {
        accumulator.accept({kind: 'stage', stage: 'working'});
        const result = await runAction(entry.schema.name, inputs);
        accumulator.accept({kind: 'output', port: 'result', value: result, mimetype: 'application/json', textual: false});
      } else {
        await this.session.runGatewayAction(entry.schema, inputs, headers, {
          onStage: (stage) => {
            accumulator.accept({kind: 'stage', stage});
            timing.textContent = `${stage}…`;
          },
          onOutput: (port, value, mimetype) => accumulator.accept({
            kind: 'output', port, value, mimetype, textual: mimetype.startsWith('text/'),
          }),
          onLog: (text, record) => accumulator.accept({kind: 'log', log: runLog(text, record)}),
          onDispatchStatus: (at) => accumulator.accept({kind: 'dispatch-status', at}),
          onStatus: (at) => accumulator.accept({kind: 'status', at}),
        });
      }
    } catch (error) {
      finalStatus = {
        code: this.activeCancelled ? StatusCode.CANCELLED : StatusCode.UNKNOWN,
        message: this.activeCancelled ? 'The action run was cancelled.' : errorMessage(error),
      };
    } finally {
      const ended = Date.now();
      accumulator.accept({
        kind: 'ended',
        state: this.activeCancelled ? 'cancelled' : isOk(finalStatus) ? 'succeeded' : 'failed',
        at: ended,
        status: finalStatus,
      });
      timing.textContent = `${ended - accumulator.getSnapshot().startedAt} ms`;
      button.disabled = false;
      cancel.remove();
      this.activeRunId = null;
      this.activeCancelled = false;
      this.renderRunInspector();
    }
  }

  private recordRun(run: RunSnapshot): Status {
    this.runs = retainRuns(this.runs, run);
    if (this.selectedRunId === run.id) this.renderRunInspector();
    return okStatus();
  }

  private renderRunInspector(): void {
    this.detail.querySelector('.action-run-inspector')?.remove();
    const run = this.runs.find((candidate) => candidate.id === this.selectedRunId);
    if (!run) return;
    const panel = document.createElement('section');
    panel.className = 'action-run-inspector';
    const header = document.createElement('header');
    header.className = 'action-run-header';
    const title = document.createElement('h3');
    title.textContent = 'Run results';
    const controls = document.createElement('div');
    controls.className = 'action-run-header-controls';
    const select = document.createElement('select');
    select.className = 'runner-run-selector';
    select.setAttribute('aria-label', 'Action run');
    for (const [index, candidate] of this.runs.entries()) {
      const option = document.createElement('option');
      option.value = candidate.id;
      const time = new Date(candidate.startedAt).toLocaleTimeString([], {
        hour: '2-digit', minute: '2-digit', second: '2-digit',
      });
      option.textContent = index === 0 ? `Latest · ${time}` : `Run ${this.runs.length - index} · ${time}`;
      option.selected = candidate.id === run.id;
      select.append(option);
    }
    select.addEventListener('change', () => {
      this.selectedRunId = select.value;
      this.renderRunInspector();
    });
    const clear = document.createElement('button');
    clear.type = 'button';
    clear.className = 'runner-clear-runs';
    clear.setAttribute('aria-label', 'Clear run history');
    clear.title = 'Clear run history';
    clear.append(trashIcon());
    clear.disabled = this.activeRunId !== null;
    clear.addEventListener('click', () => {
      this.runs = [];
      this.selectedRunId = null;
      this.renderRunInspector();
    });
    controls.append(select, clear);
    const status = document.createElement('div');
    status.className = `action-run-status ${run.state}`;
    const mark = document.createElement('span');
    mark.textContent = run.state === 'running' ? '●' : run.state === 'succeeded' ? '✓' : '×';
    const elapsed = document.createElement('span');
    elapsed.textContent = run.state === 'running'
      ? run.stage
      : `${run.state} · ${(run.endedAt ?? Date.now()) - run.startedAt} ms`;
    status.append(mark, elapsed);
    header.append(title, controls, status);
    const outputs = document.createElement('div');
    outputs.className = 'action-run-outputs';
    new OutputPresenter(outputs).show(run.outputs);
    if (run.outputs.length === 0) outputs.append(emptyLine(run.state === 'running' ? 'Waiting for output…' : 'No output values.'));
    const logs = document.createElement('div');
    logs.className = 'action-run-logs';
    for (const log of run.logs) {
      const line = document.createElement('div');
      line.className = `action-run-log ${log.level}`;
      const level = document.createElement('span');
      level.textContent = log.level;
      const text = document.createElement('pre');
      text.textContent = log.text;
      line.append(level, text);
      logs.append(line);
    }
    if (run.logs.length === 0) logs.append(emptyLine(run.state === 'running' ? 'Waiting for logs…' : 'Nothing logged.'));
    panel.append(header);
    if (run.status && !isOk(run.status)) {
      const failure = document.createElement('p');
      failure.className = 'action-run-failure';
      failure.textContent = run.status.message;
      panel.append(failure);
    }
    panel.append(outputs, logs);
    this.detail.append(panel);
  }

  private showInlineFailure(message: string): void {
    const failure = document.createElement('p');
    failure.className = 'action-run-failure';
    failure.textContent = message;
    this.detail.querySelector('.action-run-failure')?.remove();
    this.detail.append(failure);
  }

  private showFailure(message: string): void {
    this.detail.replaceChildren(errorLine(message));
  }
}

function descriptorToSchema(descriptor: ActionDescriptor): SchemaEntry {
  return {
    name: descriptor.name,
    description: descriptor.description,
    runnable: true,
    inputs: descriptor.inputs.map(descriptorPortToSchema),
    outputs: descriptor.outputs.map(descriptorPortToSchema),
    output_to_json_field: descriptor.output_to_json_field,
  };
}

function descriptorPortToSchema(port: PortDescriptor) {
  return {
    name: port.name, type: port.type, required: port.required, unary: port.unary,
    description: port.description, json_schema: port.schema,
  };
}

function portToDescriptor(port: NonNullable<SchemaEntry['inputs']>[number]): PortDescriptor {
  return {
    name: port.name,
    type: port.type,
    required: port.required === true,
    unary: port.unary === true,
    description: port.description,
    schema: port.json_schema as Record<string, unknown> | undefined,
  };
}

function outputsSection(schema: SchemaEntry): HTMLElement {
  const section = document.createElement('section');
  section.className = 'port outputs action-contract';
  const title = document.createElement('h3');
  title.textContent = 'Outputs';
  section.append(title);
  for (const port of schema.outputs ?? []) {
    const row = document.createElement('div');
    row.className = 'output-row';
    const name = document.createElement('span');
    name.className = 'output-name';
    name.textContent = port.name;
    const cardinality = document.createElement('span');
    cardinality.className = 'port-flag';
    cardinality.textContent = port.unary === true ? 'single value' : 'stream';
    const type = document.createElement('span');
    type.className = 'port-type';
    type.textContent = port.type;
    row.append(name, cardinality, type);
    section.append(row);
    if (port.description) section.append(emptyLine(port.description));
    const fields = describeSchema(port.json_schema as Record<string, unknown> | undefined);
    if (fields) section.append(fields);
  }
  if (!schema.outputs?.length) section.append(emptyLine('This action declares no outputs.'));
  return section;
}

function runLog(text: string, record: LogRecord) {
  return {
    level: record.level, text, channel: record.channel,
    at: record.timestamp?.getTime(), callId: record.actionId,
  };
}

function emptyLine(text: string): HTMLParagraphElement {
  const line = document.createElement('p');
  line.className = 'field-hint';
  line.textContent = text;
  return line;
}

function errorLine(message: string): HTMLElement {
  const line = document.createElement('p');
  line.className = 'error';
  line.textContent = message;
  return line;
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/** Mount the action workspace into `root`. */
export function mountActions(root: HTMLElement): MountedView {
  const explorer = new ActionExplorer(root);
  void explorer.load();
  return {dispose: () => explorer.dispose()};
}
