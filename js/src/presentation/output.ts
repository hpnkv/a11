/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import { safeJson } from "./value.js";

export interface OutputBlock {
  port: string;
  text: string;
  parts: string[];
  streamed: boolean;
  value: unknown;
  mimetype: string;
  count: number;
  omitted: number;
  values: unknown[];
  valueMimetypes: string[];
  textual: boolean;
  textTruncated: boolean;
}

export const MAX_OUTPUT_VALUES = 200;
export const MAX_TEXT_PARTS = 400;
export const MAX_TEXT_CHARS = 200_000;

export function isTextualPort(type: string): boolean {
  return type.startsWith("text/");
}

export function renderValue(value: unknown): string {
  if (typeof value === "string") return value;
  try {
    return JSON.stringify(value, null, 2) ?? String(value);
  } catch {
    try {
      return String(value);
    } catch {
      return "<unprintable value>";
    }
  }
}

/** Studio's bounded, immutable, per-port stream reduction. */
export function appendOutput(
  blocks: readonly OutputBlock[],
  port: string,
  value: unknown,
  textual: boolean,
  mimetype = "",
): OutputBlock[] {
  const text = renderValue(value);
  const at = blocks.findIndex((block) => block.port === port);
  if (at < 0) {
    return [
      ...blocks,
      {
        port,
        text,
        parts: [text],
        streamed: textual,
        value: textual ? text : value,
        mimetype,
        count: 1,
        omitted: 0,
        values: textual ? [] : [value],
        valueMimetypes: textual ? [] : [mimetype],
        textual,
        textTruncated: false,
      },
    ];
  }
  const grown = blocks.slice();
  const growing = grown[at]!;
  if (!textual) {
    const values = [...growing.values, value];
    const valueMimetypes = [...growing.valueMimetypes, mimetype];
    const excess = Math.max(0, values.length - MAX_OUTPUT_VALUES);
    if (excess) {
      values.splice(0, excess);
      valueMimetypes.splice(0, excess);
    }
    grown[at] = {
      ...growing,
      text,
      parts: [text],
      streamed: growing.streamed || growing.count > 0,
      value,
      mimetype: mimetype || growing.mimetype,
      count: growing.count + 1,
      omitted: growing.omitted + excess,
      values,
      valueMimetypes,
      textual: false,
    };
    return grown;
  }
  const parts = [...growing.parts, text];
  const excessParts = Math.max(0, parts.length - MAX_TEXT_PARTS);
  if (excessParts) parts.splice(0, excessParts);
  const joined = growing.text + text;
  const excessChars = Math.max(0, joined.length - MAX_TEXT_CHARS);
  const boundedText = excessChars ? joined.slice(excessChars) : joined;
  grown[at] = {
    ...growing,
    text: boundedText,
    parts,
    value: boundedText,
    mimetype: mimetype || growing.mimetype,
    count: growing.count + 1,
    omitted: growing.omitted + excessParts,
    streamed: true,
    textual: true,
    textTruncated: growing.textTruncated || excessChars > 0,
  };
  return grown;
}

export interface OutputViewState {
  joined: boolean;
  markdown: boolean;
  expanded: boolean;
}

export const DEFAULT_OUTPUT_VIEW: Readonly<OutputViewState> = Object.freeze({
  joined: true,
  markdown: true,
  expanded: false,
});

export function outputCopyText(
  block: OutputBlock,
  view: Pick<OutputViewState, "joined">,
): string {
  return block.textual
    ? view.joined
      ? block.text
      : safeJson(block.parts)
    : safeJson(block.values);
}
