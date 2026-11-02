import type {ActionSchema} from '../../action_schema.js';
import type {Status} from '../../status.js';

/** Synchronous native schema checks after WASM initialization. */
export interface SchemaBridge {
  validate(schema: ActionSchema): Status;
  describe(schema: ActionSchema, runnable: boolean, allPorts: boolean):
    Record<string, unknown>;
  parse(entry: Record<string, unknown>): Record<string, unknown>;
  entries(document: unknown): Array<Record<string, unknown>>;
}

let initialized: SchemaBridge | null = null;

export function installSchemaBridge(bridge: SchemaBridge): void {
  initialized = bridge;
}

export function schemaBridge(): SchemaBridge | null {
  return initialized;
}
