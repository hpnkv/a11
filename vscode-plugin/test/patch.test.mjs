/**
 * The patch algorithm, which is the one piece of real logic in this extension and
 * the one whose failure mode is expensive: a fuzzy match silently rewrites the
 * wrong lines.
 *
 * Every case here is a decision `Patch.kt` makes, so the two editors agree about
 * what a given diff does to a given file. These run in plain Node with no editor,
 * because the algorithm is text in and text out — the VSCode-shaped part is a
 * single `WorkspaceEdit` around it, which the integration test covers.
 */

import assert from 'node:assert/strict';
import {test} from 'node:test';
import {applyPatch} from "../dist/testable/tools/patch.mjs";

const FILE = ['def one():', '    return 1', '', 'def two():', '    return 2', ''].join('\n');

test('applies a hunk placed by its @@ header', () => {
  const patch = [
    '@@ -1,2 +1,2 @@',
    ' def one():',
    '-    return 1',
    '+    return 11',
    '',
  ].join('\n');
  const out = applyPatch(FILE, patch);
  assert.match(out.text, /return 11/);
  assert.match(out.text, /return 2/);
  assert.equal(out.hunks, 1);
});

test('places a hunk by its context when the header lies', () => {
  // The numbers are wrong by twenty lines; the context is right. A patch written
  // by hand or by a model against a file it read earlier looks exactly like this.
  const patch = [
    '@@ -21,2 +21,2 @@',
    ' def two():',
    '-    return 2',
    '+    return 22',
    '',
  ].join('\n');
  const out = applyPatch(FILE, patch);
  assert.match(out.text, /return 22/);
  assert.match(out.text, /return 1\n/);
});

test('refuses a hunk that does not match, and says what is there', () => {
  const patch = [
    '@@ -1,2 +1,2 @@',
    ' def three():',
    '-    return 3',
    '+    return 33',
    '',
  ].join('\n');
  assert.throws(
    () => applyPatch(FILE, patch),
    (error) => {
      assert.match(error.message, /does not match the file/);
      assert.match(error.message, /nothing was applied/);
      // The useful part: what is actually at that line.
      assert.match(error.message, /def one\(\)/);
      return true;
    },
  );
});

test('reads a context line that lost its leading space', () => {
  // The commonest slip in a written-by-hand diff, and one only the file can
  // settle: a line of an indented file starts with a space anyway.
  const patch = [
    '@@ -1,2 +1,2 @@',
    'def one():',
    '-    return 1',
    '+    return 11',
    '',
  ].join('\n');
  const out = applyPatch(FILE, patch);
  assert.match(out.text, /return 11/);
});

test('reads a patch whose markers are indented as a block', () => {
  // What a diff looks like after a trip through a bulleted list or a quoted block.
  const patch = [
    '    @@ -1,2 +1,2 @@',
    '     def one():',
    '    -    return 1',
    '    +    return 11',
    '',
  ].join('\n');
  const out = applyPatch(FILE, patch);
  assert.match(out.text, /return 11/);
});

test('keeps the file’s own line, not the patch’s copy of it', () => {
  // The context line differs in trailing whitespace, which is ignored; what goes
  // back is what the file had.
  const patch = [
    '@@ -1,2 +1,2 @@',
    ' def one():   ',
    '-    return 1',
    '+    return 11',
    '',
  ].join('\n');
  const out = applyPatch(FILE, patch);
  assert.match(out.text, /^def one\(\):\n/);
});

test('applies several hunks top to bottom', () => {
  const patch = [
    '@@ -1,2 +1,2 @@',
    ' def one():',
    '-    return 1',
    '+    return 11',
    '@@ -4,2 +4,2 @@',
    ' def two():',
    '-    return 2',
    '+    return 22',
    '',
  ].join('\n');
  const out = applyPatch(FILE, patch);
  assert.equal(out.hunks, 2);
  assert.match(out.text, /return 11/);
  assert.match(out.text, /return 22/);
});

test('inserts a hunk that only adds', () => {
  const patch = ['@@ -3,0 +3,1 @@', '+# between', ''].join('\n');
  const out = applyPatch(FILE, patch);
  assert.match(out.text, /# between/);
});

test('deletes without leaving an empty line behind', () => {
  const patch = ['@@ -2,1 +2,0 @@', '-    return 1', ''].join('\n');
  const out = applyPatch(FILE, patch);
  assert.doesNotMatch(out.text, /return 1$/m);
  assert.equal(out.text.split('\n')[1], '');
});

test('keeps the file’s newline style and its trailing newline', () => {
  const crlf = FILE.replaceAll('\n', '\r\n');
  const patch = [
    '@@ -1,2 +1,2 @@',
    ' def one():',
    '-    return 1',
    '+    return 11',
    '',
  ].join('\n');
  const out = applyPatch(crlf, patch);
  assert.ok(out.text.includes('\r\n'), 'CRLF survives');
  assert.ok(out.text.endsWith('\r\n'), 'the trailing newline survives');
});

test('counts what it added and removed', () => {
  const patch = [
    '@@ -1,2 +1,3 @@',
    ' def one():',
    '-    return 1',
    '+    x = 1',
    '+    return x',
    '',
  ].join('\n');
  const out = applyPatch(FILE, patch);
  assert.equal(out.removed, 2);
  assert.equal(out.added, 3);
});
