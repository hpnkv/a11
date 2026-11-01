import {utf8Decode, utf8Encode} from '../bytes.js';
import {failedPreconditionError, invalidArgumentError, isOk, okStatus, type Status, type StatusOr} from '../status.js';
import {parseStreamReference, type Action, type StreamReference} from '../action.js';
import {AsyncNode} from '../async_node.js';
import {ChunkStoreReader} from '../chunk_store_reader.js';
import {fromChunk} from '../serialization.js';
import {usageMetadataSchema, type Interaction, type UsageMetadata} from './llm.js';

/** Request usage streams for an action subtree, or suppress them with false. */
export function enableUsageTracking(action: Action, enabled = true): Status {
  if (action.hasBeenRun() || action.hasBeenCalled()) {
    return failedPreconditionError('Configure usage tracking before starting the action.');
  }
  return action.setHeader('x-a11-usage-tracking', enabled ? 'on' : 'off');
}

/** Read the inherited opt-in for dedicated usage streams. */
export function usageTrackingEnabled(action: Action): StatusOr<boolean> {
  const raw = action.getHeader('x-a11-usage-tracking');
  if (!isOk(raw)) return raw;
  if (raw === null) return false;
  const value = utf8Decode(raw);
  if (!isOk(value)) return value;
  if (value !== 'on' && value !== 'off') {
    return invalidArgumentError("x-a11-usage-tracking must be 'on' or 'off'.");
  }
  return value === 'on';
}

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
  producer_id?: string | null;
  covers?: string[];
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


/** Replace observations by identity and resolve explicit aggregate coverage. */
export class UsageLedger {
  readonly records = new Map<string, UsageUpdate>();

  observe(update: UsageUpdate): boolean {
    const previous = this.records.get(update.id);
    if (previous && previous.revision >= update.revision) return false;
    this.records.delete(update.id);
    this.records.set(update.id, update);
    return true;
  }

  contributions(): StatusOr<UsageUpdate[]> {
    const records = new Map([...this.records].filter(([, record]) =>
      (record.contribution ?? 'consumption') === 'consumption' &&
      !record.usage.context_only && !record.usage.context_invalidated));
    const covered = new Set<string>();
    for (const [id, record] of records) {
      const pending = [...record.covers ?? []];
      const visited = new Set<string>();
      while (pending.length) {
        const target = pending.pop()!;
        if (target === id) return invalidArgumentError('Usage coverage contains a cycle.');
        if (visited.has(target)) continue;
        visited.add(target);
        covered.add(target);
        pending.push(...records.get(target)?.covers ?? []);
      }
    }
    const aggregates = new Set([...records.values()].filter(record =>
      record.usage.usage_scope === 'turn' && record.scope?.agent_id && record.scope.turn_id)
      .map(record => JSON.stringify([record.scope!.agent_id, record.scope!.turn_id])));
    return [...records].filter(([id, record]) => !covered.has(id) && (
      record.usage.usage_scope === 'turn' ||
      !aggregates.has(JSON.stringify([record.scope?.agent_id, record.scope?.turn_id]))
    )).map(([, record]) => record);
  }

  totals(): StatusOr<UsageMetadata> {
    const records = this.contributions();
    if (!isOk(records)) return records;
    const result = summarizeUsage(records.map(record => record.usage));
    for (const name of ['total_tokens', 'reasoning_tokens'] as const) {
      result[name] = records.length && records.every(record => record.usage[name] != null)
        ? records.reduce((sum, record) => sum + record.usage[name]!, 0) : undefined;
    }
    const primary = records.filter(record => !record.scope?.parent_agent_id).at(-1)?.usage;
    result.context_tokens = primary?.context_tokens;
    result.context_source = primary?.context_source;
    if (records.some(record => record.scope?.reporting === 'cumulative')) {
      for (const name of ['input_tokens', 'output_tokens', 'cached_input_tokens',
        'cache_write_tokens', 'total_tokens', 'reasoning_tokens'] as const) result[name] = undefined;
      result.complete = false;
    }
    return result;
  }
}

/** Collect direct and nested usage through independent stream readers. */
export class UsageAccounting {
  readonly sources = new Map<string, StreamReference>();
  readonly ledgers = new Map<string, UsageLedger>();
  readonly errors = new Map<string, Status>();
  finished = false;
  private collecting = false;
  private readonly directory: Promise<StatusOr<AsyncNode>> | null;

  private constructor(readonly action: Action, readonly enabled: boolean) {
    this.directory = enabled ? action.getStreamSourcesNode() : null;
  }

