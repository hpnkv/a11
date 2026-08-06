/**
 * `interact_with_gemma`: run a Gemma-family model entirely in the browser.
 *
 * This backend has no server: it loads a Gemma model into the page over WebGPU
 * and streams generated tokens straight onto the action's `text_output` port,
 * so a client-side agent can hold a conversation with no network round-trip.
 * The runtime is pluggable through {@link setGemmaEngineFactory}; the default
 * factory dynamically imports Google's MediaPipe `LlmInference` task from a
 * config-supplied URL (kept out of the bundle by using a runtime specifier).
 *
 * Like every A11 handler here it never throws: foreign engine calls are wrapped
 * and mapped to a {@link Status}, and a failure is returned so the runtime
 * aborts the output ports with it.
 */

import { z } from 'zod';

import type { Action } from '../../action.js';
import {
  ActionHeaderSchema,
  ActionPortSchema,
  ActionSchema,
} from '../../action_schema.js';
import type { AsyncNode } from '../../async_node.js';
import { utf8Decode } from '../../bytes.js';
import { Chunk } from '../../data.js';
import { JSON_MIMETYPE } from '../../serialization.js';
import {
  invalidArgumentError,
  isOk,
  noexceptFetch,
  okStatus,
  statusFromResponse,
  statusFromUnknown,
  unavailableError,
  type Status,
  type StatusOr,
} from '../../status.js';
import {
  BACKEND_METADATA_KEY,
  Backend,
  LlmHeaders,
  NormalizedContentType,
  Role,
  makeInteraction,
  parseInteraction,
  registerInteractionNormalizer,
  zodParse,
  type Interaction,
  type NormalizedMessage,
  type NormalizedPart,
} from '../llm.js';

/** Default model label recorded on produced interactions. */
export const DEFAULT_MODEL = 'gemma-4-E2B-it';

/** Default Gemma model asset served by the LiteRT community on HuggingFace. */
export const DEFAULT_MODEL_ASSET_PATH =
  'https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/gemma-4-E2B-it-web.litertlm?download=true';

/**
 * Sequences that end a Gemma turn. Generation is truncated at the first of
 * these so the model does not leak turn markers or run on to fabricate the
 * user's next turn.
 */
export const DEFAULT_STOP_SEQUENCES = ['<end_of_turn>', '<start_of_turn>', '<eos>'];

/** Parameters for a browser Gemma run, carried on the unary `config` port. */
export const gemmaConfigSchema = z.object({
  model_asset_path: z
    .string()
    .default(DEFAULT_MODEL_ASSET_PATH)
    .describe(
      'URL of the Gemma model asset (`.task`/`.litertlm`) to download and' +
        ' run on WebGPU. HTTP redirects are followed when fetching it.',
    ),
  max_tokens: z
    .number()
    .int()
    .default(1024)
    .describe('Maximum number of tokens to generate for a turn.'),
  temperature: z
    .number()
    .nullish()
    .describe('Sampling temperature; higher is more random.'),
  top_k: z
    .number()
    .int()
    .nullish()
    .describe('Top-k sampling cutoff.'),
  random_seed: z
    .number()
    .int()
    .nullish()
    .describe('Sampling seed for reproducible output.'),
  stop_sequences: z
    .array(z.string())
    .default(DEFAULT_STOP_SEQUENCES)
    .describe(
      'Text sequences that end generation. Output is truncated at the first' +
        ' occurrence so turn markers do not leak and the model does not' +
        " continue past its own turn.",
    ),
  runtime_url: z
    .string()
    .default('https://cdn.jsdelivr.net/npm/@mediapipe/tasks-genai')
    .describe('ES module URL of the MediaPipe GenAI tasks runtime.'),
  wasm_base: z
    .string()
    .default('https://cdn.jsdelivr.net/npm/@mediapipe/tasks-genai/wasm')
    .describe('Base URL of the MediaPipe GenAI WebAssembly fileset.'),
});
export type GemmaConfig = z.infer<typeof gemmaConfigSchema>;

/** A loaded, streaming Gemma runtime. */
export interface GemmaEngine {
  /**
   * Generate a completion for `prompt`, delivering each incremental piece of
   * text through `onToken`. Resolves with the full generated text or a Status.
   */
  generate(
    prompt: string,
    onToken: (delta: string) => void,
    signal?: AbortSignal,
  ): Promise<StatusOr<string>>;
  /** Release the underlying model, if the runtime supports it. */
  close?(): void;
}

/** Builds a {@link GemmaEngine} for a given configuration. */
export type GemmaEngineFactory = (
  config: GemmaConfig,
  signal?: AbortSignal,
) => Promise<StatusOr<GemmaEngine>>;

