/**
 * Mirror the IDE tools that Kotlin exposes into a TypeScript `ActionRegistry`,
 * so the A11 `Session` can serve the gateway's reverse-dispatched tool calls.
 *
 * Each tool's schema is built dynamically from the Kotlin descriptor (no schema
 * is hard-coded here), and every handler delegates execution back to Kotlin via
 * the bridge — Kotlin remains the single source of truth. This mirrors the
 * Kotlin `IdeTools.buildRegistry()` handler shape (read the declared inputs, run
 * the tool, write its outputs back onto the declared output ports).
 *
 * One class of port never reaches the model: a descriptor may flag an output
 * `user_facing`, meaning it carries a run log for the person watching. Those
 * ports are written and announced like any other, and the gateway is what holds
 * them back — its bridge re-points a flagged port onto the canonical
 * `user_facing_log` name (`a11/gateway/tool_bridge.py`) and its LLM tool runner
 * drains that port, keeps it out of the model's tool result, and files it under
 * the tool call (`a11/sdk/llm_tools/runner.py`). Stripping there rather than here
 * is what lets the log be recorded with the conversation, so a replayed
 * transcript shows what a tool did and not merely that it ran. The values still
 * go to [ToolRunSink] as well, which is what the live UI renders — that path
 * needs no round trip.
 */

import {
  ActionPortSchema,
  ActionRegistry,
  ActionSchema,
  isOk,
  okStatus,
  statusFromUnknown,
  type Action,
  type Status,
  type ToolDefinition,
} from '@curiositystack/a11';

import { runAction, type ActionDescriptor, type PortDescriptor } from './bridge.js';

/** How long a handler waits for an input value the caller already sent. */
const READ_TIMEOUT_MS = 5_000;

/** Notified when a tool runs, with the run log it produced (if any). */
export type ToolRunSink = (run: { tool: string; log: string | null }) => void;

/** Whether a port is the user's to see rather than part of the model contract. */
const isUserFacing = (port: PortDescriptor): boolean => port.user_facing === true;

function toSchema(descriptor: ActionDescriptor): ActionSchema {
  const inputs: Record<string, ActionPortSchema> = {};
  for (const port of descriptor.inputs) {
    inputs[port.name] = new ActionPortSchema({
      name: port.name,
      type: port.type,
      required: port.required,
      unary: port.unary,
    });
  }
  const outputs: Record<string, ActionPortSchema> = {};
  for (const port of descriptor.outputs) {
    outputs[port.name] = new ActionPortSchema({
      name: port.name,
      type: port.type,
      required: port.required,
      unary: port.unary,
    });
  }
  const schema = new ActionSchema({
    name: descriptor.name,
    description: descriptor.description,
    inputs,
    outputs,
  });
  // Reproduce whatever output-to-JSON mapping Kotlin declared; which port (if
  // any) carries the whole result is the tool's choice, not a fixed name here.
  for (const [port, field] of Object.entries(descriptor.output_to_json_field ?? {})) {
    if (port in outputs) schema.mapOutputToJson(port, field);
  }
  return schema;
}

/**
 * Read the tool's declared inputs, keyed by port name: a unary port yields its
 * single value, a streaming port the list of values it carried. Absent and empty
 * ports are left out, so Kotlin sees exactly what the caller sent.
 */
async function readInputs(
  action: Action,
  descriptor: ActionDescriptor,
): Promise<Record<string, unknown>> {
  const inputs: Record<string, unknown> = {};
  // Read every port concurrently, for the same reason the outputs are written
  // concurrently: the caller fills them in its own order, not ours.
  await Promise.all(
    descriptor.inputs.map(async (port) => {
      if (!action.containsPort(port.name)) return;
      const node = await action.getInput(port.name);
      if (!isOk(node)) return;
      if (port.unary) {
        const value = await node.consume({ timeoutMs: READ_TIMEOUT_MS, allowNone: true });
        if (isOk(value) && value !== null) inputs[port.name] = value;
        return;
      }
      const values: unknown[] = [];
      for (;;) {
        const next = await node.next({ timeoutMs: READ_TIMEOUT_MS });
        if (!isOk(next) || next === null) break;
        values.push(next);
      }
      if (values.length > 0) inputs[port.name] = values;
    }),
  );
  return inputs;
}

