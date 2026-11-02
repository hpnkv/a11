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

import {currentFlowToolingService, flowToolBridge} from './internal/wasm/flow_bridge.js';
import {isOk, invalidArgumentError, StatusException,
  unimplementedError} from './status.js';
import {
  FLOW_KEYWORDS, FLOW_TYPES, FLOW_STAGES, FLOW_BUILTINS,
  FLOW_LOG_LEVELS, FLOW_CONSTANTS, FLOW_OPERATOR_WORDS,
} from './flow_vocabulary.js';

export {
  FLOW_KEYWORDS, FLOW_TYPES, FLOW_STAGES, FLOW_BUILTINS,
  FLOW_LOG_LEVELS, FLOW_CONSTANTS, FLOW_OPERATOR_WORDS,
};

/** Presentation categories derived from native Flow semantic tokens. */
export type FlowTokenKind =
  | 'comment' | 'string' | 'string-escape' | 'number' | 'keyword'
  | 'type' | 'stage' | 'builtin' | 'level' | 'constant'
  | 'operator' | 'punctuation' | 'plain';

/** One UTF-16 source span for editor colouring. */
export interface FlowToken {
  kind: FlowTokenKind;
  start: number;
  end: number;
  text: string;
}

function presentationKind(kind: string): FlowTokenKind {
  switch (kind) {
    case 'comment': return 'comment';
    case 'string': return 'string';
    case 'string-escape': return 'string-escape';
    case 'number':
    case 'duration': return 'number';
    case 'declaration-keyword':
    case 'statement-keyword':
    case 'modifier-keyword': return 'keyword';
    case 'builtin-type': return 'type';
    case 'type': return 'type';
    case 'stage': return 'stage';
    case 'builtin': return 'builtin';
    case 'level':
    case 'log-level': return 'level';
    case 'constant':
    case 'status-code': return 'constant';
    case 'flow-operator':
    case 'word-operator':
    case 'operator': return 'operator';
    case 'brace':
    case 'bracket':
    case 'parenthesis':
    case 'punctuation': return 'punctuation';
    default: return 'plain';
  }
}

/** Colour Flow source from the C++ token service after WASM initialization. */
export function tokenizeFlow(source: string): FlowToken[] {
  const service = currentFlowToolingService();
  if (service === null) {
    throw new StatusException(unimplementedError(
      'Initialize A11 WASM before tokenizing Flow source.'));
  }
  const answer = flowToolBridge(service, {method: 'tokens', source,
    offsets: 'utf16'});
  if (!isOk(answer)) throw new StatusException(answer);
  if (answer.ok !== true) {
    throw new StatusException(invalidArgumentError(
      String((answer.error as {message?: unknown} | undefined)?.message ??
        'Flow tokenization failed.')));
  }
  const result = answer.result as {tokens?: unknown} | undefined;
  if (!Array.isArray(result?.tokens)) {
    throw new StatusException(invalidArgumentError(
      'Native Flow token response is malformed.'));
  }
  const tokens: FlowToken[] = [];
  let offset = 0;
  for (const entry of result.tokens) {
    const token = entry as {kind?: unknown; start?: unknown; end?: unknown};
    if (typeof token.kind !== 'string' ||
        typeof token.start !== 'number' || typeof token.end !== 'number' ||
        token.start < offset || token.end < token.start ||
        token.end > source.length) {
      throw new StatusException(invalidArgumentError(
        'Native Flow token range is malformed.'));
    }
    if (offset < token.start) {
      tokens.push({kind: 'plain', start: offset, end: token.start,
        text: source.slice(offset, token.start)});
    }
    tokens.push({kind: presentationKind(token.kind),
      start: token.start, end: token.end,
      text: source.slice(token.start, token.end)});
    offset = token.end;
  }
  if (offset < source.length) {
    tokens.push({kind: 'plain', start: offset, end: source.length,
      text: source.slice(offset)});
  }
  return tokens;
}
