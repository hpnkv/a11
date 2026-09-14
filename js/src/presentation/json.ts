/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

export type JsonTokenKind =
  "key" | "string" | "keyword" | "number" | "punctuation";

export interface JsonToken {
  kind: JsonTokenKind;
  from: number;
  to: number;
}

const JSON_TOKENS = new RegExp(
  [
    '("(?:\\\\.|[^"\\\\])*")\\s*(?=:)',
    '("(?:\\\\.|[^"\\\\])*")',
    "\\b(true|false|null)\\b",
    "(-?\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?)",
    "([{}\\[\\],:])",
  ].join("|"),
  "g",
);

const TOKEN_KINDS: readonly JsonTokenKind[] = [
  "key",
  "string",
  "keyword",
  "number",
  "punctuation",
];

/** Token ranges only: renderers choose elements and class names. */
export function jsonTokens(text: string): JsonToken[] {
  const tokens: JsonToken[] = [];
  for (const match of text.matchAll(JSON_TOKENS)) {
    const from = match.index ?? 0;
    const group = TOKEN_KINDS.findIndex(
      (_kind, index) => match[index + 1] !== undefined,
    );
    if (group >= 0) {
      tokens.push({
        kind: TOKEN_KINDS[group]!,
        from,
        to: from + match[0].length,
      });
    }
  }
  return tokens;
}

export interface JsonDocument {
  text: string;
  value?: unknown;
  error?: string;
  tokens: JsonToken[];
}

export function readJson(text: string): JsonDocument {
  const tokens = jsonTokens(text);
  if (text.trim() === "") return { text, tokens };
  try {
    return { text, tokens, value: JSON.parse(text) };
  } catch (error) {
    let message = "Invalid JSON.";
    try {
      message = error instanceof Error ? error.message : String(error);
    } catch {
      // A hostile thrown value is still represented as an editor error.
    }
    return {
      text,
      tokens,
      error: message,
    };
  }
}

export function indentJson(
  text: string,
  selectionStart: number,
  selectionEnd: number,
  indentation = "  ",
): { text: string; selectionStart: number; selectionEnd: number } {
  return {
    text: `${text.slice(0, selectionStart)}${indentation}${text.slice(selectionEnd)}`,
    selectionStart: selectionStart + indentation.length,
    selectionEnd: selectionStart + indentation.length,
  };
}
