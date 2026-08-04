/**
 * Core model-interaction notions shared by every A11 backend.
 *
 * This is the TypeScript counterpart of `a11/sdk/llm.py`: the portable
 * {@link Interaction} message, the {@link LlmHeaders} that steer a backend, the
 * registry/header driven tool allow-list helpers, and the cross-backend content
 * normalization registry. Well-known shapes are defined with `zod` and every
 * fallible operation returns a {@link Status}/{@link StatusOr} instead of
 * throwing, matching the rest of the library.
 */

import { z } from 'zod';

import type { Action } from '../action.js';
import { utf8Decode } from '../bytes.js';
import {
  failedPreconditionError,
  invalidArgumentError,
  isOk,
  type NonOkStatus,
  type Status,
  type StatusOr,
} from '../status.js';

/** Framework headers that select and configure an LLM backend. */
export enum LlmHeaders {
  API_KEY = 'x-a11-llm-api-key',
  MODEL = 'x-a11-llm-model',
  PROVIDER = 'x-a11-llm-provider',
  BASE_URL = 'x-a11-llm-base-url',
  ALLOWED_LLM_ACTIONS = 'x-a11-allowed-llm-actions',
}

/** Conversation roles. `model` is the assistant role, matching the Python SDK. */
export enum Role {
  SYSTEM = 'system',
  ASSISTANT = 'model',
  USER = 'user',
}

/** Role assumed when an interaction does not specify one. */
export const DEFAULT_ROLE = Role.USER;

const roleSchema = z.enum(['system', 'model', 'user']);

/**
 * Run a zod schema against a value, returning a {@link StatusOr} rather than
 * throwing. Narrow the result with {@link isOk} to obtain the parsed value.
 */
export function zodParse<S extends z.ZodType>(
  schema: S,
  value: unknown,
  context: string,
): StatusOr<z.infer<S>> {
  const result = schema.safeParse(value);
  if (result.success) return result.data;
  return invalidArgumentError(
    `${context}: ${result.error.issues
      .map((issue) => `${issue.path.join('.') || '<root>'} ${issue.message}`)
      .join('; ')}`,
    [...result.error.issues],
  );
}

// --- Usage accounting --------------------------------------------------------

/** Provider-independent token accounting for a single interaction. */
export const usageMetadataSchema = z
  .object({
    input_tokens: z.number().int().nullish(),
    output_tokens: z.number().int().nullish(),
    total_tokens: z.number().int().nullish(),
    cached_input_tokens: z.number().int().nullish(),
    cache_write_tokens: z.number().int().nullish(),
    reasoning_tokens: z.number().int().nullish(),
  })
  .loose();
export type UsageMetadata = z.infer<typeof usageMetadataSchema>;

// --- Peers -------------------------------------------------------------------

/** Global WebRTC signalling endpoint used when an `rtc` peer omits one. */
export const GLOBAL_WEBRTC_SIGNALLING_ENDPOINT = 'wss://a11.services/ice';

export const a11PeerSchema = z.object({
  protocol: z.enum(['a11', 'mcp']).default('a11'),
  scheme: z
    .enum(['session', 'ws', 'wss', 'http', 'https', 'rtc'])
    .default('session'),
  identity: z.string().default(''),
  endpoint: z.string().default(''),
});
export type A11Peer = z.infer<typeof a11PeerSchema>;

/** Enforce the cross-field invariants the Python model validates. */
export function validateA11Peer(peer: A11Peer): StatusOr<A11Peer> {
  const result: A11Peer = { ...peer };
  if (result.protocol === 'mcp' && result.identity) {
    return invalidArgumentError('MCP peer identity must be empty.');
  }
  if (result.protocol === 'mcp' && !['http', 'https'].includes(result.scheme)) {
    return invalidArgumentError('MCP peer scheme must be http or https.');
  }
  if (result.protocol === 'a11' && result.scheme === 'session') {
    if (result.endpoint) {
      return invalidArgumentError(
        'A11 peer endpoint must be empty for `session` scheme.',
      );
    }
    if (!result.identity) result.identity = '$sender';
    if (
      result.identity.startsWith('$') &&
      !['$sender', '$receiver'].includes(result.identity)
    ) {
      return invalidArgumentError(
        'A11 peer identity must be `$sender`, `$receiver` or a stream ID.',
      );
    }
  }
  return result;
}

