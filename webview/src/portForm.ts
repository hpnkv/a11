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
 * Build input widgets for an action's ports from their JSON Schema.
 *
 * Kotlin ships each port's schema in its descriptor (`ActionPortSchema.jsonSchema`
 * — the Kotlin stand-in for the Python port's `typeinfo`), which is exactly the
 * contract the model is given. Rendering a form from it means the action explorer
 * asks for the same fields the model would fill, with the schema's descriptions
 * as inline help. A port with no schema falls back to a highlighted JSON editor.
 *
 * A unary port takes one value; a streaming port shows a list with an "add"
 * button, one entry per value the caller wants to put on the node.
 */

import {
  asSchema,
  describePortInput,
  isFormable,
  requiredNames,
  schemaShape,
  schemaText,
  type JsonSchema,
  type SchemaShape,
  typeLabel,
} from '@curiositystack/a11/presentation';
import {isOk} from '@curiositystack/a11';

import { createJsonEditor } from './jsonEditor.js';
import type { PortDescriptor } from './bridge.js';

/** A widget holding one value; [read] throws when the value is unusable. */
interface ValueEditor {
  readonly element: HTMLElement;
  /** The value, or `undefined` when left empty. */
  read(): unknown;
}


function label(text: string, required: boolean): HTMLLabelElement {
  const element = document.createElement('label');
  element.className = 'field-label';
  element.textContent = text;
  if (required) {
    const mark = document.createElement('span');
    mark.className = 'field-required';
    mark.textContent = '*';
    mark.title = 'required';
    element.append(mark);
  }
  return element;
}

function hint(text: string): HTMLElement | null {
  if (!text) return null;
  const element = document.createElement('p');
  element.className = 'field-hint';
  element.textContent = text;
  return element;
}

// --- leaf widgets ----------------------------------------------------------

function enumEditor(schema: JsonSchema, path: string, required: boolean): ValueEditor {
  const values = schema['enum'] as unknown[];
  const select = document.createElement('select');
  select.className = 'field-input';
  if (!required) select.append(new Option('—', ''));
  for (const value of values) select.append(new Option(String(value), String(value)));
  return {
    element: select,
    read: () => {
      if (select.value === '') return required ? missing(path) : undefined;
      return values.find((value) => String(value) === select.value);
    },
  };
}

function textEditor(schema: JsonSchema, path: string, required: boolean): ValueEditor {
  const input = document.createElement('input');
  input.type = 'text';
  input.className = 'field-input';
  const examples = schema['examples'];
  if (Array.isArray(examples) && examples.length > 0) input.placeholder = String(examples[0]);
  return {
    element: input,
    read: () => {
      const value = input.value.trim();
      if (value === '') return required ? missing(path) : undefined;
      return value;
    },
  };
}

function proseEditor(path: string, required: boolean): ValueEditor {
  const textarea = document.createElement('textarea');
  textarea.className = 'field-input field-prose';
  textarea.rows = 5;
  return {
    element: textarea,
    read: () => {
      const value = textarea.value.trim();
      if (!value) return required ? missing(path) : undefined;
      return textarea.value;
    },
  };
}

function imageEditor(path: string, required: boolean): ValueEditor {
  const element = document.createElement('div');
  element.className = 'field-image';
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = 'image/*';
  input.className = 'field-input';
  const preview = document.createElement('img');
  preview.alt = '';
  preview.hidden = true;
  let bytes: Uint8Array | undefined;
  let loading = false;
  input.addEventListener('change', () => {
    const file = input.files?.[0];
    bytes = undefined;
    preview.hidden = true;
    if (!file) return;
    loading = true;
    void file.arrayBuffer().then((buffer) => {
      bytes = new Uint8Array(buffer);
      preview.src = URL.createObjectURL(file);
      preview.hidden = false;
    }).finally(() => { loading = false; });
  });
  element.append(input, preview);
  return {
    element,
    read: () => {
      if (loading) throw new Error(`${path}: wait for the image to finish loading`);
      if (!bytes && required) return missing(path);
      return bytes;
    },
  };
}

function numberEditor(schema: JsonSchema, path: string, required: boolean): ValueEditor {
  const input = document.createElement('input');
  input.type = 'number';
  input.className = 'field-input';
  if (schema['type'] === 'integer') input.step = '1';
  if (typeof schema['minimum'] === 'number') input.min = String(schema['minimum']);
  if (typeof schema['maximum'] === 'number') input.max = String(schema['maximum']);
  return {
    element: input,
    read: () => {
      const text = input.value.trim();
      if (text === '') return required ? missing(path) : undefined;
      const value = Number(text);
      if (!Number.isFinite(value)) throw new Error(`${path}: '${text}' is not a number`);
      if (schema['type'] === 'integer' && !Number.isInteger(value)) {
        throw new Error(`${path}: ${text} must be a whole number`);
      }
      return value;
    },
  };
}

