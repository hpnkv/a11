import {type GraphAction} from './runtime_graph.js';
import {type Status, type StatusOr} from './status.js';
import {type UsageMetadata} from './sdk/llm.js';
import {type StreamReference} from './stream_reference.js';
import {enableUsageTracking as enable,
  usageTrackingEnabled as enabled,
  UsageAccounting as Accounting, type UsageLedger} from './sdk/usage.js';

/** Native usage collector with the same class identity as the SDK helper. */
export interface UsageAccounting {
  readonly sources: Map<string, StreamReference>;
  readonly ledgers: Map<string, UsageLedger>;
  readonly errors: Map<string, Status>;
  readonly enabled: boolean;
  readonly finished: boolean;
  readonly coverage: 'disabled' | 'collecting' | 'incomplete' |
    'reported_sources';
  collect(): Promise<Status>;
  directUsage(): StatusOr<UsageMetadata>;
  descendantUsage(): StatusOr<UsageMetadata>;
  totalUsage(): StatusOr<UsageMetadata>;
}

export const UsageAccounting: {
  create(action: GraphAction): Promise<StatusOr<UsageAccounting>>;
} = Accounting;

/** Request usage streams before starting a native action. */
export function enableUsageTracking(action: GraphAction,
    tracking = true): Promise<Status> {
  return enable(action, tracking);
}

/** Read the inherited usage tracking setting of a native action. */
export function usageTrackingEnabled(action: GraphAction):
    Promise<StatusOr<boolean>> {
  return enabled(action);
}