/** Render a peer back into its `protocol[+scheme]://identity[@endpoint]` URL. */
export function a11PeerToString(peer: A11Peer): string {
  const parts: string[] = [];
  if (peer.identity) parts.push(peer.identity);
  if (peer.endpoint) parts.push(peer.endpoint);
  let protocolScheme: string;
  if (peer.protocol === 'a11') {
    protocolScheme = peer.protocol;
    if (peer.scheme !== 'session') protocolScheme += `+${peer.scheme}`;
  } else {
    protocolScheme = peer.protocol;
  }
  return `${protocolScheme}://${parts.join('@')}`;
}

/** Parse a peer URL into a validated {@link A11Peer}. */
export function a11PeerFromString(peer: string): StatusOr<A11Peer> {
  const parts = peer.split('://');
  if (parts.length < 2) {
    return invalidArgumentError('Peer URL must include a scheme.');
  }
  const [protocolScheme, ...rest] = parts;
  const identityEndpoint = rest.join('://');
  const protocolSchemeParts = (protocolScheme ?? '').split('+');
  if (protocolSchemeParts.length > 2) {
    return invalidArgumentError(
      'Peer URL must include a single protocol and a single scheme.',
    );
  }
  let protocol = protocolSchemeParts[0] ?? '';
  if (!protocol) {
    if (protocolSchemeParts[1]) {
      return invalidArgumentError('Cannot include a scheme without protocol.');
    }
    protocol = 'a11';
  }
  if (protocol !== 'a11' && protocol !== 'mcp') {
    return invalidArgumentError(
      `Peer URL must include a valid protocol. Found: ${protocol}`,
    );
  }
  let scheme = protocolSchemeParts.length === 2 ? protocolSchemeParts[1] ?? '' : '';
  if (!scheme) scheme = protocol === 'mcp' ? 'http' : 'session';

  const identityEndpointParts = identityEndpoint.split('@');
  let identity = '';
  let endpoint = '';
  if (protocol === 'a11') {
    identity = identityEndpointParts[0] ?? '';
  } else {
    endpoint = identityEndpointParts[0] ?? '';
  }
  if (identityEndpointParts.length >= 2) {
    identity = identityEndpointParts[0] ?? '';
    endpoint = identityEndpointParts.slice(1).join('@');
  }
  if (!endpoint && protocol === 'a11' && scheme === 'rtc') {
    endpoint = GLOBAL_WEBRTC_SIGNALLING_ENDPOINT;
  }
  const parsed = zodParse(
    a11PeerSchema,
    { protocol, scheme, identity, endpoint },
    'A11Peer',
  );
  if (!isOk(parsed)) return parsed;
  return validateA11Peer(parsed);
}

// --- Action config -----------------------------------------------------------

export const a11ActionConfigSchema = z.object({
  peer: z.union([z.string(), a11PeerSchema]).default('a11://$sender'),
  header_autofills: z.record(z.string(), z.string()).default({}),
});
export type A11ActionConfig = z.infer<typeof a11ActionConfigSchema>;

// --- Interaction -------------------------------------------------------------

const interactionSchema = z
  .object({
    id: z.string().default(() => randomUuid()),
    role: roleSchema.default(DEFAULT_ROLE),
    created_at_millis: z.number().int().nullish(),
    previous_interaction_id: z.string().default(''),
    model: z.string().default(''),
    status: z.unknown().optional(),
    system_instructions: z.array(z.unknown()).default([]),
    action_configs: z.record(z.string(), z.unknown()).default({}),
    content: z.array(z.unknown()).default([]),
    action_calls: z.array(z.unknown()).default([]),
    action_inputs: z.record(z.string(), z.array(z.unknown())).default({}),
    action_outputs: z.record(z.string(), z.array(z.unknown())).default({}),
    backend_specific_metadata: z.record(z.string(), z.string()).default({}),
    usage_metadata: usageMetadataSchema.nullish(),
  })
  .loose();

/** Portable, backend-independent record of one turn in a conversation. */
export type Interaction = z.infer<typeof interactionSchema>;

/** Validate and default-fill an unknown value into an {@link Interaction}. */
export function parseInteraction(value: unknown): StatusOr<Interaction> {
  return zodParse(interactionSchema, value, 'Interaction');
}

/** Build a fully defaulted {@link Interaction} from a partial one. */
export function makeInteraction(
  partial: Partial<Interaction> = {},
): StatusOr<Interaction> {
  return parseInteraction(partial);
}

/**
 * Build an interaction carrying a single text message.
 *
 * The content is the backend-neutral `{role, content: [text part]}` envelope,
 * so a plain text turn stays portable across a mid-conversation model switch.
 */
export function makeTextMessageInteraction(
  text: string,
  systemPrompt = '',
  role: Role = Role.USER,
): StatusOr<Interaction> {
  if (role === Role.SYSTEM) {
    return invalidArgumentError(
      'A text message interaction cannot use the system role as content.',
    );
  }
  const roleStr = role === Role.ASSISTANT ? 'model' : 'user';
  return makeInteraction({
    role,
    content: [{ role: roleStr, content: [{ type: 'text', text }] }],
    system_instructions: systemPrompt ? [systemPrompt] : [],
  });
}

