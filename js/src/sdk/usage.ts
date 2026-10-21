import {utf8Decode, utf8Encode} from '../bytes.js';
import {isOk} from '../status.js';
import {usageMetadataSchema, type Interaction, type UsageMetadata} from './llm.js';

export interface UsageFigure {
  value?: number | null;
  quality: 'reported' | 'partial' | 'unknown';
}

export interface UsageScope {
  session_id?: string | null;
  turn_id?: string | null;
  request_id?: string | null;
  agent_id?: string | null;
  parent_agent_id?: string | null;
  epoch?: number;
  reporting?: 'snapshot' | 'cumulative' | 'delta';
}

export interface UsageUpdate {
  type: 'a11.usage';
  version: 1;
  id: string;
  revision: number;
  usage: UsageMetadata;
  contribution?: 'consumption' | 'reconciliation';
  provider?: string;
  model?: string;
  scope?: UsageScope;
  figures?: Record<string, UsageFigure>;
  source_usage?: Record<string, unknown> | null;
  source_id?: string | null;
  operation?: string;
  coverage?: string;
}

/** Read normalized usage while preserving the original event envelope. */
export function usageUpdate(event: unknown): UsageUpdate | undefined {
  if (!event || typeof event !== 'object') return undefined;
  const value = event as Record<string, unknown>;
  if (value.type !== 'a11.usage' || value.version !== 1 || typeof value.id !== 'string' || typeof value.revision !== 'number') return undefined;
  const parsed = usageMetadataSchema.safeParse(value.usage);
  if (!Number.isInteger(value.revision) || value.revision < 1 || !parsed.success) return undefined;
  if (parsed.data.usage_id !== value.id || parsed.data.usage_revision !== value.revision) return undefined;
  for (const field of ['input_tokens', 'cached_input_tokens', 'cache_write_tokens', 'output_tokens', 'reasoning_tokens', 'total_tokens'] as const) {
    const count = parsed.data[field];
    if (count != null && (!Number.isInteger(count) || count < 0)) return undefined;
  }
  return {...value, usage: parsed.data} as unknown as UsageUpdate;
}

/** Read usage stored with a conversation's initiating interactions. */
export function interactionUsageUpdates(interaction: Interaction): UsageUpdate[] {
  try {
    const raw = interaction.backend_specific_metadata?.['a11.usage'];
    if (!raw) return [];
    const text = utf8Decode(raw);
    if (!isOk(text)) return [];
    const events: unknown = JSON.parse(text);
    return Array.isArray(events) ? events.map(usageUpdate).filter((v): v is UsageUpdate => !!v) : [];
  } catch {
    return [];
  }
}

/** Retain source usage on local conversation history without model content. */
export function rememberUsage(interaction: Interaction, event: unknown): void {
  const update = usageUpdate(event);
  if (!update) return;
  const records = interactionUsageUpdates(interaction);
  if (records.some(r => r.id === update.id && r.revision === update.revision)) return;
  interaction.backend_specific_metadata ??= {};
  interaction.backend_specific_metadata['a11.usage'] = utf8Encode(JSON.stringify([...records, update]));
}

/** Format disjoint counts, reasoning inclusion, and explicit unknown states. */
export function formatUsage(usage: UsageMetadata): string {
  if (usage.context_only || usage.context_invalidated) return '';
  const number = (value: number | null | undefined) => value == null ? '—' : value.toLocaleString('en-US');
  const input = usage.input_tokens == null ? undefined : usage.input_tokens + (usage.cached_input_tokens ?? 0) + (usage.cache_write_tokens ?? 0);
  const cached = usage.cached_input_tokens == null ? '' : ` (${number(usage.cached_input_tokens)} cached)`;
  return `${number(input)} input${cached}, ${number(usage.output_tokens)} output`;
}

/** Sum each replacement snapshot once for a completed turn's compact label. */
export function summarizeUsage(values: readonly UsageMetadata[]): UsageMetadata {
  const latest = new Map<string | number, UsageMetadata>();
  for (const [index, usage] of values.entries()) {
    if (usage.context_only || usage.context_invalidated) continue;
    const key = usage.usage_id ?? index;
    const previous = latest.get(key);
    if (!previous || (usage.usage_revision ?? 0) >= (previous.usage_revision ?? 0)) latest.set(key, usage);
  }
  const records = [...latest.values()];
  const sum = (field: 'input_tokens' | 'output_tokens' | 'cached_input_tokens' | 'cache_write_tokens') => records.length && records.every(r => r[field] != null) ? records.reduce((n, r) => n + r[field]!, 0) : undefined;
  return {input_tokens: sum('input_tokens'), output_tokens: sum('output_tokens'), cached_input_tokens: sum('cached_input_tokens'), cache_write_tokens: sum('cache_write_tokens'), usage_scope: 'turn', complete: records.every(r => r.complete !== false)};
}
