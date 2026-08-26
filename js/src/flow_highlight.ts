/**
 * Flow's token vocabulary, for colouring a flow in a browser.
 *
 * A11 already generates editor definitions from the grammar -- `a11 flow syntax
 * --target sublime|pygments|vscode` writes the files under `editors/` -- and each
 * of those targets is a *file* a host editor loads. A web page has no such host:
 * it has a `<textarea>` and whatever it can render behind it, so it needs the
 * vocabulary as data and a scanner it can call.
 *
 * That makes this the one hand-written member of the family, and the risk is
 * drift: a word added to the language would colour everywhere but here.
 * `js/test/flow_highlight.test.mjs` pins every list below against
 * `editors/pygments/a11flow_lexer.py`, which *is* generated, so the drift fails a
 * test rather than being noticed by eye a release later.
 *
 * It is a scanner and not a parser. Highlighting wants "this word is a stage",
 * not a tree, and being wrong about a word in a comment is worse than being
 * unaware that `log` is a statement here and a stage there -- so words are
 * matched by set, and strings and comments are consumed first so that no word
 * inside one is ever looked up.
 */

/** What a run of characters is, for colouring. */
export type FlowTokenKind =
  | 'comment'
  | 'string'
  | 'number'
  | 'keyword'
  | 'type'
  | 'stage'
  | 'builtin'
  | 'level'
  | 'constant'
  | 'operator'
  | 'punctuation'
  | 'plain';

/** One run of characters, with where it is. */
export interface FlowToken {
  kind: FlowTokenKind;
  start: number;
  end: number;
  text: string;
}

/** Words that declare, bind, or open a statement. */
export const FLOW_KEYWORDS: readonly string[] = [
  'flow', 'describe', 'struct', 'header', 'nodes', 'in', 'out', 'stream',
  'required', 'unique', 'matching', 'default', 'as', 'run', 'call', 'try',
  'node', 'wait', 'drain', 'let', 'advance', 'skip', 'abort', 'cancel', 'fail',
  'log', 'logf', 'if', 'for', 'repeat', 'until', 'while', 'else', 'parallel',
  'unordered', 'of', 'by', 'desc', 'into', 'tee', 'via', 'timeout', 'after',
  'with', 'id', 'forward', 'headers', 'interleave', 'status', 'zip',
];

/** The built-in port type names. */
export const FLOW_TYPES: readonly string[] = [
  'string', 'text', 'number', 'integer', 'int', 'bool', 'boolean', 'duration',
  'time', 'object', 'json', 'list', 'array', 'bytes', 'any',
];

/** Every pipeline stage. */
export const FLOW_STAGES: readonly string[] = [
  'first', 'last', 'drop', 'truncate', 'batch', 'window', 'flatten', 'group',
  'sort', 'where', 'map', 'scan', 'match', 'distinct', 'then', 'log', 'logf',
  'mime', 'strformat', 'chunk', 'collect', 'count', 'sum', 'min', 'max', 'avg',
  'fold', 'join', 'text', 'json', 'packb', 'timeout', 'pace',
];

/** The language's fixed function set. No user code, ever: a flow stays data. */
export const FLOW_BUILTINS: readonly string[] = [
  'len', 'lower', 'upper', 'trim', 'text', 'number', 'bool', 'keys', 'values',
  'get', 'join', 'split', 'merge', 'contains', 'starts-with', 'ends-with',
  'replace', 'match', 'slice', 'default', 'to_chunk', 'from_chunk', 'strformat',
  'b64encode', 'b64decode', 'b64urlencode', 'b64urldecode', 'now', 'duration',
  'time', 'seconds',
];

/** The severities `log` and `logf` name. */
export const FLOW_LOG_LEVELS: readonly string[] = [
  'debug', 'info', 'warning', 'error', 'critical',
];

/** Literals that are words, and the operators that are. */
export const FLOW_CONSTANTS: readonly string[] = ['true', 'false', 'null', '_'];
export const FLOW_OPERATOR_WORDS: readonly string[] = ['and', 'or', 'not', 'in'];

const KEYWORDS = new Set(FLOW_KEYWORDS);
const TYPES = new Set(FLOW_TYPES);
const STAGES = new Set(FLOW_STAGES);
const BUILTINS = new Set(FLOW_BUILTINS);
const LEVELS = new Set(FLOW_LOG_LEVELS);
const CONSTANTS = new Set(FLOW_CONSTANTS);
const OPERATOR_WORDS = new Set(FLOW_OPERATOR_WORDS);