function randomUuid(): string {
  const generator = globalThis.crypto?.randomUUID;
  if (typeof generator === 'function') return generator.call(globalThis.crypto);
  // Deterministic-enough fallback for runtimes without WebCrypto.
  return `${Date.now().toString(16)}-${Math.floor(
    Math.random() * 0xffff_ffff,
  ).toString(16)}`;
}

// --- Tool allow-list ---------------------------------------------------------

/**
 * Regex patterns for the actions the LLM may invoke as tools.
 *
 * This is a tool-call-time restriction read from the
 * {@link LlmHeaders.ALLOWED_LLM_ACTIONS} header; it constrains which registered
 * actions are surfaced to (and callable by) the model.
 */
export function getAllowedLlmActionPatterns(action: Action): StatusOr<string[]> {
  const header = action.getHeader(LlmHeaders.ALLOWED_LLM_ACTIONS);
  if (!isOk(header)) return header;
  if (header === null) return [];
  const decoded = utf8Decode(header);
  if (!isOk(decoded)) {
    return invalidArgumentError(
      `The header ${LlmHeaders.ALLOWED_LLM_ACTIONS} is not a valid utf-8 string: ${decoded.message}`,
    );
  }
  return decoded
    .split(',')
    .map((pattern) => pattern.trim())
    .filter((pattern) => pattern.length > 0);
}

/** Whether an action name is fully matched by any allowed regex pattern. */
export function actionNameMatchesAllowed(
  name: string,
  patterns: readonly string[],
): StatusOr<boolean> {
  for (const pattern of patterns) {
    let regex: RegExp;
    try {
      regex = new RegExp(`^(?:${pattern})$`, 'u');
    } catch (error) {
      return invalidArgumentError(
        `Allowed LLM action pattern ${JSON.stringify(pattern)} is not a valid regular expression: ${
          error instanceof Error ? error.message : String(error)
        }`,
      );
    }
    if (regex.test(name)) return true;
  }
  return false;
}

// --- Cross-backend interaction normalization ---------------------------------

/** Metadata key under which an interaction records its producing backend. */
export const BACKEND_METADATA_KEY = 'backend';

/** Backends that can produce and normalize interactions. */
export enum Backend {
  CLAUDE = 'claude',
  GEMINI = 'gemini',
  OLLAMA = 'ollama',
  GEMMA = 'gemma',
}

export enum NormalizedContentType {
  TEXT = 'text',
  IMAGE = 'image',
  TOOL_CALL = 'tool_call',
  TOOL_RESULT = 'tool_result',
}

/** A single, backend-independent piece of an interaction's content. */
export interface NormalizedPart {
  type: NormalizedContentType;
  text?: string;
  data?: string;
  mime_type?: string;
  id?: string;
  name?: string;
  arguments?: Record<string, unknown>;
  call_id?: string;
  content?: string;
}

/** Backend-independent view of one interaction's content. */
export interface NormalizedMessage {
  role: Role;
  parts: NormalizedPart[];
}

export type InteractionNormalizer = (
  interaction: Interaction,
) => StatusOr<NormalizedMessage>;

const interactionNormalizers = new Map<string, InteractionNormalizer>();

/** Register a backend's native-content → {@link NormalizedMessage} producer. */
export function registerInteractionNormalizer(
  backend: string,
  normalizer: InteractionNormalizer,
): void {
  interactionNormalizers.set(String(backend), normalizer);
}

/** The backend that produced `interaction`, or `null` if untagged. */
export function interactionBackend(interaction: Interaction): string | null {
  const value = interaction.backend_specific_metadata?.[BACKEND_METADATA_KEY];
  return value ? String(value) : null;
}

/** Build the normalized view of a (foreign) interaction via its producer. */
export function normalizeInteraction(
  interaction: Interaction,
): StatusOr<NormalizedMessage> {
  const backend = interactionBackend(interaction);
  if (backend === null) {
    return invalidArgumentError(
      'Cannot normalize an interaction with no backend tag.',
    );
  }
  const normalizer = interactionNormalizers.get(backend);
  if (normalizer === undefined) {
    return failedPreconditionError(
      `No interaction normalizer is registered for backend ${JSON.stringify(
        backend,
      )}; its module must be imported to consume its interactions.`,
    );
  }
  return normalizer(interaction);
}

/** Re-export of the failure type for downstream backend modules. */
export type { NonOkStatus, Status };
