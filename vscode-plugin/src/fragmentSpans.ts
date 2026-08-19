/**
 * Which strings are flows, and where each one's text runs.
 *
 * Its own module, with nothing from the editor in it, because this is where an
 * off-by-one would do real damage and it is a fact about a string: every
 * diagnostic, completion and hover inside a fragment is placed by adding the
 * fragment's start offset, so a span one character out puts every squiggle in the
 * file one character out. Text in, spans out, checkable with no editor running.
 *
 * The rule is the one the JetBrains injector uses — a string whose first real word
 * is `flow`, followed by a name and a `{` — so the same fragment is recognised in
 * both editors, and prose merely mentioning a flow is recognised in neither.
 */

/** Where one flow's text runs in a host document, in UTF-16 offsets. */
export interface Span {
  start: number;
  end: number;
}

/**
 * The quotes a flow may be written inside.
 *
 * Only the multi-line ones, because a flow is at least `flow x { .. }` and does not
 * fit on one line of anybody's string; scanning every `"..."` in every Python file
 * would be a lot of matching for a case that does not arise.
 */
const OPENERS = ['"""', "'''", '`'] as const;

/**
 * A content that is a flow: optional whitespace and comments, then `flow NAME {`.
 *
 * Either casing of the keyword, as the language allows every keyword to be shouted.
 */
const STARTS_A_FLOW = /^\s*(?:#[^\n]*\n\s*)*(?:flow|FLOW)\s+[A-Za-z_][A-Za-z0-9_-]*\s*\{/;

/** Every flow written in `text`, as offsets into it. */
export function fragmentSpans(text: string): Span[] {
  const found: Span[] = [];
  let at = 0;
  while (at < text.length) {
    const opener = OPENERS.find((quote) => text.startsWith(quote, at));
    if (!opener) {
      at += 1;
      continue;
    }
    const contentStart = at + opener.length;
    const close = text.indexOf(opener, contentStart);
    const contentEnd = close === -1 ? text.length : close;
    if (STARTS_A_FLOW.test(text.slice(contentStart, contentEnd))) {
      found.push({start: contentStart, end: contentEnd});
    }
    // Past this string either way: a quote inside one it did not open is not an
    // opener, and re-reading the content looking for one is how a scan finds a
    // fragment inside a fragment.
    at = close === -1 ? text.length : close + opener.length;
  }
  return found;
}
