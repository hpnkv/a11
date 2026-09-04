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

import { createJsonEditor } from './jsonEditor.js';
import type { PortDescriptor } from './bridge.js';

type JsonSchema = Record<string, unknown>;

/** A widget holding one value; [read] throws when the value is unusable. */
interface ValueEditor {
  readonly element: HTMLElement;
  /** The value, or `undefined` when left empty. */
  read(): unknown;
}

const str = (schema: JsonSchema, key: string): string =>
  typeof schema[key] === 'string' ? (schema[key] as string) : '';

const asSchema = (value: unknown): JsonSchema | undefined =>
  typeof value === 'object' && value !== null && !Array.isArray(value)
    ? (value as JsonSchema)
    : undefined;

const requiredNames = (schema: JsonSchema): string[] =>
  Array.isArray(schema['required']) ? (schema['required'] as unknown[]).filter((n): n is string => typeof n === 'string') : [];

interface SchemaShape {
  readonly schema: JsonSchema;
  readonly nullable: boolean;
}

/** Unwrap `T | null` while retaining constraints declared beside the union. */
function schemaShape(schema: JsonSchema | undefined): SchemaShape | undefined {
  if (!schema) return undefined;
  for (const keyword of ['anyOf', 'oneOf']) {
    const alternatives = schema[keyword];
    if (!Array.isArray(alternatives)) continue;
    const shapes = alternatives.map(asSchema);
    if (shapes.some((shape) => shape === undefined)) continue;
    const concrete = (shapes as JsonSchema[]).filter((shape) => shape['type'] !== 'null');
    const nulls = (shapes as JsonSchema[]).filter((shape) => shape['type'] === 'null');
    if (concrete.length !== 1 || nulls.length !== 1) continue;
    const unwrapped = { ...schema, ...concrete[0] };
    delete unwrapped['anyOf'];
    delete unwrapped['oneOf'];
    return { schema: unwrapped, nullable: true };
  }
  return { schema, nullable: schema['type'] === 'null' };
}

/** Whether the schema is specific enough to render as a typed widget. */
function isFormable(schema: JsonSchema | undefined): boolean {
  const shape = schemaShape(schema)?.schema;
  if (!shape) return false;
  if (Array.isArray(shape['enum'])) return true;
  const type = shape['type'];
  if (type === 'object') return asSchema(shape['properties']) !== undefined;
  if (type === 'array') return asSchema(shape['items']) !== undefined;
  return type === 'string' || type === 'number' || type === 'integer' || type === 'boolean';
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
    const description = hint(field ? str(field, 'description') : '');
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
    const description = hint(str(field, 'description'));
    if (description) element.append(description);
  }
  return element;
}

/** A field's JSON type, including the item type of an array. */
function typeLabel(schema: JsonSchema): string {
  const resolved = schemaShape(schema);
  const shape = resolved?.schema ?? schema;
  const type = str(shape, 'type') || 'any';
  const suffix = resolved?.nullable ? ' | null' : '';
  if (type !== 'array') return `${type}${suffix}`;
  const items = asSchema(shape['items']);
  return `array<${items ? typeLabel(items) : 'any'}>${suffix}`;
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
    tag.className = 'port-flag';
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
  if (!isFormable(schema)) {
    const note = hint('No schema for this port — enter JSON directly.');
    if (note) element.append(note);
  }

  // A unary port holds one value; a streaming one, a list the caller grows.
  const editor = port.unary
    ? valueEditor(schema, port.name, port.required)
    : listEditor(schema, port.name, { addLabel: '+ Add value', startEmpty: !port.required });
  element.append(editor.element);

  return {
    element,
    read: () => {
      const value = editor.read();
      if (value === undefined && port.required) missing(`port '${port.name}'`);
      return value;
    },
  };
}