const CONFIG_READ_TIMEOUT_MS = 20_000;

/** Cache Storage bucket holding downloaded Gemma model assets. */
export const MODEL_CACHE_NAME = 'a11-gemma-models';

/** Open the persistent model cache, or `null` where Cache Storage is absent. */
async function openModelCache(): Promise<Cache | null> {
  try {
    if (typeof caches === 'undefined') return null;
    return await caches.open(MODEL_CACHE_NAME);
  } catch {
    // Cache Storage needs a secure context; without it, run uncached.
    return null;
  }
}

/**
 * Download a model asset, following HTTP redirects (HuggingFace `resolve` URLs
 * 302 to a CDN), and return its bytes. Never throws.
 *
 * The bytes are persisted in the {@link MODEL_CACHE_NAME} Cache Storage bucket
 * keyed by `url`, so a page reload serves the (large) model from disk instead
 * of re-downloading it. Caching is best-effort: a private-mode/quota failure
 * still returns the freshly downloaded bytes.
 */
export async function fetchModelAssetBuffer(url: string): Promise<StatusOr<Uint8Array>> {
  const cache = await openModelCache();
  if (cache !== null) {
    try {
      const hit = await cache.match(url);
      if (hit !== undefined) return new Uint8Array(await hit.arrayBuffer());
    } catch {
      // A cache read failure just falls through to a network fetch.
    }
  }

  const response = await noexceptFetch(url, { redirect: 'follow' });
  if (!isOk(response)) return response;
  if (!response.ok) {
    const status = await statusFromResponse(response, 'Downloading the Gemma model');
    return isOk(status)
      ? unavailableError('Downloading the Gemma model failed.')
      : status;
  }

  let buffer: ArrayBuffer;
  try {
    buffer = await response.arrayBuffer();
  } catch (error) {
    return statusFromUnknown(error, 'Reading the downloaded Gemma model failed.');
  }

  if (cache !== null) {
    try {
      // A redirected Response cannot be cached directly, so store a fresh copy
      // keyed by the requested URL.
      await cache.put(
        url,
        new Response(buffer, { headers: { 'content-type': 'application/octet-stream' } }),
      );
    } catch {
      // Caching is best-effort (quota, private mode, …); the bytes are returned regardless.
    }
  }
  return new Uint8Array(buffer);
}

// --- The default MediaPipe-backed engine -------------------------------------

async function defaultGemmaEngineFactory(
  config: GemmaConfig,
): Promise<StatusOr<GemmaEngine>> {
  if (!config.model_asset_path) {
    return invalidArgumentError(
      'GemmaConfig.model_asset_path is required to load a browser model.',
    );
  }
  if (typeof globalThis.navigator === 'undefined' || !('gpu' in globalThis.navigator)) {
    return unavailableError(
      'WebGPU is not available in this environment; a Gemma model cannot run.',
    );
  }
  let llm: { generateResponse: unknown; close?: () => void };
  try {
    // A runtime (non-literal) specifier keeps esbuild from bundling MediaPipe;
    // the browser loads it from the configured CDN at call time.
    const runtimeUrl = config.runtime_url;
    const mediapipe = (await import(/* @vite-ignore */ runtimeUrl)) as {
      FilesetResolver: { forGenAiTasks(base: string): Promise<unknown> };
      LlmInference: {
        createFromOptions(fileset: unknown, options: unknown): Promise<{
          generateResponse: unknown;
          close?: () => void;
        }>;
      };
    };
    const fileset = await mediapipe.FilesetResolver.forGenAiTasks(config.wasm_base);
    // Fetch the model ourselves so HTTP redirects (e.g. HuggingFace `resolve`
    // URLs) are followed, then hand MediaPipe the bytes.
    const modelBuffer = await fetchModelAssetBuffer(config.model_asset_path);
    if (!isOk(modelBuffer)) return modelBuffer;
    const options: Record<string, unknown> = {
      baseOptions: { modelAssetBuffer: modelBuffer },
      maxTokens: config.max_tokens,
    };
    if (config.top_k !== undefined && config.top_k !== null) options.topK = config.top_k;
    if (config.temperature !== undefined && config.temperature !== null) {
      options.temperature = config.temperature;
    }
    if (config.random_seed !== undefined && config.random_seed !== null) {
      options.randomSeed = config.random_seed;
    }
    llm = await mediapipe.LlmInference.createFromOptions(fileset, options);
  } catch (error) {
    return statusFromUnknown(
      error,
      'Loading the MediaPipe Gemma runtime failed.',
      unavailableError().code,
    );
  }

  const engine: GemmaEngine = {
    generate(prompt, onToken) {
      return new Promise<StatusOr<string>>((resolve) => {
        try {
          let full = '';
          const generateResponse = llm.generateResponse as (
            input: string,
            progress: (partial: string, done: boolean) => void,
          ) => void;
          generateResponse(prompt, (partial: string, done: boolean) => {
            if (partial) {
              full += partial;
              try {
                onToken(partial);
              } catch {
                // A UI sink failure must not abort generation.
              }
            }
            if (done) resolve(full);
          });
        } catch (error) {
          resolve(statusFromUnknown(error, 'Gemma generation failed.'));
        }
      });
    },
    close() {
      try {
        llm.close?.();
      } catch {
        // Best-effort teardown.
      }
    },
  };
  return engine;
}

