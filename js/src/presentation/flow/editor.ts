/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import {
  dataLossError,
  invalidArgumentError,
  isOk,
  noexcept,
  okStatus,
  statusFromUnknown,
  type NonOkStatus,
  type Status,
  type StatusOr,
} from "../../status.js";
import type { FlowToken } from "../../flow_highlight.js";
import {
  applyFlowEdits,
  completionsOf,
  diagnosticsOf,
  formatOf,
  hoverOf,
  type FlowCompletion,
  type FlowDiagnostic,
  type FlowFormat,
  type FlowHover,
  type FlowLanguageRequest,
  type FlowLanguageTransport,
  type FlowServiceReply,
} from "./language.js";

export const FLOW_DIAGNOSTIC_DEBOUNCE_MS = 600;
export const FLOW_COMPLETION_DEBOUNCE_MS = 100;
export const FLOW_HOVER_DEBOUNCE_MS = 250;
export const FLOW_HOVER_PROGRESS_MS = 150;
export const FLOW_HOVER_HANDOFF_MS = 500;
export const FLOW_HOVER_CACHE_LIMIT = 128;

export interface DecoratedFlowToken extends FlowToken {
  diagnostic?: FlowDiagnostic;
}

export interface FlowDiagnosticCounts {
  error: number;
  warning: number;
  suggestion: number;
}

/** Split syntax runs only where a native diagnostic begins or ends. */
export function decorateFlowTokens(
  tokens: readonly FlowToken[],
  diagnostics: readonly FlowDiagnostic[],
  length: number,
): DecoratedFlowToken[] {
  const output: DecoratedFlowToken[] = [];
  let offset = 0;
  for (const token of tokens) {
    const tokenStart = offset;
    const tokenEnd = offset + token.text.length;
    const boundaries = new Set([tokenStart, tokenEnd]);
    for (const diagnostic of diagnostics) {
      const start = Math.min(length, diagnostic.range.start.offset);
      const end = Math.min(
        length,
        Math.max(start + 1, diagnostic.range.end.offset),
      );
      if (start > tokenStart && start < tokenEnd) boundaries.add(start);
      if (end > tokenStart && end < tokenEnd) boundaries.add(end);
    }
    const points = [...boundaries].sort((left, right) => left - right);
    for (let index = 0; index < points.length - 1; index += 1) {
      const start = points[index]!;
      const end = points[index + 1]!;
      const covering = diagnostics
        .filter(
          (diagnostic) =>
            diagnostic.range.start.offset <= start &&
            diagnostic.range.end.offset > start,
        )
        .sort(
          (left, right) =>
            flowSeverityRank(left.severity) - flowSeverityRank(right.severity),
        )[0];
      output.push({
        ...token,
        text: token.text.slice(start - tokenStart, end - tokenStart),
        diagnostic: covering,
      });
    }
    offset = tokenEnd;
  }
  return output;
}

export function filterFlowCompletions(
  found: import("./language.js").FlowCompletions,
): import("./language.js").FlowCompletions {
  const prefix = found.prefix.toLocaleLowerCase();
  const proposals = prefix
    ? found.proposals.filter((proposal) =>
        proposal.name.toLocaleLowerCase().includes(prefix),
      )
    : found.proposals;
  return { ...found, proposals };
}

export function lineBeforeFlowCaret(source: string, caret: number): string {
  return source.slice(
    source.lastIndexOf("\n", Math.max(0, caret - 1)) + 1,
    caret,
  );
}

/** Insert a newline with Studio's structural indentation policy. */
export function insertIndentedFlowNewline(
  source: string,
  start: number,
  end: number,
): { value: string; caret: number } {
  const before = lineBeforeFlowCaret(source, start);
  const base = before.match(/^\s*/)?.[0] ?? "";
  const opensBlock = before.trimEnd().endsWith("{");
  const indent = opensBlock ? `${base}  ` : base;
  const closesBlock = opensBlock && source.slice(end).startsWith("}");
  const insertion = closesBlock ? `\n${indent}\n${base}` : `\n${indent}`;
  return {
    value: source.slice(0, start) + insertion + source.slice(end),
    caret: start + 1 + indent.length,
  };
}

export function countFlowDiagnostics(
  diagnostics: readonly FlowDiagnostic[],
): FlowDiagnosticCounts {
  return diagnostics.reduce<FlowDiagnosticCounts>(
    (counts, diagnostic) => {
      if (diagnostic.severity === "error") counts.error += 1;
      else if (diagnostic.severity === "warning") counts.warning += 1;
      else counts.suggestion += 1;
      return counts;
    },
    { error: 0, warning: 0, suggestion: 0 },
  );
}

function flowSeverityRank(severity: FlowDiagnostic["severity"]): number {
  return { error: 0, warning: 1, "weak-warning": 2, information: 3 }[severity];
}

