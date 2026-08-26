/**
 * The browser highlighter's vocabulary, pinned to the generated one.
 *
 * `editors/pygments/a11flow_lexer.py` is written by `a11 flow syntax --target
 * pygments --generate`, so it is the language talking about itself. The
 * TypeScript scanner is hand-written -- a web page has no host editor to load a
 * definition file into -- and this is what keeps the two from drifting: a word
 * added to the language fails here rather than being noticed by eye a release
 * later.
 *
 * Only the lists are pinned, not the shape. Pygments needs a rule ordering and a
 * token hierarchy; a page needs a set per colour, and the two will never be the
 * same structure.
 */

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

import {
  FLOW_BUILTINS,
  FLOW_CONSTANTS,
  FLOW_LOG_LEVELS,
  FLOW_OPERATOR_WORDS,
  FLOW_STAGES,
  FLOW_TYPES,
  tokenizeFlow,
} from '../dist/index.js';

const LEXER = fileURLToPath(
  new URL('../../editors/pygments/a11flow_lexer.py', import.meta.url),
);

/** The words in one `_keywords(...)` list of the generated lexer. */
function generated(name) {
  const source = readFileSync(LEXER, 'utf8');
  const at = source.indexOf(`${name} = _keywords(`);
  assert.ok(at >= 0, `${name} is not in the generated lexer any more`);
  const open = source.indexOf('(', at);
  const close = source.indexOf(')', open);
  return new Set(
    source
      .slice(open + 1, close)
      .split(',')
      .map((piece) => piece.trim().replace(/^["']|["']$/g, ''))
      .filter((word) => word.length > 0),
  );
}

const CASES = [
  ['TYPES', FLOW_TYPES],
  ['STAGES', FLOW_STAGES],
  ['BUILTINS', FLOW_BUILTINS],
  ['LOG_LEVELS', FLOW_LOG_LEVELS],
  ['CONSTANTS', FLOW_CONSTANTS],
  ['OPERATOR_WORDS', FLOW_OPERATOR_WORDS],
];

for (const [name, ours] of CASES) {
  test(`the ${name} the page colours are the language's own`, () => {
    const theirs = generated(name);
    const missing = [...theirs].filter((word) => !ours.includes(word));
    const extra = ours.filter((word) => !theirs.has(word));
    assert.deepEqual(missing, [], `${name} the highlighter does not know`);
    assert.deepEqual(extra, [], `${name} the language does not have`);
  });
}

test('every character comes back exactly once', () => {
  // A highlighter is drawn behind the text it colours, so a dropped character
  // is a visible corruption of somebody's source rather than a wrong colour.
  const source = [
    '# a flow',
    'flow ask {',
    '  in  question: string required "What to ask."',
    '  out reply:    string stream',
    '  said = call ask_model(prompt: question) timeout 30s',
    '  said.text_output | where it != "" | log info it -> reply',
    '  logf info "asked at %s" now()',
    '}',
    '',
  ].join('\n');

  const tokens = tokenizeFlow(source);
  assert.equal(tokens.map((token) => token.text).join(''), source);
  for (const [index, token] of tokens.entries()) {
    assert.equal(token.start, index === 0 ? 0 : tokens[index - 1].end);
  }
});

test('a word is read as what it stands for where it stands', () => {
  const kindOf = (source, text) =>
    tokenizeFlow(source).find((token) => token.text === text)?.kind;

  // `log` opens a statement, and after a pipe the same word is a stage.
  assert.equal(kindOf('log info "hi"', 'log'), 'keyword');
  assert.equal(kindOf('values | log info it -> out', 'log'), 'stage');
  // A level is a level only where one may stand.
  assert.equal(kindOf('log error "no"', 'error'), 'level');
  assert.equal(kindOf('error -> out', 'error'), 'plain');
  // And nothing inside a comment or a string is looked up at all.
  assert.equal(tokenizeFlow('# flow map join').length, 1);
  assert.equal(kindOf('"flow map join"', '"flow map join"'), 'string');
});
