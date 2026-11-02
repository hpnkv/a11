import {type NodeFragment} from './data.js';
import {type StatusOr} from './status.js';
import {type GraphAction} from './runtime_graph.js';
import {type ActionRegistry} from './runtime_registry.js';
import {type Interaction} from './sdk/llm.js';
import {type PortValueSchemas} from './sdk/tool_adapter.js';
import {executeActionsFromInteraction as execute,
  getToolDefinitions as definitions,
  type ActionCall, type ToolDefinition} from './sdk/tool_runner.js';

/** Build model tool definitions from the C++ action registry. */
export function getToolDefinitions(registry: ActionRegistry | null,
    allowedActions: readonly string[] = [],
    portSchemas: Readonly<Record<string, PortValueSchemas>> = {}):
    Promise<StatusOr<ToolDefinition[]>> {
  return registry === null ? Promise.resolve([]) :
    definitions(registry, allowedActions, portSchemas);
}

/** Run tool calls as native nested actions. */
export function executeActionsFromInteraction(interaction: Interaction,
    action: GraphAction, registry: ActionRegistry | null = null,
    options: {timeoutMs?: number} = {}):
    Promise<StatusOr<Record<string, NodeFragment[]>>> {
  return execute(interaction, action, registry, options);
}

export type {ActionCall, ToolDefinition};