function booleanEditor(): ValueEditor {
  const input = document.createElement('input');
  input.type = 'checkbox';
  input.className = 'field-checkbox';
  return { element: input, read: () => input.checked };
}

/** A nullable value starts unset and exposes its concrete editor on demand. */
function nullableEditor(schema: JsonSchema, path: string, required: boolean): ValueEditor {
  const element = document.createElement('div');
  element.className = 'field-nullable';
  const editor = valueEditor(schema, path, true);
  const controls = document.createElement('div');
  controls.className = 'field-nullable-controls';
  const set = document.createElement('button');
  set.type = 'button';
  set.className = 'ghost-button';
  set.textContent = 'Set value';
  const clear = document.createElement('button');
  clear.type = 'button';
  clear.className = 'ghost-button remove';
  clear.textContent = 'Clear';
  clear.title = 'Use null';
  controls.append(set, clear);
  element.append(controls, editor.element);

  let present = false;
  let cleared = false;
  const show = (): void => {
    editor.element.hidden = !present;
    set.hidden = present;
    clear.hidden = !present;
  };
  set.onclick = () => {
    present = true;
    cleared = false;
    show();
    editor.element.querySelector<HTMLElement>('input, select, textarea, button')?.focus();
  };
  clear.onclick = () => {
    present = false;
    cleared = true;
    show();
  };
  show();

  return {
    element,
    read: () => {
      if (!present) return cleared || required ? null : undefined;
      const value = editor.read();
      return value === undefined ? missing(path) : value;
    },
  };
}

function missing(path: string): never {
  throw new Error(`${path} is required`);
}

// --- composite widgets -----------------------------------------------------

/** An object: one labeled row per property, plus its own required checks. */
function objectEditor(schema: JsonSchema, path: string): ValueEditor {
  const element = document.createElement('div');
  element.className = 'field-object';
  const properties = asSchema(schema['properties']) ?? {};
  const required = requiredNames(schema);
  const fields: Array<{ name: string; editor: ValueEditor }> = [];

  for (const [name, rawField] of Object.entries(properties)) {
    const field = asSchema(rawField);
    const isRequired = required.includes(name);
    const childPath = path ? `${path}.${name}` : name;
    const editor = valueEditor(field, childPath, isRequired);

    const row = document.createElement('div');
    row.className = 'field';
    row.append(label(name, isRequired));
    const description = hint(field ? schemaText(field, 'description') : '');
    if (description) row.append(description);
    row.append(editor.element);
    element.append(row);
    fields.push({ name, editor });
  }

  return {
    element,
    read: () => {
      const value: Record<string, unknown> = {};
      for (const { name, editor } of fields) {
        const read = editor.read();
        if (read !== undefined) value[name] = read;
      }
      return Object.keys(value).length === 0 ? undefined : value;
    },
  };
}

/** A growable list of values, used for arrays and for streaming ports. */
function listEditor(
  itemSchema: JsonSchema | undefined,
  path: string,
  options: { addLabel: string; startEmpty: boolean },
): ValueEditor {
  const element = document.createElement('div');
  element.className = 'field-list';
  const items = document.createElement('div');
  items.className = 'field-list-items';
  const add = document.createElement('button');
  add.type = 'button';
  add.className = 'ghost-button';
  add.textContent = options.addLabel;
  element.append(items, add);

  const editors: ValueEditor[] = [];

  const renumber = (): void => {
    [...items.children].forEach((row, index) => {
      const badge = row.querySelector('.field-list-index');
      if (badge) badge.textContent = `${index + 1}`;
    });
    items.classList.toggle('empty', editors.length === 0);
  };

  const addItem = (): void => {
    const editor = valueEditor(itemSchema, `${path}[${editors.length}]`, true);
    const row = document.createElement('div');
    row.className = 'field-list-item';
    // A whole object per entry needs a visible boundary; a bare value does not.
    if (itemSchema?.['type'] === 'object') row.classList.add('object-item');
    const badge = document.createElement('span');
    badge.className = 'field-list-index';
    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'ghost-button remove';
    remove.textContent = '✕';
    remove.title = 'Remove this item';
    remove.onclick = () => {
      const at = editors.indexOf(editor);
      if (at >= 0) editors.splice(at, 1);
      row.remove();
      renumber();
    };
    const body = document.createElement('div');
    body.className = 'field-list-body';
    body.append(editor.element);
    row.append(badge, body, remove);
    items.append(row);
    editors.push(editor);
    renumber();
  };

  add.onclick = addItem;
  if (!options.startEmpty) addItem();
  renumber();

  return {
    element,
    read: () => {
      const values = editors.map((editor) => editor.read()).filter((value) => value !== undefined);
      return values.length === 0 ? undefined : values;
    },
  };
}

