/**
 * The indent Enter lands on, inside a flow.
 *
 * Text in, text out, with no editor running: what it answers depends only on
 * the punctuation before the caret, which `flowIndent.ts` reads without a
 * language server, on purpose -- `provideOnTypeFormattingEdits` runs
 * synchronously right after Enter, and a request to the server has no place
 * there. Kept in step with `FlowEnterHandlerTest.kt` on the JetBrains side,
 * since both editors are meant to agree.
 */

import assert from 'node:assert/strict';
import {test} from 'node:test';
import {indentAfter} from '../dist/testable/flowIndent.mjs';

const unit = '  ';

test('an ordinary line inside a block keeps the block\'s own indent', () => {
  assert.equal(indentAfter('flow f {\n  in a: string', unit), '  ');
});

test('a trailing comma indents one level deeper', () => {
  assert.equal(indentAfter('flow f {\n  skip a,', unit), '    ');
});

test('a running continuation keeps its own width rather than adding another level', () => {
  const before = 'flow f {\n  skip a,\n    b,';
  assert.equal(indentAfter(before, unit), '    ');
});

test('a manually deeper continuation is followed rather than reset', () => {
  const before = 'flow f {\n  skip a,\n      b,';
  assert.equal(indentAfter(before, unit), '      ');
});

test('the line after a continuation ends snaps back to the block', () => {
  const before = 'flow f {\n  skip a,\n    b\n  "x" -> out';
  assert.equal(indentAfter(before, unit), '  ');
});

test('a pipe left open continues the pipeline', () => {
  const before = 'flow f {\n  words | map strformat("x", it) |';
  assert.equal(indentAfter(before, unit), '    ');
});

test('an arrow left open continues onto its target', () => {
  assert.equal(indentAfter('flow f {\n  words ->', unit), '    ');
});

test('an open parenthesis continues until it closes', () => {
  assert.equal(indentAfter('flow f {\n  skip (o1, o2', unit), '    ');
});

test('closing the parenthesis on its own line still continues if a comma follows', () => {
  const before = 'flow f {\n  skip (o1, o2) of act,';
  assert.equal(indentAfter(before, unit), '    ');
});

test('a nested block indents one level past its own', () => {
  assert.equal(indentAfter('flow f {\n  for hit in hits {', unit), '    ');
});

test('leaving a nested block returns to its own level', () => {
  const before = 'flow f {\n  for hit in hits {\n    hit -> out\n  }';
  assert.equal(indentAfter(before, unit), '  ');
});

test('a comment does not itself count as a continuation', () => {
  assert.equal(indentAfter('flow f {\n  "x" -> out # done', unit), '  ');
});

test('a comma inside a string is not a continuation', () => {
  assert.equal(indentAfter('flow f {\n  "a, b" -> out', unit), '  ');
});

test('an unterminated multi-line string is left to the default', () => {
  const before = 'flow f {\n  describe """\n  still writing';
  assert.equal(indentAfter(before, unit), null);
});

test('a closed triple-quoted string is an ordinary line again', () => {
  const before = 'flow f {\n  describe """\n  two lines\n  """';
  assert.equal(indentAfter(before, unit), '  ');
});

test('the request\'s own combined example', () => {
  const before = [
    'flow f {',
    '  act1 = run action1(text: our_input)',
    '  skip our_input,',
    '    act1,',
    '    (o1, o2) of act2,',
  ].join('\n');
  assert.equal(indentAfter(before, unit), '    ');
});