export interface FlowSelection {
  start: number;
  end: number;
}

export interface FlowEditorSnapshot {
  document: {
    source: string;
    path: string;
    selection: FlowSelection;
  };
  diagnostics: readonly FlowDiagnostic[];
  completion: {
    prefix: string;
    prefixStart: number;
    proposals: readonly FlowCompletion[];
  } | null;
  hover: FlowHover | null;
  requests: {
    check: boolean;
    complete: boolean;
    hover: boolean;
    format: boolean;
  };
  failure?: Status;
}

type RequestKind = keyof FlowEditorSnapshot["requests"];

/** Native Flow language orchestration with Studio's cancellation semantics. */
export class FlowEditorController {
  private snapshot: FlowEditorSnapshot;
  private readonly listeners = new Set<() => void>();
  private readonly requests = new Map<RequestKind, AbortController>();
  private readonly generations = new Map<RequestKind, number>();
  private readonly hoverCache = new Map<string, FlowHover>();
  private disposed = false;

  constructor(
    private readonly transport: FlowLanguageTransport,
    source = "",
    path = "",
    private readonly context?: Record<string, unknown>,
  ) {
    this.snapshot = {
      document: { source, path, selection: { start: 0, end: 0 } },
      diagnostics: [],
      completion: null,
      hover: null,
      requests: {
        check: false,
        complete: false,
        hover: false,
        format: false,
      },
    };
  }

