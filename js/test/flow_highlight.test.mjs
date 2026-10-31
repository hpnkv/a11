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

/** Browser fallback colours use the generated native vocabulary. */

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

test('native string patterns split escapes in ordinary and triple quotes', () => {
  const source = String.raw`"é\0\n\"\\tail" """a\"""b\t
c""" flow`;
  const tokens = tokenizeFlow(source);
  assert.equal(tokens.map((token) => token.text).join(''), source);
  assert.deepEqual(
    tokens.filter((token) => token.kind === 'string-escape').map((token) => token.text),
    ['\\0', '\\n', '\\"', '\\\\', '\\"', '\\t'],
  );
  assert.equal(tokens.at(-1).kind, 'keyword');
  for (const incomplete of ['"a\\', '"""é\\0', '"a\\é"']) {
    const spans = tokenizeFlow(incomplete);
    assert.equal(spans.map((token) => token.text).join(''), incomplete);
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


test('new Flow syntax uses the generated browser vocabulary', () => {
  const source = 'declare flow f exported { from "m" import x\n values | take until it == 3 | consume -> out }';
  const tokens = tokenizeFlow(source);
  for (const word of ['declare', 'exported', 'from', 'import']) {
    assert.equal(tokens.find((token) => token.text === word)?.kind, 'keyword');
  }
  assert.equal(tokens.find((token) => token.text === 'take until')?.kind, 'stage');
  assert.equal(tokens.find((token) => token.text === 'consume')?.kind, 'stage');
  assert.equal(tokens.map((token) => token.text).join(''), source);
});