let gemmaEngineFactory: GemmaEngineFactory = defaultGemmaEngineFactory;

/** Replace the runtime used by {@link interactWithGemma} (e.g. for tests). */
export function setGemmaEngineFactory(factory: GemmaEngineFactory): void {
  gemmaEngineFactory = factory;
}

/** Restore the default MediaPipe-backed engine factory. */
export function resetGemmaEngineFactory(): void {
  gemmaEngineFactory = defaultGemmaEngineFactory;
}

// --- Schema ------------------------------------------------------------------

/** The action contract for a browser Gemma interaction. */
export const INTERACT_WITH_GEMMA_SCHEMA = new ActionSchema({
  name: 'interact_with_gemma',
  description:
    'Run a Gemma-family model in the browser over WebGPU, streaming its output.',
  inputs: {
    interactions: new ActionPortSchema({
      name: 'interactions',
      type: 'application/json',
      required: true,
    }),
    tools: new ActionPortSchema({
      name: 'tools',
      type: 'application/json',
      required: false,
    }),
    config: new ActionPortSchema({
      name: 'config',
      type: 'application/json',
      unary: true,
      required: false,
    }),
  },
  outputs: {
    event_stream: new ActionPortSchema({
      name: 'event_stream',
      type: 'application/json',
      required: false,
    }),
    thoughts: new ActionPortSchema({ name: 'thoughts', type: 'text/plain', required: false }),
    text_output: new ActionPortSchema({
      name: 'text_output',
      type: 'text/plain',
      required: false,
    }),
    new_interactions: new ActionPortSchema({
      name: 'new_interactions',
      type: 'application/json',
      required: true,
    }),
  },
  headers: {
    [LlmHeaders.MODEL]: new ActionHeaderSchema({
      name: LlmHeaders.MODEL,
      description: 'Label recorded for the local model.',
    }),
    [LlmHeaders.ALLOWED_LLM_ACTIONS]: new ActionHeaderSchema({
      name: LlmHeaders.ALLOWED_LLM_ACTIONS,
      description: 'The allowed LLM action (tool) name patterns, comma-separated.',
    }),
  },
});

// --- Stop-sequence streaming -------------------------------------------------

const CONTROL_TOKEN_PATTERN = /<\|?(?:start_of_turn|end_of_turn|eos|bos|pad)\|?>/g;

/** Strip stray Gemma control tokens and surrounding whitespace. */
function sanitizeGemmaText(text: string): string {
  return text.replace(CONTROL_TOKEN_PATTERN, '').trim();
}

/**
 * Filters a token stream so no text past a stop sequence is ever emitted.
 *
 * It holds back the trailing few characters that could be the start of a stop
 * sequence, so a marker split across two deltas is never surfaced and then
 * retracted. Once a stop sequence is seen the filter latches shut.
 */
class GemmaStopFilter {
  private raw = '';
  private emitted = 0;
  private stopped = false;
  private readonly maxStopLength: number;

  constructor(private readonly stops: readonly string[]) {
    this.maxStopLength = stops.reduce((max, stop) => Math.max(max, stop.length), 1);
  }

  /** Feed one streamed piece; return the text that is now safe to show. */
  push(delta: string): string {
    if (this.stopped || !delta) return '';
    this.raw += delta;
    let cut = -1;
    for (const stop of this.stops) {
      const index = this.raw.indexOf(stop);
      if (index >= 0 && (cut === -1 || index < cut)) cut = index;
    }
    if (cut >= 0) {
      this.stopped = true;
      const out = this.raw.slice(this.emitted, cut);
      this.emitted = cut;
      return out;
    }
    // Withhold a possible partial stop sequence until more text confirms it.
    const safe = Math.max(this.emitted, this.raw.length - (this.maxStopLength - 1));
    const out = this.raw.slice(this.emitted, safe);
    this.emitted = safe;
    return out;
  }

