/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import { statusFromUnknown, type StatusOr } from "../status.js";

export interface GroupableLog {
  callId?: string;
  actionName?: string;
  timestampMs?: number;
  channel?: string;
  text: string;
  status?: unknown;
}

export interface LogGroup<Log extends GroupableLog> {
  key: string;
  name: string;
  lines: Log[];
  status?: Log["status"];
  startedAt: number;
  endedAt?: number;
}

/** Studio's parent/child action-log grouping and lifecycle timing. */
export function groupLogs<Log extends GroupableLog>(
  lines: readonly Log[],
  now: () => number = Date.now,
): StatusOr<LogGroup<Log>[]> {
  try {
    const groups: LogGroup<Log>[] = [];
    const known = new Map<string, LogGroup<Log>>();
    for (const line of lines) {
      const key = line.callId ?? "__parent__";
      let group = known.get(key);
      if (!group) {
        group = {
          key,
          name:
            line.actionName ?? (line.callId ? "child action" : "action log"),
          lines: [],
          startedAt: line.timestampMs ?? now(),
        };
        known.set(key, group);
        groups.push(group);
      }
      group.lines.push(line);
      if (line.status) {
        group.status = line.status;
        group.endedAt = line.timestampMs ?? now();
      } else if (line.channel === "lifecycle" && line.text === "completed") {
        group.endedAt = line.timestampMs ?? now();
      }
    }
    return groups;
  } catch (error) {
    return statusFromUnknown(error, "Could not group the action logs.");
  }
}
