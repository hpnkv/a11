/**
 * Applies a unified diff to a file using its context.
 *
 * This implements the same matching rules and test cases as
 * `intellij-plugin/.../tools/Patch.kt`. The editor document APIs require
 * separate TypeScript and Kotlin implementations.
 *
 * The implementation follows three constraints shared with the Kotlin version:
 *
 * - Hunks use context, with the `@@` line number tried first as a hint.
 * - Every hunk must match before any edit is applied. Mismatch errors include
 *   the current file text.
 * - Context lines without a leading marker and block-indented markers are
 *   accepted when their text matches the file.
 */

/** One line of a diff: what it does, and the text either way of reading it. */
interface PatchLine {
  /** `+`, `-`, or ' ' for a line the hunk keeps. */
  kind: '+' | '-' | ' ';
  /** The line with its diff marker removed. */
  text: string;
  /**
   * The line exactly as written, marker and all.
   *
   * Used when a context line omitted its leading `' '` marker. This form is
   * ambiguous with content indented by one fewer space, so the file must match.
   */
  raw: string;
}

interface Hunk {
  lines: PatchLine[];
  /** The 0-based line the `@@` header claims, where it had one. */
  hintLine?: number;
}

/** The lines a hunk expects to find. */
function before(hunk: Hunk): PatchLine[] {
  return hunk.lines.filter((line) => line.kind !== '+');
}

/** The lines a hunk leaves behind. */
function after(hunk: Hunk): PatchLine[] {
  return hunk.lines.filter((line) => line.kind !== '-');
}

/**
 * Whether a patch line and a file line are the same line.
 *
 * Trailing whitespace is ignored because transport or editors may strip it.
 * Leading whitespace remains significant.
 */
function sameLine(expected: string, actual: string): boolean {
  return expected.replace(/\s+$/, '') === actual.replace(/\s+$/, '');
}

const HEADER = /^@@\s*-(\d+)(?:,\d+)?\s+\+\d+(?:,\d+)?\s*@@/;

/**
 * Every hunk of a unified diff, in order.
 *
 * File headers are ignored because the target file is a separate input. This
 * also permits applying a patch generated for one path to an explicit target.
 */
function parseHunks(patch: string, indented: boolean): Hunk[] {
  const hunks: Hunk[] = [];
  let current: Hunk | undefined;
  // A patch that ends with a newline does not have an empty last line: that
  // break belongs to the line before it. Keeping it would add a phantom context
  // line, which is a hunk that matches somewhere else entirely.
  const lines = patch.split('\n');
  if (lines.length > 0 && lines[lines.length - 1] === '') lines.pop();

  for (const raw of lines) {
    // With `indented`, whatever whitespace was put in front of the marker comes
    // off first, which is the second slip a written-by-hand diff makes. Before
    // the header is looked for, not after: an indented `@@` line read as
    // written is a context line, and a hunk that swallowed its own header
    // matches nothing.
    const line = indented ? raw.replace(/^[ \t]+/, '') : raw;
    const header = HEADER.exec(line);
    if (header) {
      current = {lines: [], hintLine: Math.max(0, Number(header[1]) - 1)};
      hunks.push(current);
      continue;
    }
    if (line.startsWith('---') || line.startsWith('+++') || line.startsWith('diff ')) {
      continue;
    }
    if (!current) {
      // A hunk with no `@@` header at all: the whole patch is one hunk, placed
      // by its context alone.
      if (line === '') continue;
      current = {lines: []};
      hunks.push(current);
    }
    const marker = line.charAt(0);
    if (marker === '+' || marker === '-') {
      current.lines.push({kind: marker, text: line.slice(1), raw: line});
    } else if (marker === ' ' || line === '') {
      current.lines.push({kind: ' ', text: line.slice(1), raw: line});
    } else if (line.startsWith('\\')) {
      // `\ No newline at end of file`, which is a note rather than a line.
      continue;
    } else {
      // A context line that lost its leading space. `raw` is what will match.
      current.lines.push({kind: ' ', text: line, raw: line});
    }
  }
  return hunks.filter((hunk) => hunk.lines.length > 0);
}

/** One hunk, where it was found, and the lines to leave there. */
export interface Placed {
  hunk: Hunk;
  at: number;
  replacement: string[];
}

/**
 * The lines to leave at `start` if the hunk matches the file there, else
 * undefined.
 *
 * Both spellings of every kept and removed line are tried, and whichever
 * matched is what goes back: a kept line is the *file's* line, not the patch's
 * copy of it.
 */
