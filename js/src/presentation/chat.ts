/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import type { Interaction } from "../sdk/llm.js";
import {
  toolInputs,
  type PresentationBlock,
} from "../sdk/presentation.js";
import {
  isOk,
  statusFromUnknown,
  type StatusOr,
} from "../status.js";

export const COMPLETE_FLOW_PREVIEW_LINES = 40;

export type ChatToolKind =
  | "tool"
  | "flow"
  | "completion-report"
  | "input-request";

export interface ChatToolPresentation {
  kind: ChatToolKind;
  flowSource: string;
  detailInputs: Record<string, unknown> | undefined;
  inputPreview: string;
  completeFlowPreview: boolean;
}

/** Studio's bounded, single-line summary of tool arguments. */
export function toolInputPreview(
  inputs: Record<string, unknown> | undefined,
): StatusOr<string> {
  try {
    if (!inputs || Object.keys(inputs).length === 0) return "";
    const entries = Object.entries(inputs).slice(0, 5);
    const preview = entries
      .map(([name, value]) => `${name}: ${boundedValuePreview(value, 0)}`)
      .join(" · ");
    const suffix = Object.keys(inputs).length > entries.length ? " · …" : "";
    return `${preview}${suffix}`.slice(0, 360);
  } catch (error) {
    return statusFromUnknown(error, "Could not preview tool inputs.");
  }
}

/** Keep short Flow programs fully visible while bounding longer previews. */
export function showCompleteFlowPreview(source: string): boolean {
  if (!source) return false;
  const withoutFinalNewline = source.replace(/\r?\n$/, "");
  return (
    withoutFinalNewline.split(/\r?\n/).length <= COMPLETE_FLOW_PREVIEW_LINES
  );
}

/** Classify and derive one tool card without prescribing its rendered component. */
export function describeChatTool(
  block: Pick<
    PresentationBlock,
    "toolName" | "toolArguments" | "toolArgumentsComplete"
  >,
): StatusOr<ChatToolPresentation> {
  try {
    if (block.toolName === "report_completion" && block.toolArgumentsComplete) {
      return {
        kind: "completion-report",
        flowSource: "",
        detailInputs: block.toolArguments,
        inputPreview: "",
        completeFlowPreview: false,
      };
    }
    if (block.toolName === "request_user_input" && block.toolArgumentsComplete) {
      return {
        kind: "input-request",
        flowSource: "",
        detailInputs: block.toolArguments,
        inputPreview: "",
        completeFlowPreview: false,
      };
    }
    const flowSource =
      block.toolName === "run_flow" &&
      typeof block.toolArguments?.["source"] === "string"
        ? block.toolArguments["source"]
        : "";
    const detailInputs = flowSource
      ? Object.fromEntries(
          Object.entries(block.toolArguments ?? {}).filter(
            ([name]) => name !== "source",
          ),
        )
      : block.toolArguments;
    const inputPreview = toolInputPreview(detailInputs);
    if (!isOk(inputPreview)) return inputPreview;
    return {
      kind: flowSource ? "flow" : "tool",
      flowSource,
      detailInputs,
      inputPreview,
      completeFlowPreview: showCompleteFlowPreview(flowSource),
    };
  } catch (error) {
    return statusFromUnknown(error, "Could not describe the chat tool.");
  }
}

export interface PendingUserInput {
  id: string;
  question: string;
  options: { label: string; description: string }[];
  allowFreeText: boolean;
}

/** Decode Studio's pending-input workflow from a complete interaction. */
export async function pendingUserInput(
  interaction: Interaction,
): Promise<StatusOr<PendingUserInput | null>> {
  try {
    const call = interaction.action_calls?.find(
      (candidate) => candidate.name === "request_user_input",
    );
    if (!call) return null;
    const decoded = await toolInputs(interaction);
    if (!isOk(decoded)) return decoded;
    const inputs = decoded[call.id] ?? {};
    const value = (name: string): unknown => inputs[name];
    const options = Array.isArray(value("options"))
      ? (value("options") as unknown[])
          .filter(
            (option): option is Record<string, unknown> =>
              option !== null && typeof option === "object",
          )
          .map((option) => ({
            label: String(option["label"] ?? ""),
            description: String(option["description"] ?? ""),
          }))
          .filter((option) => option.label)
      : [];
    return {
      id: call.id,
      question: String(value("question") || "Input requested"),
      options,
      allowFreeText:
        options.length === 0 || value("allow_free_text") !== false,
    };
  } catch (error) {
    return statusFromUnknown(error, "Could not read the pending input request.");
  }
}

function boundedValuePreview(value: unknown, depth: number): string {
  if (value === null) return "null";
  if (typeof value === "string") {
    const text = value.length > 120 ? `${value.slice(0, 117)}…` : value;
    return JSON.stringify(text);
  }
  if (typeof value === "number" || typeof value === "boolean") {
    return String(value);
  }
  if (value instanceof Uint8Array) return `<${value.byteLength} bytes>`;
  if (Array.isArray(value)) {
    if (depth >= 2) return `[… ${value.length} items]`;
    const items = value
      .slice(0, 3)
      .map((item) => boundedValuePreview(item, depth + 1));
    if (value.length > items.length) items.push("…");
    return `[${items.join(", ")}]`;
  }
  if (value && typeof value === "object") {
    if (depth >= 2) return "{…}";
    const entries = Object.entries(value as Record<string, unknown>).slice(0, 4);
    const fields = entries.map(
      ([name, field]) => `${name}: ${boundedValuePreview(field, depth + 1)}`,
    );
    if (Object.keys(value).length > entries.length) fields.push("…");
    return `{${fields.join(", ")}}`;
  }
  try {
    return String(value);
  } catch {
    return "<unprintable>";
  }
}
