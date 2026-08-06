/**
 * `zod` schemas for the value carried on each backend's `config` action port.
 *
 * These mirror the pydantic config models of the backends that ship no
 * TypeScript handler (`a11/sdk/{gemini,ollama,anthropic}/*_schema.py`). They are
 * provided so a TypeScript caller can validate, document, and type the
 * `config` port when routing an interaction to those providers through a
 * remote A11 session; the browser-only {@link GemmaConfig} lives in the gemma
 * module.
 *
 * Validate a value into one of these types with the matching `make*` function
 * below rather than with {@link zodParse} directly: the result is branded with
 * the provider's canonical serialization tag, and the strict, unary `config`
 * port on the Python side accepts nothing else — an untagged object is a plain
 * `object` on the wire, and the backend rejects it rather than guessing which
 * provider's config it was meant to be.
 */

import { z } from 'zod';

import {
  INTERACT_WITH_CLAUDE_CONFIG_TAG,
  INTERACT_WITH_GEMINI_CONFIG_TAG,
  INTERACT_WITH_OLLAMA_CONFIG_TAG,
} from '../serial_tags.js';
import { isOk, type StatusOr } from '../status.js';
import { registerWireValueCodec, tagValue, testTagged, type Fields } from '../wire_values.js';
import { zodParse } from './llm.js';

/** How the Gemini handler carries conversation state across turns. */
export const geminiStateModeSchema = z.enum(['full-history', 'last-id', 'auto']);
export type GeminiStateMode = z.infer<typeof geminiStateModeSchema>;

/** Parameters for starting a Gemini interaction (`interactions.create`). */
export const geminiCreateInteractionConfigSchema = z.object({
  max_output_tokens: z
    .number()
    .int()
    .default(10240)
    .describe('Maximum number of tokens to generate per step.'),
  state_mode: geminiStateModeSchema
    .default('auto')
    .describe(
      'How to carry conversation state across turns: resume by' +
        ' `previous_interaction_id` (`last-id`), replay the whole transcript' +
        ' every turn (`full-history`), or try the former and fall back to the' +
        ' latter (`auto`).',
    ),
  thinking_level: z
    .enum(['minimal', 'low', 'medium', 'high'])
    .nullish()
    .describe('How much internal reasoning the model may spend.'),
  thinking_summaries: z
    .boolean()
    .default(false)
    .describe("Stream summaries of the model's reasoning as it thinks."),
  google_search: z
    .boolean()
    .default(false)
    .describe('Enable the built-in Google Search grounding tool.'),
  code_execution: z
    .boolean()
    .default(false)
    .describe('Enable the built-in code execution tool.'),
  url_context: z
    .boolean()
    .default(false)
    .describe('Enable the built-in URL context tool.'),
});
export type GeminiCreateInteractionConfig = z.infer<
  typeof geminiCreateInteractionConfigSchema
>;

/** Parameters for a single Ollama chat turn (stateless `chat` API). */
export const ollamaCreateChatConfigSchema = z.object({
  num_predict: z
    .number()
    .int()
    .default(-1)
    .describe(
      'Maximum number of tokens to generate. -1 lets the model run until it' +
        ' stops on its own (Ollama `options.num_predict`).',
    ),
  think: z
    .union([z.boolean(), z.enum(['low', 'medium', 'high'])])
    .nullish()
    .describe(
      "Enable the model's thinking, optionally at a given effort level. Only" +
        ' honoured by models that support it.',
    ),
  temperature: z
    .number()
    .nullish()
    .describe('Sampling temperature (Ollama `options.temperature`).'),
  top_p: z
    .number()
    .nullish()
    .describe('Nucleus-sampling probability (Ollama `options.top_p`).'),
  top_k: z
    .number()
    .int()
    .nullish()
    .describe('Top-k sampling cutoff (Ollama `options.top_k`).'),
  seed: z
    .number()
    .int()
    .nullish()
    .describe('Sampling seed for reproducible output (Ollama `options.seed`).'),
  keep_alive: z
    .union([z.string(), z.number()])
    .nullish()
    .describe(
      'How long to keep the model loaded in memory after the request (e.g.' +
        ' `5m`, or seconds as a number).',
    ),
  json_output: z
    .boolean()
    .default(false)
    .describe('Constrain the model to emit valid JSON (Ollama `format="json"`).'),
});
export type OllamaCreateChatConfig = z.infer<typeof ollamaCreateChatConfigSchema>;

/** Parameters for creating a Claude message (`messages.create`). */
export const claudeCreateMessageConfigSchema = z.object({
  max_tokens: z
    .number()
    .int()
    .default(10240)
    .describe('Maximum number of tokens to generate.'),
  thinking: z
    .boolean()
    .default(false)
    .describe(
      'Enable adaptive thinking so the model decides when and how much' +
        ' internal reasoning to spend. Unsupported alongside tools and on' +
        ' Haiku models.',
    ),
  thinking_summaries: z
    .boolean()
    .default(false)
    .describe("Stream summaries of the model's reasoning as it thinks."),
  effort: z
    .enum(['low', 'medium', 'high', 'xhigh', 'max'])
    .nullish()
    .describe(
      'Overall thinking depth and token spend. Only honoured on models that' +
        ' support the effort parameter.',
    ),
  web_search: z
    .boolean()
    .default(false)
    .describe('Enable the built-in web search tool.'),
  web_fetch: z
    .boolean()
    .default(false)
    .describe('Enable the built-in web fetch tool.'),
  code_execution: z
    .boolean()
    .default(false)
    .describe('Enable the built-in code execution tool.'),
});
export type ClaudeCreateMessageConfig = z.infer<
  typeof claudeCreateMessageConfigSchema
>;

// --- Serialization -----------------------------------------------------------

function registerConfigCodec<S extends z.ZodType>(
  tag: string,
  schema: S,
  context: string,
): (value?: unknown) => StatusOr<z.infer<S>> {
  registerWireValueCodec<z.infer<S>>({
    tag,
    kind: 'pydantic',
    test: testTagged(tag),
    // A shallow copy: handing the encoder the object it is already walking
    // would read as a cycle. See the note in `llm.ts`.
    dump: (value) => ({ ...(value as Fields) }),
    load: (fields) => make(fields),
  });

  function make(value: unknown = {}): StatusOr<z.infer<S>> {
    const parsed = zodParse(schema, value, context);
    if (!isOk(parsed)) return parsed;
    return tagValue(parsed as object, tag) as z.infer<S>;
  }
  return make;
}

/** Validate and tag a Gemini request config for the `config` port. */
export const makeGeminiCreateInteractionConfig = registerConfigCodec(
  INTERACT_WITH_GEMINI_CONFIG_TAG,
  geminiCreateInteractionConfigSchema,
  'GeminiCreateInteractionConfig',
);

/** Validate and tag an Ollama request config for the `config` port. */
export const makeOllamaCreateChatConfig = registerConfigCodec(
  INTERACT_WITH_OLLAMA_CONFIG_TAG,
  ollamaCreateChatConfigSchema,
  'OllamaCreateChatConfig',
);

/** Validate and tag a Claude request config for the `config` port. */
export const makeClaudeCreateMessageConfig = registerConfigCodec(
  INTERACT_WITH_CLAUDE_CONFIG_TAG,
  claudeCreateMessageConfigSchema,
  'ClaudeCreateMessageConfig',
);

/** Default model ids matching the Python SDK. */
export const GEMINI_DEFAULT_MODEL = 'gemini-3.5-flash';
export const OLLAMA_DEFAULT_MODEL = 'llama3.2';
export const CLAUDE_DEFAULT_MODEL = 'claude-sonnet-4-6';
