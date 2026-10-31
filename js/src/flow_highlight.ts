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

/**
 * Browser fallback colouring from the generated native Flow vocabulary.
 * Hosts with a language transport use native semantic tokens.
 */

import {
  FLOW_KEYWORDS, FLOW_TYPES, FLOW_STAGES, FLOW_BUILTINS,
  FLOW_LOG_LEVELS, FLOW_CONSTANTS, FLOW_OPERATOR_WORDS,
  FLOW_STRING_PATTERN, FLOW_STRING_ESCAPE_PATTERN,
} from "./flow_vocabulary.js";

export {
  FLOW_KEYWORDS, FLOW_TYPES, FLOW_STAGES, FLOW_BUILTINS,
  FLOW_LOG_LEVELS, FLOW_CONSTANTS, FLOW_OPERATOR_WORDS,
};

/** What a run of characters is, for colouring. */
export type FlowTokenKind =
  | "comment"
  | "string"
  | "string-escape"
  | "number"
  | "keyword"
  | "type"
  | "stage"
  | "builtin"
  | "level"
  | "constant"
  | "operator"
  | "punctuation"
  | "plain";

/** One run of characters, with where it is. */
export interface FlowToken {
  /** Syntax category used for colour and font treatment. */
  kind: FlowTokenKind;
  /** Inclusive UTF-16 source offset. */
  start: number;
  /** Exclusive UTF-16 source offset. */
  end: number;
  /** Source characters in this token. */
  text: string;
}

const KEYWORDS = new Set(FLOW_KEYWORDS);
const TYPES = new Set(FLOW_TYPES);
const STAGES = new Set(FLOW_STAGES);
const PHRASE_STAGES = FLOW_STAGES.filter((word) => word.includes(" ")).map(
  (word) => new RegExp(
    word.split(" ").map((part) => `(?:${part}|${part.toUpperCase()})`).join("[ \t]+")
      + "(?![A-Za-z0-9_.-])", "y",
  ),
);

const BUILTINS = new Set(FLOW_BUILTINS);
const LEVELS = new Set(FLOW_LOG_LEVELS);
const CONSTANTS = new Set(FLOW_CONSTANTS);
const OPERATOR_WORDS = new Set(FLOW_OPERATOR_WORDS);

/** A word, including the hyphenated builtins and dotted type names. */
const WORD = /[A-Za-z_][A-Za-z0-9_.-]*/y;
/** A number, with the duration and byte-size suffixes a flow may write. */
const NUMBER = /\d[\d_]*(?:\.\d+)?(?:ms|s|m|h|d|ns|us|kb|mb|gb)?/iy;
const PUNCTUATION = new Set("{}()[],:");
const STRING = new RegExp(FLOW_STRING_PATTERN, "uy");
const STRING_ESCAPE = new RegExp(FLOW_STRING_ESCAPE_PATTERN, "gu");

function wordKind(word: string, previous: FlowToken | null): FlowTokenKind {
  // After a `|`, a word is the stage it names -- which is the one piece of
  // context worth keeping, because `log`, `text` and `json` are a statement, a
  // type and a stage depending on where they stand, and the pipe settles it.
  const piped = previous?.kind === "operator" && previous.text === "|";
  if (piped && STAGES.has(word)) return "stage";
  // A level only follows `log`/`logf`, and `error` is otherwise an ordinary word.
  if (
    previous !== null &&
    (previous.text === "log" || previous.text === "logf") &&
    LEVELS.has(word)
  ) {
    return "level";
  }
  if (CONSTANTS.has(word)) return "constant";
  if (OPERATOR_WORDS.has(word)) return "operator";
  if (KEYWORDS.has(word)) return "keyword";
  if (TYPES.has(word)) return "type";
  if (BUILTINS.has(word)) return "builtin";
  if (STAGES.has(word)) return "stage";
  return "plain";
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
    if (token.text.trim() !== "") previous = token;
  };

  while (at < source.length) {
    const character = source[at]!;

    if (character === "#") {
      const newline = source.indexOf("\n", at);
      const end = newline === -1 ? source.length : newline;
      push("comment", at, end);
      at = end;
      continue;
    }

    if (character === '"') {
      STRING.lastIndex = at;
      const found = STRING.exec(source);
      if (found) {
        const end = at + found[0].length;
        let part = at;
        for (const escape of found[0].matchAll(STRING_ESCAPE)) {
          const start = at + escape.index;
          if (part < start) push("string", part, start);
          part = start + escape[0].length;
          push("string-escape", start, part);
        }
        if (part < end) push("string", part, end);
        at = end;
        continue;
      }
    }

    if (character >= "0" && character <= "9") {
      NUMBER.lastIndex = at;
      const found = NUMBER.exec(source);
      if (found) {
        push("number", at, at + found[0].length);
        at += found[0].length;
        continue;
      }
    }

    const phrase = PHRASE_STAGES.find((pattern) => {
      pattern.lastIndex = at;
      return pattern.test(source);
    });
    if (phrase) {
      const end = phrase.lastIndex;
      push("stage", at, end);
      at = end;
      continue;
    }

    if (/[A-Za-z_]/.test(character)) {
      WORD.lastIndex = at;
      const found = WORD.exec(source);
      if (found) {
        // A trailing `.` or `-` belongs to what follows, not to the word: a
        // hyphenated flow name and `planned.narration` both scan as one word,
        // but `it.` at the end of a line does not eat the dot.
        const word = found[0].replace(/[.-]+$/, "");
        push(wordKind(word, previous), at, at + word.length);
        at += word.length;
        continue;
      }
    }

    if (source.startsWith("->", at) || source.startsWith("<-", at)) {
      push("operator", at, at + 2);
      at += 2;
      continue;
    }

    if ("|=<>!+-*/%".includes(character)) {
      push("operator", at, at + 1);
      at += 1;
      continue;
    }

    if (PUNCTUATION.has(character)) {
      push("punctuation", at, at + 1);
      at += 1;
      continue;
    }

    // Whitespace and anything else: one run, so the output stays total.
    let end = at + 1;
    while (
      end < source.length &&
      /\s/.test(source[end]!) &&
      /\s/.test(character)
    ) {
      end += 1;
    }
    push("plain", at, end);
    at = end;
  }

  return tokens;
}