/** A word, including the hyphenated builtins and dotted type names. */
const WORD = /[A-Za-z_][A-Za-z0-9_.-]*/y;
/** A number, with the duration and byte-size suffixes a flow may write. */
const NUMBER = /\d[\d_]*(?:\.\d+)?(?:ms|s|m|h|d|ns|us|kb|mb|gb)?/iy;
const PUNCTUATION = new Set('{}()[],:');

function wordKind(word: string, previous: FlowToken | null): FlowTokenKind {
  // After a `|`, a word is the stage it names -- which is the one piece of
  // context worth keeping, because `log`, `text` and `json` are a statement, a
  // type and a stage depending on where they stand, and the pipe settles it.
  const piped = previous?.kind === 'operator' && previous.text === '|';
  if (piped && STAGES.has(word)) return 'stage';
  // A level only follows `log`/`logf`, and `error` is otherwise an ordinary word.
  if (
    previous !== null &&
    (previous.text === 'log' || previous.text === 'logf') &&
    LEVELS.has(word)
  ) {
    return 'level';
  }
  if (CONSTANTS.has(word)) return 'constant';
  if (OPERATOR_WORDS.has(word)) return 'operator';
  if (KEYWORDS.has(word)) return 'keyword';
  if (TYPES.has(word)) return 'type';
  if (BUILTINS.has(word)) return 'builtin';
  if (STAGES.has(word)) return 'stage';
  return 'plain';
}

/**
 * Split `source` into coloured runs, covering every character exactly once.
 *
 * Total by construction -- anything unrecognised comes back as `plain` -- so a
 * caller can concatenate the tokens' text and get the source back. A highlighter
 * that drops characters silently corrupts the code it is drawn behind.
 */
export function tokenizeFlow(source: string): FlowToken[] {
  const tokens: FlowToken[] = [];
  let at = 0;
  let previous: FlowToken | null = null;

  const push = (kind: FlowTokenKind, start: number, end: number): void => {
    const token = { kind, start, end, text: source.slice(start, end) };
    tokens.push(token);
    // Whitespace is `plain` and must not become "the previous token" for the
    // stage and level rules, or `| where` stops being a stage the moment
    // somebody puts a space in -- which they always do.
    if (token.text.trim() !== '') previous = token;
  };

  while (at < source.length) {
    const character = source[at]!;

    if (character === '#') {
      const newline = source.indexOf('\n', at);
      const end = newline === -1 ? source.length : newline;
      push('comment', at, end);
      at = end;
      continue;
    }

    if (source.startsWith('"""', at)) {
      const close = source.indexOf('"""', at + 3);
      const end = close === -1 ? source.length : close + 3;
      push('string', at, end);
      at = end;
      continue;
    }

    if (character === '"' || character === "'") {
      let scan = at + 1;
      while (scan < source.length) {
        if (source[scan] === '\\') {
          scan += 2;
          continue;
        }
        if (source[scan] === character || source[scan] === '\n') break;
        scan += 1;
      }
      const end = source[scan] === character ? scan + 1 : scan;
      push('string', at, end);
      at = end;
      continue;
    }

    if (character >= '0' && character <= '9') {
      NUMBER.lastIndex = at;
      const found = NUMBER.exec(source);
      if (found) {
        push('number', at, at + found[0].length);
        at += found[0].length;
        continue;
      }
    }

    if (/[A-Za-z_]/.test(character)) {
      WORD.lastIndex = at;
      const found = WORD.exec(source);
      if (found) {
        // A trailing `.` or `-` belongs to what follows, not to the word: a
        // hyphenated flow name and `planned.narration` both scan as one word,
        // but `it.` at the end of a line does not eat the dot.
        const word = found[0].replace(/[.-]+$/, '');
        push(wordKind(word, previous), at, at + word.length);
        at += word.length;
        continue;
      }
    }

    if (source.startsWith('->', at) || source.startsWith('<-', at)) {
      push('operator', at, at + 2);
      at += 2;
      continue;
    }

    if ('|=<>!+-*/%'.includes(character)) {
      push('operator', at, at + 1);
      at += 1;
      continue;
    }

    if (PUNCTUATION.has(character)) {
      push('punctuation', at, at + 1);
      at += 1;
      continue;
    }

    // Whitespace and anything else: one run, so the output stays total.
    let end = at + 1;
    while (end < source.length && /\s/.test(source[end]!) && /\s/.test(character)) {
      end += 1;
    }
    push('plain', at, end);
    at = end;
  }

  return tokens;
}