  getSnapshot(): FlowEditorSnapshot {
    return this.snapshot;
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  setDocument(
    source: string,
    selection = this.snapshot.document.selection,
    path = this.snapshot.document.path,
  ): Status {
    try {
      this.abort("check");
      this.abort("complete");
      this.abort("hover");
      this.abort("format");
      this.snapshot = {
        ...this.snapshot,
        document: { source, path, selection },
        completion: null,
        hover: null,
        failure: undefined,
      };
      return this.emit();
    } catch (error) {
      return invalidArgumentError(
        "Could not update the Flow document.",
        [],
        error,
      );
    }
  }

  async check(): Promise<Status> {
    try {
      const result = await this.request("check", {
        method: "check",
        ...this.requestDocument(),
        context: this.context,
      });
      if (!isOk<FlowServiceReply | null>(result)) return result;
      if (result === null) return okStatus("Superseded");
      const diagnostics = diagnosticsOf(result);
      if (!diagnostics) {
        return this.fail(
          dataLossError("Flow check returned an invalid diagnostics document."),
        );
      }
      this.snapshot = {
        ...this.snapshot,
        diagnostics: diagnostics.diagnostics,
        failure: undefined,
      };
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not check the Flow document.");
    }
  }

  async complete(offset: number): Promise<Status> {
    try {
      const result = await this.request("complete", {
        method: "complete",
        ...this.requestDocument(),
        offset,
        context: this.context,
      });
      if (!isOk<FlowServiceReply | null>(result)) return result;
      if (result === null) return okStatus("Superseded");
      const completion = completionsOf(result);
      if (!completion) {
        return this.fail(
          dataLossError(
            "Flow completion returned an invalid proposal document.",
          ),
        );
      }
      this.snapshot = {
        ...this.snapshot,
        completion: {
          prefix: completion.prefix,
          prefixStart: completion.prefix_start,
          proposals: completion.proposals,
        },
        failure: undefined,
      };
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not complete the Flow document.");
    }
  }

  acceptCompletion(proposal: FlowCompletion): Status {
    try {
      const completion = this.snapshot.completion;
      if (!completion) return okStatus();
      const start = completion.prefixStart;
      const end = this.snapshot.document.selection.end;
      const source =
        this.snapshot.document.source.slice(0, start) +
        proposal.insert +
        this.snapshot.document.source.slice(end);
      const caret = start + (proposal.caret ?? proposal.insert.length);
      return this.setDocument(source, { start: caret, end: caret });
    } catch (error) {
      return invalidArgumentError(
        "Could not apply the Flow completion.",
        [],
        error,
      );
    }
  }

  async describe(offset: number): Promise<Status> {
    try {
      const key = this.hoverKey(offset);
      const cached = this.hoverCache.get(key);
      if (cached) {
        this.snapshot = { ...this.snapshot, hover: cached, failure: undefined };
        return this.emit();
      }
      const result = await this.request("hover", {
        method: "describe",
        ...this.requestDocument(),
        offset,
        context: this.context,
      });
      if (!isOk<FlowServiceReply | null>(result)) return result;
      if (result === null) return okStatus("Superseded");
      const hover = hoverOf(result);
      if (!hover) {
        return this.fail(
          dataLossError("Flow hover returned an invalid description document."),
        );
      }
      this.hoverCache.set(key, hover);
      while (this.hoverCache.size > 128) {
        const oldest = this.hoverCache.keys().next().value;
        if (oldest === undefined) break;
        this.hoverCache.delete(oldest);
      }
      this.snapshot = { ...this.snapshot, hover, failure: undefined };
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not describe the Flow document.");
    }
  }

  /** Return an already-described symbol without scheduling another request. */
  cachedHover(offset: number): StatusOr<FlowHover | null> {
    try {
      return this.hoverCache.get(this.hoverKey(offset)) ?? null;
    } catch (error) {
      return statusFromUnknown(error, "Could not read the Flow hover cache.");
    }
  }

  clearHover(): Status {
    try {
      this.abort("hover");
      this.snapshot = { ...this.snapshot, hover: null };
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not clear the Flow description.");
    }
  }

  async format(): Promise<Status> {
    try {
      const result = await this.request("format", {
        method: "format",
        ...this.requestDocument(),
      });
      if (!isOk<FlowServiceReply | null>(result)) return result;
      if (result === null) return okStatus("Superseded");
      const formatted = formatOf(result);
      if (!formatted) {
        return this.fail(
          dataLossError("Flow format returned an invalid edit document."),
        );
      }
      return this.applyFormat(formatted);
    } catch (error) {
      return statusFromUnknown(error, "Could not format the Flow document.");
    }
  }

  applyFix(edits: FlowFormat["edits"]): Status {
    try {
      return this.setDocument(
        applyFlowEdits(this.snapshot.document.source, edits),
      );
    } catch (error) {
      return invalidArgumentError("Could not apply the Flow fix.", [], error);
    }
  }

  dispose(): Status {
    try {
      this.disposed = true;
      for (const controller of this.requests.values()) controller.abort();
      this.requests.clear();
      this.listeners.clear();
      return okStatus();
    } catch (error) {
      return invalidArgumentError(
        "Could not dispose the Flow editor.",
        [],
        error,
      );
    }
  }

  private requestDocument(): Pick<
    FlowLanguageRequest,
    "source" | "path" | "offsets"
  > {
    return {
      source: this.snapshot.document.source,
      path: this.snapshot.document.path || undefined,
      offsets: "utf16",
    };
  }

  private async request(
    kind: RequestKind,
    request: FlowLanguageRequest,
  ): Promise<StatusOr<FlowServiceReply | null>> {
    try {
      if (this.disposed) {
        return invalidArgumentError("The Flow editor has been disposed.");
      }
      this.abort(kind);
      const controller = new AbortController();
      this.requests.set(kind, controller);
      const generation = (this.generations.get(kind) ?? 0) + 1;
      this.generations.set(kind, generation);
      this.snapshot = {
        ...this.snapshot,
        requests: { ...this.snapshot.requests, [kind]: true },
      };
      const emitted = this.emit();
      if (!isOk(emitted)) return emitted;
      const result = await noexcept(
        () =>
          kind === "format"
            ? this.transport.request(request)
            : this.transport.request(request, controller.signal),
        `Flow ${kind} request failed.`,
      );
      if (
        controller.signal.aborted ||
        generation !== this.generations.get(kind) ||
        this.disposed
      ) {
        return null;
      }
      this.requests.delete(kind);
      this.snapshot = {
        ...this.snapshot,
        requests: { ...this.snapshot.requests, [kind]: false },
      };
      if (!isOk<FlowServiceReply>(result)) return this.fail(result);
      if (!result.ok) {
        return this.fail(
          invalidArgumentError(
            result.error?.message || `Flow ${kind} request failed.`,
          ),
        );
      }
      return result;
    } catch (error) {
      return statusFromUnknown(error, `Flow ${kind} request failed.`);
    }
  }

  private applyFormat(formatted: FlowFormat): Status {
    const status = this.setDocument(formatted.formatted);
    if (!isOk(status)) return status;
    this.snapshot = {
      ...this.snapshot,
      diagnostics: formatted.diagnostics ?? this.snapshot.diagnostics,
    };
    return this.emit();
  }

  private abort(kind: RequestKind): void {
    this.requests.get(kind)?.abort();
    this.requests.delete(kind);
    this.generations.set(kind, (this.generations.get(kind) ?? 0) + 1);
    if (this.snapshot.requests[kind]) {
      this.snapshot = {
        ...this.snapshot,
        requests: { ...this.snapshot.requests, [kind]: false },
      };
    }
  }

  private hoverKey(offset: number): string {
    return `${this.snapshot.document.path}\u0000${this.snapshot.document.source}\u0000${offset}`;
  }

  private fail(status: NonOkStatus): NonOkStatus {
    this.snapshot = { ...this.snapshot, failure: status };
    this.emit();
    return status;
  }

  private emit(): Status {
    for (const listener of this.listeners) {
      try {
        listener();
      } catch (error) {
        return invalidArgumentError(
          "A Flow editor subscriber could not receive an update.",
          [],
          error,
        );
      }
    }
    return okStatus();
  }
}
