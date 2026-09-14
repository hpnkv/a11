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

import * as vscode from 'vscode';
import type {FlowServer} from './flowClient.js';
import {FLOW_LANGUAGE} from './flowClient.js';
import {fragmentsIn} from './fragments.js';

export interface FlowPort {
  name: string;
  direction: 'in' | 'out';
  type: string;
  stream: boolean;
  required: boolean;
}

export interface RunnableFlow {
  source: string;
  name: string;
  path: string;
  line: number;
  ports: FlowPort[];
  tokens?: Array<{start: number; end: number; kind: string}>;
}

interface SymbolValue {
  name: string;
  kind: string;
  selection: {start: {offset: number; line: number}; end: {offset: number}};
  children?: Array<{
    name: string;
    kind: string;
    direction?: 'in' | 'out';
    type?: string;
    stream?: boolean;
    required?: boolean;
    detail?: string;
  }>;
}

interface Envelope {
  result?: {
    symbols?: SymbolValue[];
    tokens?: Array<{start: number; end: number; kind: string}>;
  };
}

/** Ask the native language service for runnable declarations in one editor. */
export async function runnableFlows(
  server: FlowServer | undefined,
  document: vscode.TextDocument,
): Promise<Array<{flow: RunnableFlow; range: vscode.Range}>> {
  if (!server) return [];
  const regions = document.languageId === FLOW_LANGUAGE
    ? [{text: document.getText(), offset: 0}]
    : fragmentsIn(document).map((one) => ({text: one.text, offset: one.offset}));
  const result: Array<{flow: RunnableFlow; range: vscode.Range}> = [];
  for (const region of regions) {
    const answer = await server.request({
      method: 'symbols',
      source: region.text,
      offsets: 'utf16',
    }) as Envelope | undefined;
    const tokenAnswer = await server.request({
      method: 'tokens',
      source: region.text,
      offsets: 'utf16',
    }) as Envelope | undefined;
    for (const symbol of answer?.result?.symbols ?? []) {
      if (symbol.kind !== 'flow') continue;
      const offset = region.offset + symbol.selection.start.offset;
      const start = document.positionAt(offset);
      const end = document.positionAt(region.offset + symbol.selection.end.offset);
      const ports: FlowPort[] = (symbol.children ?? [])
        .filter((child) => child.kind === 'port')
        .map(portFromSymbol)
        .filter((port): port is FlowPort => port !== undefined);
      result.push({
        range: new vscode.Range(start, end),
        flow: {
          source: region.text,
          name: symbol.name,
          path: document.uri.fsPath || document.uri.toString(),
          line: start.line,
          ports,
          tokens: tokenAnswer?.result?.tokens,
        },
      });
    }
  }
  return result;
}

/** Read current structured fields, accepting the previous additive envelope. */
function portFromSymbol(
  symbol: NonNullable<SymbolValue['children']>[number],
): FlowPort | undefined {
  if (symbol.direction) {
    return {
      name: symbol.name,
      direction: symbol.direction,
      type: symbol.type ?? 'application/octet-stream',
      stream: symbol.stream ?? false,
      required: symbol.required ?? false,
    };
  }
  const words = (symbol.detail ?? '').trim().split(/\s+/);
  const direction = words.shift();
  if (direction !== 'in' && direction !== 'out') return undefined;
  const required = words.at(-1) === 'required';
  if (required) words.pop();
  const stream = words.at(-1) === 'stream';
  if (stream) words.pop();
  return {
    name: symbol.name,
    direction,
    type: words.join(' ') || 'application/octet-stream',
    stream,
    required,
  };
}
