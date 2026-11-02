import type { SnapshotStore } from "./index.mjs";

export function indexedDbSnapshotStore(name: string, key?: string):
  SnapshotStore & { close(): Promise<void> };