  /** Flush the held-back tail when generation ends without a stop sequence. */
  finish(): string {
    if (this.stopped) return '';
    const out = this.raw.slice(this.emitted);
    this.emitted = this.raw.length;
    return out;
  }
}

// --- Content helpers ---------------------------------------------------------

/**
 * The application value inside one `content` / `system_instructions` entry.
 *
 * An interaction carries these as {@link Chunk}s. Decoded here rather than
 * through the serialization registry because the callers below are synchronous
 * — one of them implements the sync {@link InteractionNormalizer} contract —
 * and the only thing they want is the text a message envelope holds, which is
 * always written as JSON or plain text. Anything else reads as absent, and a
 * bare value (as older callers wrote) passes through as it stands.
 */
function entryValue(item: unknown): unknown {
  if (!(item instanceof Chunk)) return item;
  const text = utf8Decode(item.data);
  if (!isOk(text)) return null;
  const mediaType = item.mimetype.split(';')[0]?.trim() ?? '';
  if (mediaType.startsWith('text/')) return text;
  if (mediaType !== '' && mediaType !== JSON_MIMETYPE) return null;
  try {
    return JSON.parse(text) as unknown;
  } catch {
    return text;
  }
}

/** Extract the plain text of one interaction's `{role, content:[…]}` envelope. */
function extractInteractionText(interaction: Interaction): string {
  const pieces: string[] = [];
  for (const entry of interaction.content ?? []) {
    const item = entryValue(entry);
    if (typeof item === 'string') {
      pieces.push(item);
      continue;
    }
    if (typeof item !== 'object' || item === null) continue;
    const envelope = item as Record<string, unknown>;
    const inner = envelope.content;
    if (typeof inner === 'string') {
      pieces.push(inner);
    } else if (Array.isArray(inner)) {
      for (const part of inner) {
        if (typeof part === 'object' && part !== null) {
          const partObject = part as Record<string, unknown>;
          if (partObject.type === 'text' && typeof partObject.text === 'string') {
            pieces.push(partObject.text);
          }
        } else if (typeof part === 'string') {
          pieces.push(part);
        }
      }
    } else if (typeof envelope.text === 'string') {
      pieces.push(envelope.text);
    }
  }
  return pieces.join('');
}

function systemInstructionsText(interactions: readonly Interaction[]): string {
  const lines: string[] = [];
  for (const interaction of interactions) {
    for (const entry of interaction.system_instructions ?? []) {
      const instruction = entryValue(entry);
      if (typeof instruction === 'string') lines.push(instruction);
      else if (
        typeof instruction === 'object' &&
        instruction !== null &&
        typeof (instruction as Record<string, unknown>).text === 'string'
      ) {
        lines.push((instruction as Record<string, unknown>).text as string);
      }
    }
  }
  return lines.join('\n\n');
}

/** Assemble the Gemma chat-template prompt from the conversation so far. */
function buildGemmaPrompt(interactions: readonly Interaction[]): string {
  const system = systemInstructionsText(interactions);
  const turns: string[] = [];
  let first = true;
  for (const interaction of interactions) {
    const text = extractInteractionText(interaction);
    if (!text) continue;
    const role = interaction.role === Role.ASSISTANT ? 'model' : 'user';
    let body = text;
    if (first && role === 'user' && system) {
      body = `${system}\n\n${text}`;
      first = false;
    }
    turns.push(`<start_of_turn>${role}\n${body}<end_of_turn>\n`);
  }
  return `${turns.join('')}<start_of_turn>model\n`;
}

// --- Normalizer --------------------------------------------------------------

function gemmaToNormalized(interaction: Interaction): StatusOr<NormalizedMessage> {
  const text = extractInteractionText(interaction);
  const parts: NormalizedPart[] = [];
  if (text) parts.push({ type: NormalizedContentType.TEXT, text });
  const role = interaction.role === Role.ASSISTANT ? Role.ASSISTANT : Role.USER;
  return { role, parts };
}

registerInteractionNormalizer(Backend.GEMMA, gemmaToNormalized);

// --- Handler -----------------------------------------------------------------

async function closeStream(node: AsyncNode): Promise<void> {
  await node.putNullFinal();
  await node.drainAndClose();
}

