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
import { base64Decode, utf8Decode } from '../bytes.js';
import { ActionMessage, Chunk, NodeFragment } from '../data.js';
import { toChunk } from '../serialization.js';
import {
  ACTION_CONFIG_TAG,
  ACTION_MESSAGE_TAG,
  CHUNK_TAG,
  INTERACTION_TAG,
  NODE_FRAGMENT_TAG,
  PEER_TAG,
  STATUS_TAG,
  USAGE_METADATA_TAG,
} from '../serial_tags.js';
import {
  registerWireValueCodec,
  tagValue,
  testTagged,
  valueTag,
  wireValueCodecByTag,
  type Fields,
} from '../wire_values.js';
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

/**
 * A field holding a registered model, without losing what it already is.
 *
 * When a serialized value arrives, the decoder has already rebuilt each nested
 * model and branded it with its tag. Validating that field again would produce
 * a *fresh* object and drop the brand, and the model would go back out to the
 * peer as an anonymous map instead of as its own type. So an already-branded
 * value passes through untouched, and only a value supplied by hand — by a
 * TypeScript caller writing an object literal — is parsed and branded.
 */
function taggedOr<S extends z.ZodType>(tag: string, schema: S) {
  return z.union([
    z.custom<z.infer<S>>((value) => valueTag(value) === tag),
    schema.transform((value) => tagValue({ ...(value as object) }, tag) as z.infer<S>),
  ]);
}

/**
 * A field holding one of the runtime's own classes — a `Chunk`, a `Status`.
 *
 * Nothing on the wire says which class this is; the field's type does, and this
 * is where that is written down. A value arrives as the bare field map its
 * sender dumped, and the codec registered for `tag` rebuilds it. An instance a
 * TypeScript caller supplied directly passes straight through.
 */
function wireValueField<T>(tag: string) {
  return z.unknown().transform((value, ctx): T => {
    const codec = wireValueCodecByTag(tag);
    if (codec === null) {
      ctx.addIssue({ code: 'custom', message: `No wire value codec for ${tag}.` });
      return z.NEVER;
    }
    if (codec.test(value)) return value as T;
    if (typeof value !== 'object' || value === null || Array.isArray(value)) {
      ctx.addIssue({ code: 'custom', message: `Expected the fields of a ${tag}.` });
      return z.NEVER;
    }
    const loaded: StatusOr<unknown> = codec.load(value as Fields);
    if (!isOk(loaded)) {
      ctx.addIssue({ code: 'custom', message: (loaded as NonOkStatus).message });
      return z.NEVER;
    }
    return loaded as T;
  });
}

/**
 * A `dict[str, bytes]` field.
 *
 * The values are bytes, so that is what a caller supplies. Untagged, the wire
 * spells them as base64 in JSON and as real bytes in MessagePack, and a string
 * is read as the former — text belongs in the field as its encoded bytes, via
 * `utf8Encode`, not as a string that would be indistinguishable from base64.
 */
const byteRecordSchema = z
  .record(
    z.string(),
    // `z.instanceof` narrows to `Uint8Array<ArrayBuffer>`, which rejects the
    // `Uint8Array<ArrayBufferLike>` that `utf8Encode` and friends return.
    z.union([
      z.custom<Uint8Array>((value) => value instanceof Uint8Array),
      z.string(),
    ]).transform((value, ctx) => {
      if (value instanceof Uint8Array) return value;
      const decoded = base64Decode(value);
      if (!isOk(decoded)) {
        ctx.addIssue({
          code: 'custom',
          message: 'A byte field takes bytes or the base64 the wire spells them as.',
        });
        return z.NEVER;
      }
      return decoded;
    }),
  )
  .default({});

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
  peer: z.union([z.string(), taggedOr(PEER_TAG, a11PeerSchema)]).default('a11://$sender'),
  header_autofills: byteRecordSchema,
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
    // These types are the only thing that says what the wire carries here, so
    // they mirror the Python model's annotations exactly. Loosening one back to
    // `z.unknown()` would leave its values as anonymous field maps.
    status: wireValueField<Status>(STATUS_TAG).optional(),
    system_instructions: z.array(wireValueField<Chunk>(CHUNK_TAG)).default([]),
    action_configs: z
      .record(z.string(), taggedOr(ACTION_CONFIG_TAG, a11ActionConfigSchema))
      .default({}),
    content: z.array(wireValueField<Chunk>(CHUNK_TAG)).default([]),
    action_calls: z.array(wireValueField<ActionMessage>(ACTION_MESSAGE_TAG)).default([]),
    action_inputs: z
      .record(z.string(), z.array(wireValueField<NodeFragment>(NODE_FRAGMENT_TAG)))
      .default({}),
    action_outputs: z
      .record(z.string(), z.array(wireValueField<NodeFragment>(NODE_FRAGMENT_TAG)))
      .default({}),
    backend_specific_metadata: byteRecordSchema,
    usage_metadata: taggedOr(USAGE_METADATA_TAG, usageMetadataSchema).nullish(),
  })
  .loose();