function matchAt(file: string[], hunk: Hunk, start: number): string[] | undefined {
  const expected = before(hunk);
  const kept: string[] = [];
  for (let offset = 0; offset < expected.length; offset += 1) {
    const actual = file[start + offset];
    if (actual === undefined) return undefined;
    const line = expected[offset];
    if (!sameLine(line.text, actual) && !sameLine(line.raw, actual)) return undefined;
    kept.push(actual);
  }
  const replacement: string[] = [];
  let taken = 0;
  for (const line of hunk.lines) {
    if (line.kind === '+') {
      replacement.push(line.text);
    } else if (line.kind === '-') {
      taken += 1;
    } else {
      replacement.push(kept[taken]);
      taken += 1;
    }
  }
  return replacement;
}

/** Where a hunk fits, searching from `searchFrom` downwards. */
function locateHunk(file: string[], hunk: Hunk, searchFrom: number): Placed {
  const expected = before(hunk);
  if (expected.length === 0) {
    if (hunk.hintLine === undefined) {
      throw new Error(
        "A hunk that only adds lines needs an '@@' header to say where they go.",
      );
    }
    if (hunk.hintLine > file.length) {
      throw new Error(
        `The hunk at line ${hunk.hintLine + 1} is past the end of the file.`,
      );
    }
    return {
      hunk,
      at: Math.min(Math.max(hunk.hintLine, searchFrom), file.length),
      replacement: after(hunk).map((line) => line.text),
    };
  }

  const candidates: number[] = [];
  if (hunk.hintLine !== undefined && hunk.hintLine >= searchFrom) {
    candidates.push(hunk.hintLine);
  }
  for (let start = searchFrom; start < file.length; start += 1) candidates.push(start);

  const seen = new Set<number>();
  for (const start of candidates) {
    if (seen.has(start)) continue;
    seen.add(start);
    if (start + expected.length > file.length) continue;
    const replacement = matchAt(file, hunk, start);
    if (replacement) return {hunk, at: start, replacement};
  }

  // Refused, with what is actually there: nothing is applied on a near miss.
  const wanted = expected.map((line) => line.text).join('\n');
  let found = '';
  if (hunk.hintLine !== undefined && hunk.hintLine < file.length) {
    const to = Math.min(hunk.hintLine + expected.length, file.length);
    found = file.slice(hunk.hintLine, to).join('\n');
  }
  throw new Error(
    'This hunk does not match the file, so nothing was applied:\n\n' +
      wanted +
      (found
        ? `\n\nWhat is at line ${(hunk.hintLine ?? 0) + 1} instead:\n\n${found}`
        : '') +
      '\n\nRead the file again and patch what is there.',
  );
}

/** Every hunk placed against the file, top to bottom, or the first mismatch. */
function locateAll(file: string[], hunks: Hunk[]): Placed[] {
  const placed: Placed[] = [];
  let searchFrom = 0;
  for (const hunk of hunks) {
    const one = locateHunk(file, hunk, searchFrom);
    placed.push(one);
    searchFrom = one.at + before(one.hunk).length;
  }
  return placed;
}

export interface PatchOutcome {
  /** The whole file, patched. */
  text: string;
  /** How many lines the hunks left behind, and how many they replaced. */
  added: number;
  removed: number;
  hunks: number;
}

/**
 * `patch` applied to `text`.
 *
 * Throws with what is in the file instead when a hunk does not match, and
 * applies nothing in that case: the caller turns one outcome into one editor
 * edit, so a patch either lands whole or does not land.
 *
 * Parsing first uses the patch as written, then retries with unindented markers
 * to support diffs copied from a list or quoted block.
 */
export function applyPatch(text: string, patch: string): PatchOutcome {
  const newline = text.includes('\r\n') ? '\r\n' : '\n';
  const file = text.split(/\r?\n/);
  // A trailing newline makes an empty last element, which is the end of the
  // file rather than a line of it. Remembered so it can be put back.
  const trailing = file.length > 0 && file[file.length - 1] === '';
  if (trailing) file.pop();

  let hunks = parseHunks(patch, false);
  let placed: Placed[];
  try {
    if (hunks.length === 0) throw new Error('The patch has no hunks in it.');
    placed = locateAll(file, hunks);
  } catch (first) {
    const indented = parseHunks(patch, true);
    if (indented.length === 0) throw first;
    // If the indented reading fails too, the *first* complaint is the useful
    // one: it is about the patch as it was actually written.
    try {
      placed = locateAll(file, indented);
      hunks = indented;
    } catch {
      throw first;
    }
  }

  // Bottom up, so an earlier edit does not move a later one's lines.
  const out = [...file];
  for (const one of [...placed].reverse()) {
    out.splice(one.at, before(one.hunk).length, ...one.replacement);
  }
  const added = placed.reduce((sum, one) => sum + after(one.hunk).length, 0);
  const removed = placed.reduce((sum, one) => sum + before(one.hunk).length, 0);
  return {
    text: out.join(newline) + (trailing ? newline : ''),
    added,
    removed,
    hunks: placed.length,
  };
}