/** Run a browser Gemma interaction, streaming its reply. Never throws. */
export async function interactWithGemma(action: Action): Promise<Status> {
  const eventNodeResult = await action.getOutput('event_stream', false);
  const thoughtsNodeResult = await action.getOutput('thoughts', false);
  const textNodeResult = await action.getOutput('text_output', false);
  const newInteractionsResult = await action.getOutput('new_interactions', false);
  for (const result of [
    eventNodeResult,
    thoughtsNodeResult,
    textNodeResult,
    newInteractionsResult,
  ]) {
    if (!isOk(result)) return result;
  }
  const eventNode = eventNodeResult as AsyncNode;
  const thoughtsNode = thoughtsNodeResult as AsyncNode;
  const textNode = textNodeResult as AsyncNode;
  const newInteractionsNode = newInteractionsResult as AsyncNode;

  try {
    // Model label header, if any.
    const modelHeader = action.getHeader(LlmHeaders.MODEL);
    if (!isOk(modelHeader)) return modelHeader;
    let model = DEFAULT_MODEL;
    if (modelHeader !== null) {
      const decoded = utf8Decode(modelHeader);
      if (!isOk(decoded)) return decoded;
      if (decoded) model = decoded;
    }

    // Configuration (unary, optional): missing or timed-out reads use defaults.
    const configNode = await action.getInput('config');
    if (!isOk(configNode)) return configNode;
    const rawConfig = await configNode.consume({
      allowNone: true,
      timeoutMs: CONFIG_READ_TIMEOUT_MS,
    });
    let configValue: unknown = null;
    if (isOk(rawConfig)) configValue = rawConfig;
    const config = zodParse(gemmaConfigSchema, configValue ?? {}, 'GemmaConfig');
    if (!isOk(config)) return config;

    // Read the whole conversation off the required `interactions` port.
    const interactionsNode = await action.getInput('interactions');
    if (!isOk(interactionsNode)) return interactionsNode;
    const interactions: Interaction[] = [];
    let previousInteractionId = '';
    while (true) {
      const value = await interactionsNode.next();
      if (!isOk(value)) return value;
      if (value === null) break;
      const parsed = parseInteraction(value);
      if (!isOk(parsed)) return parsed;
      interactions.push(parsed);
      previousInteractionId = parsed.id;
    }
    if (interactions.length === 0) {
      return invalidArgumentError('At least one interaction is required.');
    }

    // Load the runtime and stream a reply.
    const engine = await gemmaEngineFactory(config, action.signal);
    if (!isOk(engine)) return engine;

    const prompt = buildGemmaPrompt(interactions);
    let writeChain: Promise<StatusOr<number>> = Promise.resolve(0);
    let writeError: Status | null = null;
    const chain = (work: () => Promise<StatusOr<number>>): void => {
      writeChain = writeChain.then(async (previous) => {
        if (writeError !== null) return previous;
        const result = await work();
        if (!isOk(result)) writeError = result;
        return result;
      });
    };
    // Route raw model tokens through the stop filter so turn markers never
    // reach the output and generation is treated as ending at the first one.
    const stopFilter = new GemmaStopFilter(config.stop_sequences);
    let visible = '';
    const show = (piece: string): void => {
      if (!piece) return;
      visible += piece;
      chain(() => textNode.put(piece));
      chain(() => eventNode.put({ type: 'token', text: piece }));
    };
    const emit = (delta: string): void => show(stopFilter.push(delta));

    const generated = await engine.generate(prompt, emit, action.signal);
    if (isOk(generated)) show(stopFilter.finish());
    await writeChain;
    try {
      engine.close?.();
    } catch {
      // Best-effort teardown.
    }
    if (!isOk(generated)) return generated;
    if (writeError !== null) return writeError;

    // Persist the assistant turn as a Gemma-tagged interaction. Store the same
    // cleaned text the reader saw, with any stray control tokens removed.
    const replyText = sanitizeGemmaText(visible);
    const assistant = makeInteraction({
      role: Role.ASSISTANT,
      model,
      created_at_millis: Date.now(),
      previous_interaction_id: previousInteractionId,
      content: [{ role: 'model', content: [{ type: 'text', text: replyText }] }],
      backend_specific_metadata: { [BACKEND_METADATA_KEY]: Backend.GEMMA },
    });
    if (!isOk(assistant)) return assistant;
    const put = await newInteractionsNode.put(assistant, { final: true });
    if (!isOk(put)) return put;

    // Close every output cleanly so a reader's stream terminates.
    await eventNode.put({ type: 'done' });
    await eventNode.drainAndClose();
    await closeStream(thoughtsNode);
    await textNode.drainAndClose();
    await newInteractionsNode.drainAndClose();
    return okStatus();
  } catch (error) {
    return statusFromUnknown(error, 'interact_with_gemma raised an exception.');
  }
}
