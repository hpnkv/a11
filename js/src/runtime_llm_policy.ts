import {type GraphAction} from './runtime_graph.js';
import {type StatusOr} from './status.js';
import {getAllowedLlmActionPatterns as getPatterns} from './sdk/llm.js';

/** Read model tool permissions from a native action's headers. */
export function getAllowedLlmActionPatterns(action: GraphAction):
    Promise<StatusOr<string[]>> {
  return getPatterns(action);
}