/** The widget for one value of `schema`; free-form JSON when it has no shape. */
function valueEditor(schema: JsonSchema | undefined, path: string, required: boolean): ValueEditor {
  if (!isFormable(schema)) {
    const editor = createJsonEditor({ placeholder: '{ }' });
    return {
      element: editor.element,
      read: () => {
        try {
          const value = editor.read();
          if (value === undefined && required) missing(path);
          return value;
        } catch (error) {
          throw new Error(`${path}: ${error instanceof Error ? error.message : String(error)}`);
        }
      },
    };
  }
  const resolved = schemaShape(schema) as SchemaShape;
  const shape = resolved.schema;
  if (resolved.nullable) return nullableEditor(shape, path, required);
  if (Array.isArray(shape['enum'])) return enumEditor(shape, path, required);
  switch (shape['type']) {
    case 'object':
      return objectEditor(shape, path);
    case 'array':
      return listEditor(asSchema(shape['items']), path, { addLabel: '+ Add', startEmpty: !required });
    case 'boolean':
      return booleanEditor();
    case 'number':
    case 'integer':
      return numberEditor(shape, path, required);
    default:
      return textEditor(shape, path, required);
  }
}

/**
 * A read-only rendering of a schema's fields: what an action promises to return.
 * Objects list one row per property (marking the guaranteed ones), anything else
 * renders as its type alone. Returns null when the schema has no shape to show.
 */
export function describeSchema(schema: JsonSchema | undefined): HTMLElement | null {
  if (!schema) return null;
  const properties = asSchema(schema['properties']);
  const element = document.createElement('div');
  element.className = 'schema-fields';
  if (!properties) {
    const type = typeLabel(schema);
    if (type === 'any') return null;
    const row = document.createElement('div');
    row.className = 'schema-field';
    row.append(fieldType(type));
    element.append(row);
    return element;
  }
  const guaranteed = requiredNames(schema);
  for (const [name, rawField] of Object.entries(properties)) {
    const field = asSchema(rawField) ?? {};
    const row = document.createElement('div');
    row.className = 'schema-field';
    const label = document.createElement('span');
    label.className = 'schema-field-name';
    label.textContent = name;
    row.append(label, fieldType(typeLabel(field)));
    if (guaranteed.includes(name)) {
      const always = document.createElement('span');
      always.className = 'port-flag';
      always.textContent = 'always';
      always.title = 'Always present in the result';
      row.append(always);
    }
    element.append(row);
    const description = hint(schemaText(field, 'description'));
    if (description) element.append(description);
  }
  return element;
}

function fieldType(text: string): HTMLElement {
  const element = document.createElement('span');
  element.className = 'schema-field-type';
  element.textContent = text;
  return element;
}

export interface PortInput {
  readonly element: HTMLElement;
  /** The port's value (or list of values), or `undefined` when left empty. */
  read(): unknown;
}

/** Build the input section for one port: header, description, and widget. */
export function createPortInput(port: PortDescriptor): PortInput {
  const element = document.createElement('section');
  element.className = 'port';

  const head = document.createElement('div');
  head.className = 'port-head';
  const name = document.createElement('span');
  name.className = 'port-name';
  name.textContent = port.name;
  head.append(name);
  for (const flag of [port.required ? 'required' : 'optional', port.unary ? 'single value' : 'multiple values']) {
    const tag = document.createElement('span');
    tag.className = `port-flag${flag === 'required' ? ' required' : ''}`;
    tag.textContent = flag;
    head.append(tag);
  }
  const type = document.createElement('span');
  type.className = 'port-type';
  type.textContent = port.type;
  head.append(type);
  element.append(head);

  const description = hint(port.description ?? '');
  if (description) {
    description.classList.add('port-hint');
    element.append(description);
  }

  const schema = asSchema(port.schema);
  const widget = describePortInput({
    name: port.name,
    type: port.type,
    required: port.required,
    unary: port.unary,
    description: port.description,
    json_schema: port.schema,
  }, undefined);
  if (!isFormable(schema)) {
    const note = hint('No schema for this port — enter JSON directly.');
    if (note) element.append(note);
  }

  // A unary port holds one value; a streaming one, a list the caller grows.
  const unaryEditor = isOk(widget) && widget.kind === 'image'
    ? imageEditor(port.name, port.required)
    : isOk(widget) && widget.kind === 'prose'
      ? proseEditor(port.name, port.required)
      : valueEditor(schema, port.name, port.required);
  const editor = port.unary
    ? unaryEditor
    : listEditor(schema, port.name, { addLabel: '+ Add value', startEmpty: !port.required });
  element.append(editor.element);
  element.addEventListener('input', () => {
    element.classList.remove('invalid');
    editor.element.removeAttribute('aria-invalid');
  });

  return {
    element,
    read: () => {
      const value = editor.read();
      if (value === undefined && port.required) missing(`port '${port.name}'`);
      return value;
    },
  };
}
