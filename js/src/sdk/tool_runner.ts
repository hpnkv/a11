/**
 * Turn an interaction's tool calls into nested action runs. Ported from
 * `a11/sdk/llm_tools/runner.py`.
 *
 * {@link getToolDefinitions} surfaces the allow-listed registry actions to a
 * model as JSON-Schema tools; {@link executeActionsFromInteraction} runs the
 * actions a model asked for and collects their outputs. Both return
 * {@link StatusOr} and never throw.
 */

import type { Action, ActionRegistryLike } from '../action.js';
import { NodeFragment } from '../data.js';
import {
  failedPreconditionError,
  invalidArgumentError,
  isOk,
  notFoundError,
  permissionDeniedError,
  statusFromUnknown,
  type StatusOr,
} from '../status.js';
import { WHOLE_JSON_OUTPUT } from '../action_schema.js';
import {
  actionNameMatchesAllowed,
  getAllowedLlmActionPatterns,
  type Interaction,
} from './llm.js';
import { ToolAdapter, type PortValueSchemas } from './tool_adapter.js';

/** One tool definition surfaced to a model. */
export interface ToolDefinition {
  name: string;
  description: string;
  input_schema: Record<string, unknown>;
}

/** Minimal shape of an interaction's `action_calls` entries. */
export interface ActionCall {
  name: string;
  id: string;
}

/** Build model tool definitions for the allow-listed registry actions. */
export function getToolDefinitions(
  registry: ActionRegistryLike | null,
  allowedActions: readonly string[] = [],
  portSchemas: Readonly<Record<string, PortValueSchemas>> = {},
): StatusOr<ToolDefinition[]> {
  if (registry === null) return [];
  const definitions: ToolDefinition[] = [];
  for (const name of allowedActions) {
    const schema = registry.getSchema(name);
    if (!isOk(schema)) {
      // A missing action is skipped; any other failure is propagated, matching
      // the Python NOT_FOUND handling.
      if (schema.code === notFoundError().code) continue;
      return schema;
    }
    const adapter = new ToolAdapter(schema, portSchemas[schema.name] ?? {});
    const inputSchema = adapter.getInputSchema();
    if (!isOk(inputSchema)) return inputSchema;
    definitions.push({
      name: schema.name,
      description: schema.description,
      input_schema: inputSchema,
    });
  }
  return definitions;
}

function readActionCalls(interaction: Interaction): StatusOr<ActionCall[]> {
  const calls: ActionCall[] = [];
  for (const call of interaction.action_calls ?? []) {
    if (!call.name || !call.id) {
      return invalidArgumentError('Each action call needs a name and id.');
    }
    calls.push({ name: call.name, id: call.id });
  }
  return calls;
}

/**
 * Run every allow-listed action call in `interaction` as a nested action and
 * return the produced output fragments, keyed by the nested action's call id.
 */
export async function executeActionsFromInteraction(
  interaction: Interaction,
  action: Action,
  registry: ActionRegistryLike | null = null,
  options: { timeoutMs?: number } = {},
): Promise<StatusOr<Record<string, NodeFragment[]>>> {
  try {
    const effectiveRegistry = registry ?? action.getRegistry();
    if (effectiveRegistry === null) {
      return failedPreconditionError(
        'Cannot execute actions against an empty registry.',
      );
    }
    const allowedPatterns = getAllowedLlmActionPatterns(action);
    if (!isOk(allowedPatterns)) return allowedPatterns;

    const calls = readActionCalls(interaction);
    if (!isOk(calls)) return calls;

    const nestedActions: Array<{ call: ActionCall; action: Action }> = [];
    for (const call of calls) {
      const allowed = actionNameMatchesAllowed(call.name, allowedPatterns);
      if (!isOk(allowed)) return allowed;
      if (!allowed) {
        return permissionDeniedError(`Action ${call.name} is not allowed`);
      }
      const nested = action.makeNested(call.name);
      if (!isOk(nested)) return nested;
      const idSet = nested.setId(call.id);
      if (!isOk(idSet)) return idSet;
      const started = nested.run();
      if (!isOk(started)) return started;

      const inputFragments = (interaction.action_inputs?.[call.id] ?? []) as unknown[];
      for (const fragment of inputFragments) {
        if (!(fragment instanceof NodeFragment)) {
          return invalidArgumentError(
            'Action input fragments must be NodeFragment instances.',
          );
        }
        const port = await nested.getInput(fragment.id);
        if (!isOk(port)) return port;
        const stored = await port.putFragment(fragment);
        if (!isOk(stored)) return stored;
      }

      // Autofilled inputs are written and closed by the native run flow, so the
      // runner only closes the inputs it fed itself. Closed, not finalized: the
      // forwarded fragments carry whatever finality the model's arguments had.
      for (const [inputName, portSchema] of nested.getSchema().inputs) {
        if (portSchema.autofills.length > 0) continue;
        const port = await nested.getInput(inputName);
        if (!isOk(port)) return port;
        const closed = await port.close();
        if (!isOk(closed)) return closed;
      }
      nestedActions.push({ call, action: nested });
    }

    for (const { action: nested } of nestedActions) {
      const waited = await nested.wait(options.timeoutMs);
      if (!isOk(waited)) return waited;
    }

    const allOutputs: Record<string, NodeFragment[]> = {};
    for (const { action: nested } of nestedActions) {
      const gathered = await gatherActionOutputs(nested);
      if (!isOk(gathered)) return gathered;
      allOutputs[nested.getId()] = gathered;
    }
    return allOutputs;
  } catch (error) {
    return statusFromUnknown(
      error,
      'Executing actions from interaction raised an exception.',
    );
  }
}

async function gatherActionOutputs(nested: Action): Promise<StatusOr<NodeFragment[]>> {
  const schema = nested.getSchema();
  const perOutput: Record<string, NodeFragment[]> = {};
  for (const outputName of schema.outputs.keys()) {
    const node = await nested.getOutput(outputName, false);
    if (!isOk(node)) return node;
    const fragments: NodeFragment[] = [];
    while (true) {
      const fragment = await node.nextFragment();
      if (!isOk(fragment)) return fragment;
      if (fragment === null) break;
      fragment.id = outputName;
      fragment.continued = true;
      fragments.push(fragment);
    }
    const last = fragments[fragments.length - 1];
    if (last !== undefined) last.continued = false;
    perOutput[outputName] = fragments;
  }

  const substitutions = schema.outputToJsonField;
  const mappedNames = [...substitutions.keys()];
  const collected: NodeFragment[] = [];
  if (substitutions.size === 1 && substitutions.get(mappedNames[0]!) === WHOLE_JSON_OUTPUT) {
    for (const fragment of perOutput[mappedNames[0]!] ?? []) {
      fragment.id = '_';
      collected.push(fragment);
    }
    return collected;
  }

  for (const [outputName, fragments] of Object.entries(perOutput)) {
    let mapTo: string | undefined = outputName;
    if (substitutions.size > 0) mapTo = substitutions.get(outputName);
    if (mapTo === undefined) continue;
    for (const fragment of fragments) {
      fragment.id = mapTo;
      collected.push(fragment);
    }
  }
  return collected;
}
