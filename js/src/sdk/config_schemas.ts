/**
 * `zod` schemas for the value carried on each backend's `config` action port.
 *
 * These mirror the pydantic config models of the backends that ship no
 * TypeScript handler (`a11/sdk/{gemini,ollama,vllm,anthropic}/*_schema.py`). They
 * are provided so a TypeScript caller can validate, document, and type the
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
  INTERACT_WITH_CLAUDE_CODE_CONFIG_TAG,
  INTERACT_WITH_CLAUDE_CONFIG_TAG,
  INTERACT_WITH_GEMINI_CONFIG_TAG,
  INTERACT_WITH_OLLAMA_CONFIG_TAG,
  INTERACT_WITH_VLLM_CONFIG_TAG,
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

/** Parameters for a single vLLM chat completion (OpenAI-compatible route). */
export const vllmCreateChatCompletionConfigSchema = z.object({
  max_tokens: z
    .number()
    .int()
    .default(-1)
    .describe(
      'Maximum number of tokens to generate. -1 lets the model run until it' +
        " stops on its own or reaches the deployment's context limit.",
    ),
  temperature: z.number().nullish().describe('Sampling temperature.'),
  top_p: z.number().nullish().describe('Nucleus-sampling probability.'),
  presence_penalty: z
    .number()
    .nullish()
    .describe('Penalty applied to tokens that already appeared.'),
  frequency_penalty: z
    .number()
    .nullish()
    .describe('Penalty scaled by how often a token already appeared.'),
  seed: z.number().int().nullish().describe('Sampling seed for reproducible output.'),
  stop: z
    .array(z.string())
    .default([])
    .describe('Strings that end the generation when produced.'),
  top_k: z
    .number()
    .int()
    .nullish()
    .describe('Top-k sampling cutoff (vLLM `extra_body.top_k`).'),
  min_p: z
    .number()
    .nullish()
    .describe(
      'Minimum token probability, relative to the most likely token (vLLM' +
        ' `extra_body.min_p`).',
    ),
  repetition_penalty: z
    .number()
    .nullish()
    .describe(
      'Penalty applied to tokens from the prompt and the output so far (vLLM' +
        ' `extra_body.repetition_penalty`).',
    ),
  json_output: z
    .boolean()
    .default(false)
    .describe('Constrain the model to emit valid JSON (`response_format`).'),
  json_schema: z
    .record(z.string(), z.unknown())
    .nullish()
    .describe(
      "A JSON Schema the output has to satisfy, enforced by vLLM's structured" +
        ' decoding. Takes precedence over `json_output`.',
    ),
  chat_template_kwargs: z
    .record(z.string(), z.unknown())
    .default({})
    .describe(
      "Values passed to the model's chat template, such as" +
        ' `{"enable_thinking": true}` on models whose template gates reasoning' +
        ' (vLLM `extra_body.chat_template_kwargs`).',
    ),
  extra_body: z
    .record(z.string(), z.unknown())
    .default({})
    .describe(
      'Additional request fields, merged into the request body last. Covers' +
        ' deployment-specific sampling parameters this config does not name.',
    ),
});
export type VllmCreateChatCompletionConfig = z.infer<
  typeof vllmCreateChatCompletionConfigSchema
>;

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

/** The Claude Code tools a session may enable. */
export const claudeCodeBuiltinToolSchema = z.enum([
  'Bash',
  'Edit',
  'Glob',
  'Grep',
  'NotebookEdit',
  'Read',
  'Task',
  'TodoWrite',
  'WebFetch',
  'WebSearch',
  'Write',
]);
export type ClaudeCodeBuiltinTool = z.infer<typeof claudeCodeBuiltinToolSchema>;