  /** Request tracking before starting an action; an explicit off is respected. */
  static create(action: Action): StatusOr<UsageAccounting> {
    const raw = action.getHeader('x-a11-usage-tracking');
    if (!isOk(raw)) return raw;
    const setting = raw === null ? null : utf8Decode(raw);
    if (setting !== null && !isOk(setting)) return setting;
    if (!action.hasBeenRun() && !action.hasBeenCalled() && setting !== 'off') {
      const requested = enableUsageTracking(action);
      if (!isOk(requested)) return requested;
    }
    const enabled = usageTrackingEnabled(action);
    return isOk(enabled) ? new UsageAccounting(action, enabled) : enabled;
  }

  async collect(): Promise<Status> {
    if (this.collecting || this.finished) return failedPreconditionError('A usage collection may run only once.');
    if (this.directory === null) { this.finished = true; return okStatus(); }
    this.collecting = true;
    const tasks: Promise<void>[] = [];
    const node = await this.directory;
    if (!isOk(node)) { this.errors.set('scope', node); this.finished = true; return node; }
    const directoryStatus = await this.read(node, async value => {
      if (!value || typeof value !== 'object') return invalidArgumentError('Invalid stream reference.');
      const purpose = (value as Record<string, unknown>).purpose;
      if (purpose !== 'llm_usage') return okStatus();
      const reference = parseStreamReference(value);
      if (!isOk(reference)) return reference;
      const previous = this.sources.get(reference.producer_id);
      if (previous) return JSON.stringify(previous) === JSON.stringify(reference)
        ? okStatus() : invalidArgumentError('Conflicting usage producer identity.');
      this.sources.set(reference.producer_id, reference);
      const ledger = new UsageLedger();
      this.ledgers.set(reference.producer_id, ledger);
      tasks.push((async () => {
        const stream = await this.action.getNodeMap().resolve(reference.node);
        const status = isOk(stream) ? await this.read(stream, async value => {
          const update = usageUpdate(value);
          if (!update || (update.producer_id != null && update.producer_id !== reference.producer_id)) {
            return invalidArgumentError('Invalid usage producer observation.');
          }
          ledger.observe(update);
          return okStatus();
        }) : stream;
        if (!isOk(status)) this.errors.set(reference.producer_id, status);
      })());
      return okStatus();
    });
    if (!isOk(directoryStatus)) this.errors.set('scope', directoryStatus);
    await Promise.all(tasks);
    this.collecting = false;
    this.finished = true;
    return directoryStatus;
  }

  private async read(node: AsyncNode, consume: (value: unknown) => Promise<Status>): Promise<Status> {
    const reader = ChunkStoreReader.create(node.chunkStore);
    if (!isOk(reader)) return reader;
    try {
      for (;;) {
        const fragment = await reader.next();
        if (!isOk(fragment)) return fragment;
        if (fragment === null) return okStatus();
        const chunk = fragment.getChunk();
        if (!isOk(chunk)) return chunk;
        if (chunk.isNull) continue;
        const value = await fromChunk(chunk);
        if (!isOk(value)) return value;
        const status = await consume(value);
        if (!isOk(status)) return status;
      }
    } finally { reader.cancel(); }
  }

  private ledger(producers: Iterable<string>): UsageLedger {
    const result = new UsageLedger();
    for (const id of producers) for (const update of this.ledgers.get(id)!.records.values()) result.observe(update);
    return result;
  }

  directUsage(): StatusOr<UsageMetadata> {
    return this.ledger([...this.sources].filter(([, source]) =>
      !this.sources.has(source.parent_producer_id)).map(([id]) => id)).totals();
  }

  descendantUsage(): StatusOr<UsageMetadata> {
    return this.ledger([...this.sources].filter(([, source]) =>
      this.sources.has(source.parent_producer_id)).map(([id]) => id)).totals();
  }

  totalUsage(): StatusOr<UsageMetadata> {
    const total = this.ledger(this.sources.keys()).totals();
    if (!isOk(total)) return total;
    const direct = this.directUsage();
    if (!isOk(direct)) return direct;
    total.context_tokens = direct.context_tokens;
    total.context_source = direct.context_source;
    if (!this.enabled || !this.finished || this.errors.size || !this.ledgers.size ||
        [...this.ledgers.values()].some(ledger => !ledger.records.size)) total.complete = false;
    return total;
  }

  get coverage(): 'disabled' | 'collecting' | 'incomplete' | 'reported_sources' {
    if (!this.enabled) return 'disabled';
    if (!this.finished) return 'collecting';
    const total = this.totalUsage();
    return isOk(total) && total.complete !== false ? 'reported_sources' : 'incomplete';
  }
}