/** Portable, backend-independent record of one turn in a conversation. */
export type Interaction = z.infer<typeof interactionSchema>;

/**
 * What may be *supplied* for an interaction, before validation fills it in.
 *
 * Looser than {@link Interaction} itself: a byte field accepts base64, and a
 * `Chunk` field accepts either a Chunk or the bare fields a peer sent.
 */
export type InteractionInput = z.input<typeof interactionSchema>;

/**
 * Validate and default-fill an unknown value into an {@link Interaction}.
 *
 * The result is branded with its serialization tag, which is what lets it go
 * back out to a peer as an `Interaction` rather than an anonymous object — see
 * {@link tagValue}. Build interactions through here (or
 * {@link makeTextMessageInteraction}) rather than as object literals, or the
 * backend will reject them.
 */
export function parseInteraction(value: unknown): StatusOr<Interaction> {
  const parsed = zodParse(interactionSchema, value, 'Interaction');
  if (!isOk(parsed)) return parsed;
  return tagValue(parsed, INTERACTION_TAG);
}

/** Build a fully defaulted {@link Interaction} from a partial one. */
export function makeInteraction(
  partial: Partial<InteractionInput> = {},
): StatusOr<Interaction> {
  return parseInteraction(partial);
}

/**
 * Build an interaction carrying a single text message.
 *
 * The content is the backend-neutral `{role, content: [text part]}` envelope,
 * so a plain text turn stays portable across a mid-conversation model switch.
 *
 * An interaction's `content` and `system_instructions` are lists of
 * {@link Chunk}s, not of bare JSON — that is what every backend reads, and a
 * peer validating this one against its own model rejects anything else. Which
 * is why this is async: making a chunk means going through the serialization
 * registry, the same way `a11.to_chunk` does on the Python side.
 */
export async function makeTextMessageInteraction(
  text: string,
  systemPrompt = '',
  role: Role = Role.USER,
): Promise<StatusOr<Interaction>> {
  if (role === Role.SYSTEM) {
    return invalidArgumentError(
      'A text message interaction cannot use the system role as content.',
    );
  }
  const roleStr = role === Role.ASSISTANT ? 'model' : 'user';
  const content = await toChunk({ role: roleStr, content: [{ type: 'text', text }] });
  if (!isOk(content)) return content;
  const instructions: Chunk[] = [];
  if (systemPrompt) {
    const instruction = await toChunk(systemPrompt);
    if (!isOk(instruction)) return instruction;
    instructions.push(instruction);
  }
  return makeInteraction({
    role,
    content: [content],
    system_instructions: instructions,
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

// --- Serialization -----------------------------------------------------------
//
// The SDK's models are `zod` types, so at runtime they are plain objects with
// nothing to tell them apart from any other object a caller might send. Each is
// branded with its canonical tag when built or decoded, and registered here so
// it survives a round trip through a peer in another language: an interaction
// handed back to the backend has to arrive as `a11.sdk.Interaction`, not as an
// anonymous JSON blob the strict `interactions` port will refuse.
//
// `dump` is a shallow copy: the brand lives under a symbol, so a model's own
// enumerable fields are already exactly what goes on the wire — but handing the
// encoder the very object it is walking would read as a cycle and be refused.

function registerModelCodec<S extends z.ZodType>(
  tag: string,
  schema: S,
  context: string,
): void {
  registerWireValueCodec<z.infer<S>>({
    tag,
    test: testTagged(tag),
    dump: (value) => ({ ...(value as Fields) }),
    load: (fields) => {
      const parsed = zodParse(schema, fields, context);
      if (!isOk(parsed)) return parsed;
      return tagValue(parsed as object, tag) as z.infer<S>;
    },
  });
}

registerModelCodec(INTERACTION_TAG, interactionSchema, 'Interaction');
registerModelCodec(PEER_TAG, a11PeerSchema, 'A11Peer');
registerModelCodec(ACTION_CONFIG_TAG, a11ActionConfigSchema, 'A11ActionConfig');
registerModelCodec(USAGE_METADATA_TAG, usageMetadataSchema, 'UsageMetadata');

/** Brand a peer so it serializes as `a11.sdk.Peer`. */
export function asPeerValue(peer: A11Peer): A11Peer {
  return tagValue({ ...peer }, PEER_TAG);
}

/** Brand an action config so it serializes as `a11.sdk.ActionConfig`. */
export function asActionConfigValue(config: A11ActionConfig): A11ActionConfig {
  return tagValue({ ...config }, ACTION_CONFIG_TAG);
}

/** Brand usage metadata so it serializes as `a11.sdk.UsageMetadata`. */
export function asUsageMetadataValue(usage: UsageMetadata): UsageMetadata {
  return tagValue({ ...usage }, USAGE_METADATA_TAG);
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
  if (value === undefined) return null;
  const text = utf8Decode(value);
  return isOk(text) && text ? text : null;
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
