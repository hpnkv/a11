/**
 * Where the next line lands after Enter, inside a flow.
 *
 * A flow's blocks are `{ }`, and the editor's own default -- copy the previous
 * line's indentation -- already gets those right, because the line that opens
 * one is exactly one level shallower than what follows it. What it cannot know
 * is a *continuation*: a `skip` list still running past a `,`, a pipeline left
 * open after `|` or `->`, an `(o1, o2 of act)` group whose `)` has not been
 * typed yet. Each of those wants one more level than the statement they are
 * part of, and the line after the continuation ends comes back to the block's
 * own indent -- which is what "provably ends" means here: no trailing `,`,
 * `|`, `->` or `<-`, and every `(`/`[` opened since the block's own line is
 * closed again. Re-derived fresh on every Enter from the whole document, so
 * there is no state to fall out of step with what is actually written.
 *
 * Its own module, with nothing from the editor in it, on purpose: this is
 * called from `provideOnTypeFormattingEdits`, which runs synchronously right
 * after the character that triggered it, and a request to the language
 * server has no place there. The punctuation is enough to answer "is this a
 * continuation" -- nothing here needs to know what a word means, which is
 * also what makes it checkable with no editor running.
 */

interface Token {
  kind: 'brace-open' | 'brace-close' | 'group-open' | 'group-close' |
    'comma' | 'pipe' | 'arrow' | 'carry' | 'string' | 'comment' | 'other';
  text: string;
}

const GROUP_OPEN = new Set(['(', '[']);
const GROUP_CLOSE = new Set([')', ']']);

/** The punctuation and quoting of `text`, and nothing about what a word means. */
function tokenize(text: string): Token[] {
  const tokens: Token[] = [];
  let at = 0;
  while (at < text.length) {
    const letter = text[at];
    if (letter === '#') {
      const stop = text.indexOf('\n', at);
      const end = stop < 0 ? text.length : stop;
      tokens.push({kind: 'comment', text: text.slice(at, end)});
      at = end;
      continue;
    }
    if (letter === '"') {
      const triple = text.startsWith('"""', at);
      const quote = triple ? '"""' : '"';
      let stop = at + quote.length;
      while (stop < text.length) {
        if (text[stop] === '\\') {
          stop += 2;
          continue;
        }
        if (text.startsWith(quote, stop)) {
          stop += quote.length;
          break;
        }
        // A single-quoted string cannot span a line.
        if (!triple && text[stop] === '\n') break;
        stop++;
      }
      const end = Math.min(stop, text.length);
      tokens.push({kind: 'string', text: text.slice(at, end)});
      at = end;
      continue;
    }
    if (/\s/.test(letter)) {
      at++;
      continue;
    }
    if (text.startsWith('->', at)) {
      tokens.push({kind: 'arrow', text: '->'});
      at += 2;
      continue;
    }
    if (text.startsWith('<-', at)) {
      tokens.push({kind: 'carry', text: '<-'});
      at += 2;
      continue;
    }
    if (letter === '{') {
      tokens.push({kind: 'brace-open', text: letter});
      at++;
      continue;
    }
    if (letter === '}') {
      tokens.push({kind: 'brace-close', text: letter});
      at++;
      continue;
    }
    if (GROUP_OPEN.has(letter)) {
      tokens.push({kind: 'group-open', text: letter});
      at++;
      continue;
    }
    if (GROUP_CLOSE.has(letter)) {
      tokens.push({kind: 'group-close', text: letter});
      at++;
      continue;
    }
    if (letter === ',') {
      tokens.push({kind: 'comma', text: letter});
      at++;
      continue;
    }
    if (letter === '|') {
      tokens.push({kind: 'pipe', text: letter});
      at++;
      continue;
    }
    // Everything else -- identifiers, numbers, other punctuation -- is a
    // continuation only through the string/comment cases above, so its exact
    // shape does not matter here.
    tokens.push({kind: 'other', text: letter});
    at++;
  }
  return tokens;
}

/** Whether a string token's own text ends with the quote it opened with. */
function isClosedString(raw: string): boolean {
  const quote = raw.startsWith('"""') ? '"""' : '"';
  return raw.length >= quote.length * 2 && raw.endsWith(quote);
}

/**
 * The indentation for the line after `before`, or `null` to leave whatever the
 * editor's default already put there.
 *
 * `null` inside an unterminated multi-line string: the shape of a flow has no
 * opinion about a description still running, and copying the previous line --
 * what the default already does -- is what keeps it aligned.
 */
export function indentAfter(before: string, unit: string): string | null {
  const tokens = tokenize(before);
  if (tokens.length === 0) return null;

  const last = tokens[tokens.length - 1];
  if (last.kind === 'string' && !isClosedString(last.text)) return null;

  let braceDepth = 0;
  let groupDepth = 0;
  let continues = false;
  for (const token of tokens) {
    switch (token.kind) {
      case 'brace-open':
        braceDepth++;
        break;
      case 'brace-close':
        braceDepth = Math.max(0, braceDepth - 1);
        break;
      case 'group-open':
        groupDepth++;
        break;
      case 'group-close':
        groupDepth = Math.max(0, groupDepth - 1);
        break;
      default:
        break;
    }
    if (token.kind !== 'comment') {
      continues = token.kind === 'comma' || token.kind === 'pipe' ||
        token.kind === 'arrow' || token.kind === 'carry';
    }
  }

  const blockWidth = braceDepth * unit.length;
  if (groupDepth <= 0 && !continues) return unit.repeat(braceDepth);

  // A continuation: one level deeper than the block, unless the line just
  // finished was itself already a continuation -- in which case that line's
  // own width is the one running, and this one matches it rather than adding
  // another level on top.
  const previousLineStart = before.lastIndexOf('\n') + 1;
  let indentEnd = previousLineStart;
  while (indentEnd < before.length && (before[indentEnd] === ' ' || before[indentEnd] === '\t')) {
    indentEnd++;
  }
  const previousIndent = before.slice(previousLineStart, indentEnd);
  return previousIndent.length > blockWidth ? previousIndent : unit.repeat(braceDepth + 1);
}
