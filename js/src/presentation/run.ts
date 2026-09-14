/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import type { LogLevel } from "../action_log.js";
import { isOk, okStatus, statusFromUnknown, type Status } from "../status.js";
import { appendOutput, type OutputBlock } from "./output.js";

export const MAX_RUNS = 100;
export const MAX_RUN_LOGS = 2_000;

export type RunState = "running" | "succeeded" | "failed" | "cancelled";

export interface RunLog {
  level: LogLevel;
  text: string;
  channel?: string;
  at?: number;
  callId?: string;
}

export interface RunSnapshot<Log extends RunLog = RunLog> {
  id: string;
  action: string;
  inputs: Readonly<Record<string, readonly unknown[]>>;
  headers: Readonly<Record<string, string>>;
  outputs: readonly OutputBlock[];
  logs: readonly Log[];
  omittedLogs: number;
  state: RunState;
  stage: string;
  status?: Status;
  startedAt: number;
  endedAt?: number;
  dispatchStatusAt?: number;
  statusAt?: number;
}

export type RunEvent<Log extends RunLog = RunLog> =
  | { kind: "stage"; stage: string }
  | {
      kind: "output";
      port: string;
      value: unknown;
      mimetype: string;
      textual: boolean;
    }
  | { kind: "log"; log: Log }
  | { kind: "dispatch-status"; at: number }
  | { kind: "status"; at: number }
  | {
      kind: "ended";
      state: Exclude<RunState, "running">;
      at: number;
      status?: Status;
    };

/** Studio-compatible immutable run accumulation with bounded logs and outputs. */
export class RunAccumulator<Log extends RunLog = RunLog> {
  private snapshot: RunSnapshot<Log>;
  private readonly listeners = new Set<() => Status | void>();

  constructor(
    run: Pick<
      RunSnapshot<Log>,
      "id" | "action" | "inputs" | "headers" | "startedAt"
    >,
  ) {
    this.snapshot = {
      ...run,
      outputs: [],
      logs: [],
      omittedLogs: 0,
      state: "running",
      stage: "dispatching",
    };
  }

  getSnapshot(): RunSnapshot<Log> {
    return this.snapshot;
  }

  subscribe(listener: () => Status | void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  accept(event: RunEvent<Log>): Status {
    try {
      if (event.kind === "stage") {
        this.snapshot = { ...this.snapshot, stage: event.stage };
      } else if (event.kind === "output") {
        this.snapshot = {
          ...this.snapshot,
          outputs: appendOutput(
            this.snapshot.outputs,
            event.port,
            event.value,
            event.textual,
            event.mimetype,
          ),
        };
      } else if (event.kind === "log") {
        const logs = [...this.snapshot.logs, event.log];
        const excess = Math.max(0, logs.length - MAX_RUN_LOGS);
        if (excess) logs.splice(0, excess);
        this.snapshot = {
          ...this.snapshot,
          logs,
          omittedLogs: this.snapshot.omittedLogs + excess,
        };
      } else if (event.kind === "dispatch-status") {
        this.snapshot = { ...this.snapshot, dispatchStatusAt: event.at };
      } else if (event.kind === "status") {
        this.snapshot = { ...this.snapshot, statusAt: event.at };
      } else {
        this.snapshot = {
          ...this.snapshot,
          state: event.state,
          endedAt: event.at,
          status: event.status,
        };
      }
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not accumulate the action run.");
    }
  }

  dispose(): Status {
    this.listeners.clear();
    return okStatus();
  }

  private emit(): Status {
    for (const listener of this.listeners) {
      try {
        const status = listener();
        if (status && !isOk(status)) return status;
      } catch (error) {
        return statusFromUnknown(
          error,
          "A run subscriber could not receive an update.",
        );
      }
    }
    return okStatus();
  }
}

export function retainRuns<Run extends { readonly id: string }>(
  runs: readonly Run[],
  next: Run,
  limit = MAX_RUNS,
): Run[] {
  const without = runs.filter((run) => run.id !== next.id);
  return [next, ...without].slice(0, Math.max(1, limit));
}
