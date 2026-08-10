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

/** One JSON token class the highlighter recognizes. */
const TOKENS = new RegExp(
  [
    '("(?:\\\\.|[^"\\\\])*")\\s*(?=:)', // property name
    '("(?:\\\\.|[^"\\\\])*")', // string
    '\\b(true|false|null)\\b', // keyword
    '(-?\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?)', // number
    '([{}\\[\\],:])', // punctuation
  ].join('|'),
  'g',
);

const CLASSES = ['json-key', 'json-string', 'json-keyword', 'json-number', 'json-punct'];

function escapeHtml(text: string): string {
  return text.replace(/[&<>]/g, (c) => (c === '&' ? '&amp;' : c === '<' ? '&lt;' : '&gt;'));
}

/** Wrap every recognized JSON token of `text` in a class-carrying span. */
function highlight(text: string): string {
  let html = '';
  let last = 0;
  for (const match of text.matchAll(TOKENS)) {
    const at = match.index ?? 0;
    html += escapeHtml(text.slice(last, at));
    // Group i+1 is set for exactly one alternative; its index picks the class.
    const cls = CLASSES[CLASSES.findIndex((_, i) => match[i + 1] !== undefined)];
    const token = escapeHtml(match[0]);
    html += cls ? `<span class="${cls}">${token}</span>` : token;
    last = at + match[0].length;
  }
  // The trailing newline keeps the box tall enough for a caret on the last line.
  return `${html + escapeHtml(text.slice(last))}\n`;
}

export interface JsonEditor {
  readonly element: HTMLElement;
  /** The parsed value, or `undefined` when blank. Throws on malformed JSON. */
  read(): unknown;
  setText(text: string): void;
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
    const text = input.value.trim();
    let message = '';
    if (text !== '') {
      try {
        JSON.parse(text);
      } catch (error) {
        message = error instanceof Error ? error.message : String(error);
      }
    }
    hint.textContent = message;
    hint.classList.toggle('visible', message !== '');
    element.classList.toggle('invalid', message !== '');
  };

  input.addEventListener('input', sync);
  // Keep Tab as indentation: leaving the field mid-object is rarely the intent.
  input.addEventListener('keydown', (event) => {
    if (event.key !== 'Tab') return;
    event.preventDefault();
    const { selectionStart: start, selectionEnd: end, value } = input;
    input.value = `${value.slice(0, start)}  ${value.slice(end)}`;
    input.selectionStart = input.selectionEnd = start + 2;
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
  };
}
