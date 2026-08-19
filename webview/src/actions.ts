/**
 * The action explorer: a developer tool to list every action the IDE exposes and
 * run it, independent of the LLM/backend. It drives the same `bridge.runAction`
 * path the chat tool round-trip uses, so it exercises the real IDE handlers.
 *
 * Inputs are collected per port, from a form built out of the port's JSON Schema
 * (see `portForm.ts`) — the same contract the model is handed — so the explorer
 * asks for typed fields with their descriptions rather than one opaque blob of
 * JSON. Ports without a schema keep a highlighted free-form JSON editor.
 */

import { listActions, runAction, type ActionDescriptor } from './bridge.js';
import type { MountedView } from './mount.js';
import { createPortInput, describeSchema, type PortInput } from './portForm.js';

class ActionExplorer {
  private readonly list: HTMLDivElement;
  private readonly detail: HTMLDivElement;
  private descriptors: ActionDescriptor[] = [];
  private selected: string | null = null;

  constructor(root: HTMLElement) {
    root.classList.add('actions-view');
    this.list = document.createElement('div');
    this.list.className = 'action-list';
    this.detail = document.createElement('div');
    this.detail.className = 'action-detail';
    root.append(this.list, this.detail);
  }

  async load(): Promise<void> {
    try {
      this.descriptors = await listActions();
    } catch (error) {
      this.detail.replaceChildren(errorLine(error));
      return;
    }
    this.renderList();
    if (this.descriptors.length > 0) this.select(this.descriptors[0]!.name);
  }

  private renderList(): void {
    this.list.replaceChildren(
      ...this.descriptors.map((descriptor) => {
        const item = document.createElement('button');
        item.type = 'button';
        item.className = 'action-item';
        item.classList.toggle('selected', descriptor.name === this.selected);
        const name = document.createElement('span');
        name.className = 'action-item-name';
        name.textContent = descriptor.name;
        const desc = document.createElement('span');
        desc.className = 'action-item-desc';
        desc.textContent = descriptor.description;
        item.append(name, desc);
        item.onclick = () => this.select(descriptor.name);
        return item;
      }),
    );
  }

  private select(name: string): void {
    this.selected = name;
    this.renderList();
    const descriptor = this.descriptors.find((d) => d.name === name);
    if (descriptor) this.renderDetail(descriptor);
  }

  private renderDetail(descriptor: ActionDescriptor): void {
    this.detail.replaceChildren();

    const title = document.createElement('h2');
    title.textContent = descriptor.name;
    const description = document.createElement('p');
    description.className = 'action-description';
    description.textContent = descriptor.description;
    this.detail.append(title, description);

    const ports = new Map<string, PortInput>();
    if (descriptor.inputs.length === 0) {
      const empty = document.createElement('p');
      empty.className = 'field-hint';
      empty.textContent = 'This action takes no inputs.';
      this.detail.append(empty);
    } else {
      for (const port of descriptor.inputs) {
        const input = createPortInput(port);
        ports.set(port.name, input);
        this.detail.append(input.element);
      }
    }

    if (descriptor.outputs.length > 0) this.detail.append(outputsSection(descriptor));

    const controls = document.createElement('div');
    controls.className = 'action-controls';
    const run = document.createElement('button');
    run.type = 'button';
    run.className = 'run-button';
    run.textContent = 'Run';
    const timing = document.createElement('span');
    timing.className = 'timing';
    controls.append(run, timing);
    this.detail.append(controls);

    const output = document.createElement('pre');
    output.className = 'json-output';
    this.detail.append(output);

    const fail = (message: string): void => {
      output.className = 'json-output error';
      output.textContent = message;
    };

    run.onclick = async () => {
      // Read every port first: a bad value must not half-run the action.
      const inputs: Record<string, unknown> = {};
      try {
        for (const [name, port] of ports) {
          const value = port.read();
          if (value !== undefined) inputs[name] = value;
        }
      } catch (error) {
        fail(error instanceof Error ? error.message : String(error));
        return;
      }

      run.disabled = true;
      timing.textContent = 'running...';
      const started = performance.now();
      try {
        const result = await runAction(descriptor.name, inputs);
        output.className = 'json-output';
        output.textContent = JSON.stringify(result, null, 2);
      } catch (error) {
        fail(error instanceof Error ? error.message : String(error));
      } finally {
        timing.textContent = `${Math.round(performance.now() - started)} ms`;
        run.disabled = false;
      }
    };
  }
}

/** A read-only summary of what the action produces, one row per output port. */
function outputsSection(descriptor: ActionDescriptor): HTMLElement {
  const section = document.createElement('section');
  section.className = 'port outputs';
  const head = document.createElement('div');
  head.className = 'port-head';
  const title = document.createElement('span');
  title.className = 'port-name';
  title.textContent = 'outputs';
  head.append(title);
  section.append(head);

  for (const port of descriptor.outputs) {
    const row = document.createElement('div');
    row.className = 'output-row';
    const name = document.createElement('span');
    name.className = 'output-name';
    name.textContent = port.name;
    // Cardinality matters on an output too: one value, or a stream of them.
    const cardinality = document.createElement('span');
    cardinality.className = 'port-flag';
    cardinality.textContent = port.unary ? 'single value' : 'multiple values';
    const type = document.createElement('span');
    type.className = 'port-type';
    type.textContent = port.type;
    if (port.unary) {
      row.append(name, cardinality, type);
    }

    section.append(row);
    const description = port.description ?? '';
    if (description) {
      const text = document.createElement('p');
      text.className = 'field-hint';
      text.textContent = description;
      section.append(text);
    }
    // The port's own JSON Schema, when it declares one: the result's shape.
    const fields = describeSchema(port.schema);
    if (fields) section.append(fields);
  }
  return section;
}

function errorLine(error: unknown): HTMLElement {
  const line = document.createElement('p');
  line.className = 'error';
  line.textContent = error instanceof Error ? error.message : String(error);
  return line;
}

/** Mount the action explorer into `root`. */
export function mountActions(root: HTMLElement): MountedView {
  void new ActionExplorer(root).load();
  return {};
}
