/**
 * Which strings are flows, and where they start.
 *
 * The other place an off-by-one does real damage: every diagnostic, completion and
 * hover inside a fragment is placed by adding the fragment's start offset, so a
 * span that is one character out puts every squiggle in the file one character out.
 *
 * The rule is the one the JetBrains injector uses — a string whose first real word
 * is `flow`, followed by a name and a `{` — so the same fragment is recognised in
 * both editors, and prose merely mentioning a flow is recognised in neither.
 */

import assert from 'node:assert/strict';
import {test} from 'node:test';
import {fragmentSpans} from '../dist/testable/fragmentSpans.mjs';

/** The text of the one span in `source`, for a test that is about the content. */
function only(source) {
  const spans = fragmentSpans(source);
  assert.equal(spans.length, 1, `expected one fragment, got ${spans.length}`);
  return source.slice(spans[0].start, spans[0].end);
}

test('finds a flow in a Python triple-quoted string', () => {
  const source = [
    'program = flow.loads("""',
    'flow shout {',
    '  in words: string stream',
    '}',
    '""")',
    '',
  ].join('\n');
  const text = only(source);
  assert.match(text, /^\nflow shout \{/);
  assert.match(text, /in words: string stream/);
  // The span is the content, without the quotes: an offset into it is an offset
  // into the flow, which is what the language is asked about.
  assert.doesNotMatch(text, /"""/);
});

test('the span starts exactly after the opening quote', () => {
  const source = 'x = """\nflow t {\n  in q: string\n}\n"""\n';
  const [span] = fragmentSpans(source);
  assert.equal(source.slice(span.start - 3, span.start), '"""');
  assert.equal(source.slice(span.end, span.end + 3), '"""');
  // And the arithmetic every feature relies on: offset zero of the fragment is
  // `span.start` of the host.
  assert.equal(source.slice(span.start, span.start + 1), '\n');
});

test('finds a flow in a single-quoted Python block and a template literal', () => {
  assert.match(only("x = '''\nflow t {\n  in q: string\n}\n'''\n"), /flow t \{/);
  assert.match(only('const p = `\nflow t {\n  in q: string\n}\n`;\n'), /flow t \{/);
});

test('a string that merely mentions a flow is not one', () => {
  const source = [
    'doc = """',
    'This module builds a flow at run time. A flow is text, so it travels.',
    'Call flow.loads on it.',
    '"""',
    '',
  ].join('\n');
  assert.deepEqual(fragmentSpans(source), []);
});

test('the keyword may be shouted, as every keyword may', () => {
  assert.match(only('x = """\nFLOW t {\n  IN q: string\n}\n"""\n'), /FLOW t \{/);
});

test('a leading comment before the declaration is still a flow', () => {
  const source = 'x = """\n# what this does\nflow t {\n  in q: string\n}\n"""\n';
  assert.match(only(source), /flow t \{/);
});

test('finds several flows in one file, and does not nest', () => {
  const source = [
    'a = """',
    'flow one {',
    '  in q: string',
    '}',
    '"""',
    'b = """',
    'flow two {',
    '  in q: string',
    '}',
    '"""',
    '',
  ].join('\n');
  const spans = fragmentSpans(source);
  assert.equal(spans.length, 2);
  assert.match(source.slice(spans[0].start, spans[0].end), /flow one/);
  assert.match(source.slice(spans[1].start, spans[1].end), /flow two/);
  // The second span begins after the first one's closing quote, so a `"""` inside
  // the first is never read as opening the second.
  assert.ok(spans[0].end < spans[1].start);
});

test('a string nobody closed is still a fragment, to the end of the file', () => {
  // What a file somebody is in the middle of typing looks like, and not a reason
  // to stop offering them anything.
  const source = 'x = """\nflow t {\n  in q: string\n';
  const [span] = fragmentSpans(source);
  assert.equal(span.end, source.length);
});

test('a flow-shaped string that is not a declaration is not a fragment', () => {
  // `flow` with no name, and a name with no brace: neither opens a flow.
  assert.deepEqual(fragmentSpans('x = """\nflow {\n}\n"""\n'), []);
  assert.deepEqual(fragmentSpans('x = """\nflow named\n"""\n'), []);
});