/**
 * Write Kotlin's outputs onto the ports the descriptor declares: a unary port
 * takes its single value, a streaming port one value per item with the last
 * marked final, and a port the tool had nothing for is simply closed.
 *
 * Nothing is ever written as an explicit null. A null chunk is a *value* on the
 * wire, and a consumer that decodes an action's outputs — the LLM tool runner,
 * for one — has no type to decode it as; closing an empty port says "nothing
 * here" without putting an undecodable value in front of anyone.
 *
 * A user-facing port is the exception, and has to be: closing a port that never
 * carried a fragment leaves the *remote* reader with no end of stream, and the
 * backend proxy reads that port live rather than after the call completes (it
 * has to — an unread port stalls the writer). So an empty run log is terminated
 * explicitly. Its only reader is that proxy, which skips null chunks; the model
 * never sees this port at all.
 */
async function writeOutputs(
  action: Action,
  descriptor: ActionDescriptor,
  outputs: Record<string, unknown>,
): Promise<Status> {
  // One writer per port, all at once: the reader on the other side drains the
  // ports in an order this side cannot know, and the transport pushes back when
  // a port fills up. Filling one port to completion before starting the next
  // would wedge both peers as soon as a result is large enough to hit that
  // backpressure.
  const written = await Promise.all(
    descriptor.outputs.map(async (port): Promise<Status> => {
      const node = await action.getOutput(port.name);
      if (!isOk(node)) return node;
      const value = outputs[port.name];
      const values =
        value === undefined || value === null ? [] : port.unary ? [value] : Array.isArray(value) ? value : [value];
      for (const [index, item] of values.entries()) {
        const put = await node.put(item, { final: index === values.length - 1 });
        if (!isOk(put)) return put;
      }
      if (values.length === 0 && isUserFacing(port)) {
        const end = await node.putNullFinal();
        if (!isOk(end)) return end;
      }
      return node.drainAndClose();
    }),
  );
  return written.find((status) => !isOk(status)) ?? okStatus();
}

function handlerFor(descriptor: ActionDescriptor, onRun?: ToolRunSink) {
  const name = descriptor.name;
  const userFacing = descriptor.outputs.filter(isUserFacing);
  return async (action: Action): Promise<Status> => {
    const inputs = await readInputs(action, descriptor);
    let outputs: unknown;
    try {
      outputs = await runAction(name, inputs);
    } catch (error) {
      onRun?.({ tool: name, log: `Failed: ${error instanceof Error ? error.message : String(error)}` });
      return statusFromUnknown(error, `IDE tool '${name}' failed.`);
    }
    if (typeof outputs !== 'object' || outputs === null) {
      return statusFromUnknown(
        new Error(`IDE tool '${name}' returned no outputs.`),
        `IDE tool '${name}' failed.`,
      );
    }
    const produced = outputs as Record<string, unknown>;
    // The log goes straight to the UI, which is why the live transcript shows it
    // without waiting for a round trip. It is *also* written to its port, where
    // the backend picks it up for the conversation record and keeps it away from
    // the model; see the note at the top of this file.
    const log = userFacing.map((port) => produced[port.name]).find((value) => typeof value === 'string');
    onRun?.({ tool: name, log: typeof log === 'string' ? log : null });
    return writeOutputs(action, descriptor, produced);
  };
}

/**
 * Build an `ActionRegistry` populated with every IDE tool, plus the ordered
 * list of tool descriptors (for the `__register_tools__` announcement).
 */
export function buildIdeToolRegistry(
  descriptors: ActionDescriptor[],
  onRun?: ToolRunSink,
): { registry: ActionRegistry; toolNames: string[] } {
  const registry = new ActionRegistry();
  const toolNames: string[] = [];
  for (const descriptor of descriptors) {
    const status = registry.register(descriptor.name, toSchema(descriptor), handlerFor(descriptor, onRun));
    if (!isOk(status)) throw new Error(`Registering IDE tool '${descriptor.name}': ${status.message}`);
    toolNames.push(descriptor.name);
  }
  return { registry, toolNames };
}

/**
 * Overlay each descriptor's per-port JSON Schema onto the model's tool
 * definitions.
 *
 * A JS `ActionPortSchema` carries only a MIME type, so `ToolAdapter` describes a
 * `request` port as a bare `{"type": "object"}` and the model never learns its
 * fields. Kotlin ships the request DTO's real JSON Schema in the descriptor
 * (`IdeTools.port`), so splice it in here — the Kotlin session path gets the
 * same schemas straight from `ActionPortSchema.jsonSchema`.
 */
export function applyPortSchemas(
  definitions: ToolDefinition[],
  descriptors: ActionDescriptor[],
): ToolDefinition[] {
  const byName = new Map(descriptors.map((d) => [d.name, d]));
  for (const definition of definitions) {
    const properties = definition.input_schema.properties as Record<string, unknown> | undefined;
    if (!properties) continue;
    for (const port of byName.get(definition.name)?.inputs ?? []) {
      if (port.schema && port.name in properties) properties[port.name] = port.schema;
    }
  }
  return definitions;
}
