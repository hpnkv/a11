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

export * from './sdk/interact_with_llm.js';
export * from './sdk/gemma/interact_with_gemma.js';

import {type GraphAction} from './runtime_graph.js';
import {type Status} from './status.js';
import {interactWithLlm as route} from './sdk/interact_with_llm.js';
import {interactWithGemma as gemma} from './sdk/gemma/interact_with_gemma.js';

/** Route a native action to its configured model provider. */
export function interactWithLlm(action: GraphAction,
    context: {signal?: AbortSignal} = {}): Promise<Status> {
  return route(action, context);
}

/** Run a browser Gemma model as a native action. */
export function interactWithGemma(action: GraphAction,
    context: {signal?: AbortSignal} = {}): Promise<Status> {
  return gemma(action, context);
}
