/**
 * Provider-agnostic entry point that routes an interaction to a concrete
 * backend. Ported from `a11/sdk/interact_with_llm.py`.
 *
 * The handler inspects the {@link LlmHeaders.PROVIDER} header (falling back to
 * the model id's family prefix) and runs the matching backend inline on the
 * same action, so a backend's port reads and writes are the same nodes. In this
 * TypeScript build only the in-browser `gemma` backend ships a handler; the
 * other providers are recognized for routing but return a
 * `FAILED_PRECONDITION` status pointing at the Python package, since no
 * TypeScript handler exists for them.
 */

import type { Action, ActionHandler } from '../action.js';
import {
  ActionHeaderSchema,
  ActionPortSchema,
  ActionSchema,
} from '../action_schema.js';
import { utf8Decode } from '../bytes.js';
import {
  failedPreconditionError,
  invalidArgumentError,
  isOk,
  statusFromUnknown,
  type Status,
  type StatusOr,
} from '../status.js';
import { LlmHeaders } from './llm.js';
import { interactWithGemma } from './gemma/interact_with_gemma.js';

interface Provider {
  /** The handler to run, or `null` when this build ships none for it. */
  handler: ActionHandler | null;
  /** Extra hint returned when no TypeScript handler exists. */
  note: string;
}

const PROVIDERS: Readonly<Record<string, Provider>> = {
  gemma: { handler: interactWithGemma as ActionHandler, note: '' },
  claude: {
    handler: null,
    note: 'The claude backend is only available in the Python package (a11-kit[claude]).',
  },
  gemini: {
    handler: null,
    note: 'The gemini backend is only available in the Python package (a11-kit[gemini]).',
  },
  ollama: {
    handler: null,
    note: 'The ollama backend is only available in the Python package (a11-kit[ollama]).',
  },
  vllm: {
    handler: null,
    note: 'The vllm backend is only available in the Python package (a11-kit[vllm]).',
  },
};

// Fallbacks used when no explicit provider header is set: the model id's prefix
// usually names its family. `gemma` routes to the in-browser backend here.
const MODEL_PREFIXES: ReadonlyArray<readonly [string, string]> = [
  ['gemma', 'gemma'],
  ['claude', 'claude'],
  ['gemini', 'gemini'],
  ['llama', 'ollama'],
  ['qwen', 'ollama'],
  ['mistral', 'ollama'],
  ['phi', 'ollama'],
  ['deepseek', 'ollama'],
];

/** The union action contract routed by {@link interactWithLlm}. */
export const INTERACT_WITH_LLM_SCHEMA = new ActionSchema({
  name: 'interact_with_llm',
  description: `Route an LLM interaction to a concrete backend chosen by the ${LlmHeaders.PROVIDER} header.`,
  inputs: {
    interactions: new ActionPortSchema({
      name: 'interactions',
      type: 'application/json',
      required: true,
    }),
    tools: new ActionPortSchema({ name: 'tools', type: 'application/json', required: false }),
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
    [LlmHeaders.API_KEY]: new ActionHeaderSchema({
      name: LlmHeaders.API_KEY,
      description: 'The backend API key.',
    }),
    [LlmHeaders.PROVIDER]: new ActionHeaderSchema({
      name: LlmHeaders.PROVIDER,
      description: `Which backend to route to, one of: ${Object.keys(PROVIDERS).join(', ')}.`,
    }),
    [LlmHeaders.MODEL]: new ActionHeaderSchema({
      name: LlmHeaders.MODEL,
      description: 'The downstream model.',
    }),
    [LlmHeaders.BASE_URL]: new ActionHeaderSchema({
      name: LlmHeaders.BASE_URL,
      description: 'The downstream base URL, where applicable.',
    }),
    [LlmHeaders.ALLOWED_LLM_ACTIONS]: new ActionHeaderSchema({
      name: LlmHeaders.ALLOWED_LLM_ACTIONS,
      description: 'The allowed action (tool) name patterns, comma-separated.',
    }),
  },
});

function headerString(action: Action, name: string): StatusOr<string | null> {
  const value = action.getHeader(name);
  if (!isOk(value)) return value;
  if (value === null) return null;
  return utf8Decode(value);
}

/** Pick the backend from the provider header, or infer it from the model. */
function resolveProvider(action: Action): StatusOr<string> {
  const provider = headerString(action, LlmHeaders.PROVIDER);
  if (!isOk(provider)) return provider;
  if (provider !== null && provider.trim() !== '') {
    const normalized = provider.trim().toLowerCase();
    if (!(normalized in PROVIDERS)) {
      return invalidArgumentError(
        `Unknown LLM provider ${JSON.stringify(normalized)}; expected one of ${Object.keys(
          PROVIDERS,
        ).join(', ')}.`,
      );
    }
    return normalized;
  }
  const model = headerString(action, LlmHeaders.MODEL);
  if (!isOk(model)) return model;
  const modelName = (model ?? '').trim().toLowerCase();
  for (const [prefix, name] of MODEL_PREFIXES) {
    if (modelName.startsWith(prefix)) return name;
  }
  return invalidArgumentError(
    `No ${LlmHeaders.PROVIDER} header was set and the provider could not be inferred from the model ${JSON.stringify(
      modelName,
    )}. Set one of: ${Object.keys(PROVIDERS).join(', ')}.`,
  );
}

/** Route the interaction to the header-selected backend and run it inline. */
export async function interactWithLlm(action: Action): Promise<Status> {
  try {
    const provider = resolveProvider(action);
    if (!isOk(provider)) return provider;
    const entry = PROVIDERS[provider]!;
    if (entry.handler === null) {
      return failedPreconditionError(entry.note);
    }
    const result = await entry.handler(action);
    return result ?? { code: 0, message: 'OK' };
  } catch (error) {
    return statusFromUnknown(error, 'Routing the LLM interaction raised an exception.');
  }
}
