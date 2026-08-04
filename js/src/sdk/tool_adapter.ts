/**
 * Translate an {@link ActionSchema} into the JSON-Schema tool contract an LLM
 * consumes. Ported from `a11/sdk/llm_tools/adapter.py`.
 *
 * A JavaScript {@link ActionPortSchema} carries a MIME `type` rather than a
 * Python `typeinfo`, so a port's value shape is derived from that MIME type by
 * default; callers with a richer contract may pass a per-port `zod` schema.
 * Non-unary ports become arrays and autofilled inputs are hidden from the model,
 * exactly as in the Python adapter.
 */

import { z } from 'zod';

import { WHOLE_JSON_OUTPUT, type ActionPortSchema, type ActionSchema } from '../action_schema.js';
import { isOk, type StatusOr } from '../status.js';
import { organiseAndDeduplicateJsonschema, zodToJsonSchema } from './jsonschema.js';

/** Optional per-port value schemas, keyed by port name. */
export type PortValueSchemas = Readonly<Record<string, z.ZodType>>;

function mimeToJsonSchema(mimetype: string): Record<string, unknown> {
  const mediaType = mimetype.split(';')[0]?.trim().toLowerCase() ?? '';
  if (mediaType.startsWith('text/')) return { type: 'string' };
  return { type: 'object' };
}

/** Builds the input/output JSON Schemas that describe an action as a tool. */
export class ToolAdapter {
  constructor(
    private readonly schema: ActionSchema,
    private readonly portSchemas: PortValueSchemas = {},
  ) {}

  /** JSON Schema for the tool's callable inputs (autofilled inputs excluded). */
  getInputSchema(): StatusOr<Record<string, unknown>> {
    const properties: Record<string, unknown> = {};
    const requiredNodes: string[] = [];
    for (const port of this.schema.inputs.values()) {
      // Autofilled inputs are supplied automatically before the handler runs,
      // so the LLM must never see them in the tool definition.
      if (port.autofills.length > 0) continue;
      const nodeSchema = this.nodeSchema(port);
      if (!isOk(nodeSchema)) return nodeSchema;
      if (port.required) requiredNodes.push(port.name);
      properties[port.name] = this.wrapCollection(nodeSchema, port, port.required);
    }
    return organiseAndDeduplicateJsonschema({
      type: 'object',
      properties,
      required: requiredNodes,
    });
  }

  /** JSON Schema for the tool's result, honoring `output_to_json_field`. */
  getOutputSchema(): StatusOr<Record<string, unknown>> {
    const properties: Record<string, unknown> = {};
    const requiredNodes: string[] = [];
    for (const port of this.schema.outputs.values()) {
      const nodeSchema = this.nodeSchema(port);
      if (!isOk(nodeSchema)) return nodeSchema;
      if (port.required) requiredNodes.push(port.name);
      // Output arrays are always constrained to at least one item.
      properties[port.name] = this.wrapCollection(nodeSchema, port, true);
    }

    const substitutions = this.schema.outputToJsonField;
    let schema: Record<string, unknown>;
    if (substitutions.size === 0) {
      schema = { type: 'object', properties, required: requiredNodes };
    } else if (
      substitutions.size === 1 &&
      [...substitutions.values()][0] === WHOLE_JSON_OUTPUT
    ) {
      const only = [...substitutions.keys()][0]!;
      schema = properties[only] as Record<string, unknown>;
    } else {
      for (const [name, substitution] of substitutions) {
        if (name in properties) {
          properties[substitution] = properties[name];
          delete properties[name];
        }
      }
      schema = { type: 'object', properties, required: requiredNodes };
    }
    return organiseAndDeduplicateJsonschema(schema);
  }

  private nodeSchema(port: ActionPortSchema): StatusOr<Record<string, unknown>> {
    const provided = this.portSchemas[port.name];
    if (provided !== undefined) return zodToJsonSchema(provided);
    return mimeToJsonSchema(port.type);
  }

  private wrapCollection(
    nodeSchema: Record<string, unknown>,
    port: ActionPortSchema,
    minOne: boolean,
  ): Record<string, unknown> {
    if (port.unary) return nodeSchema;
    const wrapped: Record<string, unknown> = { type: 'array', items: nodeSchema };
    if (minOne) wrapped.minItems = 1;
    return wrapped;
  }
}