/** Parameters for one Claude Code session. */
export const claudeCodeCreateSessionConfigSchema = z.object({
  builtin_tools: z
    .union([z.boolean(), z.array(claudeCodeBuiltinToolSchema)])
    .default(false)
    .describe(
      "Claude Code's own tools. `false` offers the model A11 actions alone;" +
        ' `true` offers the full Claude Code toolset, which reads and writes' +
        ' the filesystem and runs commands; a list offers the named subset.' +
        ' Enabling a tool also permits it — a session driven through A11' +
        ' answers no permission prompt — so name only what the turn should be' +
        ' able to do, and use `disallowed_tools` to carve back a command' +
        ' shape.',
    ),
  permission_mode: z
    .enum(['default', 'acceptEdits', 'plan', 'dontAsk', 'bypassPermissions'])
    .nullish()
    .describe(
      "Claude Code's permission mode, such as `plan` for a read-only session" +
        ' or `acceptEdits` for unattended file edits.',
    ),
  disallowed_tools: z
    .array(z.string())
    .default([])
    .describe(
      'Tool names or scoped rules the model may never use, such as' +
        ' `Bash(rm *)`. A scoped rule is refused in every permission mode.',
    ),
  max_turns: z
    .number()
    .int()
    .nullish()
    .describe('Maximum agent turns before the session stops.'),
  max_budget_usd: z
    .number()
    .nullish()
    .describe('Stop the session once the estimated cost reaches this.'),
  cwd: z.string().nullish().describe("Working directory for the session's tools."),
  add_dirs: z
    .array(z.string())
    .default([])
    .describe("Extra directories the session's tools may reach."),
  setting_sources: z
    .array(z.enum(['user', 'project', 'local']))
    .nullish()
    .describe(
      'Which on-disk Claude Code settings to load. Omitted loads none, which' +
        " keeps a session's behaviour independent of the host's" +
        ' configuration; include `project` to load CLAUDE.md.',
    ),
  skills: z
    .union([z.array(z.string()), z.literal('all')])
    .nullish()
    .describe('Skills to make available, or `all`.'),
  thinking: z
    .boolean()
    .default(false)
    .describe(
      'Enable adaptive thinking so the model decides when and how much' +
        ' internal reasoning to spend.',
    ),
  thinking_summaries: z
    .boolean()
    .default(false)
    .describe("Stream summaries of the model's reasoning as it thinks."),
  effort: z
    .enum(['low', 'medium', 'high', 'xhigh', 'max'])
    .nullish()
    .describe('Overall thinking depth and token spend.'),
  fallback_model: z
    .string()
    .nullish()
    .describe('Model to fall back to when the primary is unavailable.'),
  resume: z
    .string()
    .nullish()
    .describe(
      'Claude Code session id to continue. Also read from the newest' +
        " assistant interaction's metadata when unset.",
    ),
  fork_session: z
    .boolean()
    .default(false)
    .describe('Branch a resumed session instead of extending it.'),
  cli_path: z.string().nullish().describe('Path to the `claude` executable.'),
});
export type ClaudeCodeCreateSessionConfig = z.infer<
  typeof claudeCodeCreateSessionConfigSchema
>;

// --- Serialization -----------------------------------------------------------

function registerConfigCodec<S extends z.ZodType>(
  tag: string,
  schema: S,
  context: string,
): (value?: unknown) => StatusOr<z.infer<S>> {
  registerWireValueCodec<z.infer<S>>({
    tag,
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

/** Validate and tag a vLLM request config for the `config` port. */
export const makeVllmCreateChatCompletionConfig = registerConfigCodec(
  INTERACT_WITH_VLLM_CONFIG_TAG,
  vllmCreateChatCompletionConfigSchema,
  'VllmCreateChatCompletionConfig',
);

/** Validate and tag a Claude request config for the `config` port. */
export const makeClaudeCreateMessageConfig = registerConfigCodec(
  INTERACT_WITH_CLAUDE_CONFIG_TAG,
  claudeCreateMessageConfigSchema,
  'ClaudeCreateMessageConfig',
);

/** Validate and tag a Claude Code session config for the `config` port. */
export const makeClaudeCodeCreateSessionConfig = registerConfigCodec(
  INTERACT_WITH_CLAUDE_CODE_CONFIG_TAG,
  claudeCodeCreateSessionConfigSchema,
  'ClaudeCodeCreateSessionConfig',
);

/** Default model ids matching the Python SDK. */
export const GEMINI_DEFAULT_MODEL = 'gemini-3.5-flash';
export const OLLAMA_DEFAULT_MODEL = 'llama3.2';
// A vLLM deployment serves the models it was started with; the Python handler
// asks it for the first of them when no model is named.
export const VLLM_DEFAULT_MODEL = '';
export const CLAUDE_DEFAULT_MODEL = 'claude-sonnet-4-6';
