/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import type { WireMessage } from "../../data.js";
import type { PortEntry, SchemaEntry } from "../../schema_json.js";
import {
  cancelledError,
  failedPreconditionError,
  invalidArgumentError,
  isOk,
  noexcept,
  okStatus,
  statusFromUnknown,
  StatusCode,
  type NonOkStatus,
  type Status,
  type StatusOr,
} from "../../status.js";
import { consumeStatusIterator } from "../iterator.js";
import {
  MAX_RUNS,
  RunAccumulator,
  retainRuns,
  type RunSnapshot,
} from "../run.js";
import { asSchema, blankFor, isEmpty, missing } from "../schema.js";
import { FlowReader, type FlowEntry } from "./stream.js";

export const FLOW_SCHEMA_DEBOUNCE_MS = 350;

/** Convert flow_check's first native plan entry into an action-shaped contract. */
export function schemaEntryFromFlowPlan(
  value: unknown,
): StatusOr<SchemaEntry | null> {
  try {
    if (
      !isRecord(value) ||
      !Array.isArray(value["flows"]) ||
      value["flows"].length === 0
    )
      return null;
    const flow = value["flows"][0];
    if (!isRecord(flow) || typeof flow["flow"] !== "string") return null;
    const inputs = describedPorts(flow["inputs"]);
    if (!isOk(inputs)) return inputs;
    const outputs = describedPorts(flow["outputs"]);
    if (!isOk(outputs)) return outputs;
    if (inputs === null || outputs === null) return null;
    return {
      name: flow["flow"],
      description:
        typeof flow["description"] === "string" ? flow["description"] : "",
      inputs,
      outputs,
      headers: Array.isArray(flow["headers"])
        ? flow["headers"]
            .filter((name): name is string => typeof name === "string")
            .map((name) => ({ name }))
        : [],
    };
  } catch (error) {
    return statusFromUnknown(error, "Could not read the compiled Flow plan.");
  }
}

function describedPorts(value: unknown): StatusOr<PortEntry[] | null> {
  try {
    if (!isRecord(value)) return null;
    const ports: PortEntry[] = [];
    for (const [name, described] of Object.entries(value)) {
      if (!isRecord(described) || typeof described["type"] !== "string")
        return null;
      ports.push({
        name,
        type: described["type"],
        description:
          typeof described["description"] === "string"
            ? described["description"]
            : "",
        required: described["required"] === true,
        unary: described["unary"] !== false,
        json_schema: jsonSchemaForFlowType(described["type"]),
      });
    }
    return ports;
  } catch (error) {
    return statusFromUnknown(error, "Could not read compiled Flow ports.");
  }
}

function jsonSchemaForFlowType(
  type: string,
): Record<string, unknown> | undefined {
  const normalized = type.trim().toLowerCase();
  if (normalized === "string" || normalized === "str")
    return { type: "string" };
  if (normalized === "integer" || normalized === "int")
    return { type: "integer" };
  if (normalized === "number" || normalized === "float")
    return { type: "number" };
  if (normalized === "boolean" || normalized === "bool")
    return { type: "boolean" };
  if (
    normalized === "object" ||
    normalized === "dict" ||
    normalized === "json"
  )
    return { type: "object" };
  const list = /^list\[(.+)]$/.exec(normalized);
  return list
    ? {
        type: "array",
        items: jsonSchemaForFlowType(list[1]!) ?? {},
      }
    : undefined;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

export interface FlowCompileService {
  compile(source: string, signal: AbortSignal): Promise<StatusOr<SchemaEntry>>;
}

export interface FlowRunRequest {
  source: string;
  inputs: Readonly<Record<string, unknown>>;
}

/** A run stream returns its final outcome instead of rejecting. */
export interface FlowRunService {
  run(
    request: FlowRunRequest,
    signal: AbortSignal,
  ): Promise<StatusOr<AsyncIterator<WireMessage, Status>>>;
}

export interface FlowRunnerSnapshot {
  document: {
    source: string;
    compiledSource: string | null;
  };
  contract: {
    schema: SchemaEntry | null;
    warning: string | null;
    compiling: boolean;
  };
  input: {
    values: Readonly<Record<string, unknown>>;
    failure: string | null;
  };
  execution: {
    running: boolean;
    selectedRunId: string | null;
    runs: readonly RunSnapshot[];
  };
  failure?: Status;
}

export interface FlowRunnerOptions {
  now?: () => number;
  createId?: () => string;
  onTraffic?: (message: WireMessage) => Status;
  maxRuns?: number;
}

/**
 * Studio's Flow compile/input/run workflow without React, DOM, or transport
 * assumptions. Products provide the native compiler and wire-stream adapters.
 */
export class FlowRunnerController {
  private snapshot: FlowRunnerSnapshot;
  private readonly listeners = new Set<() => void>();
  private compileController: AbortController | null = null;
  private runController: AbortController | null = null;
  private compileGeneration = 0;
  private disposed = false;
  private readonly now: () => number;
  private readonly createId: () => string;
  private readonly maxRuns: number;

  constructor(
    source: string,
    private readonly compiler: FlowCompileService,
    private readonly runner: FlowRunService,
    private readonly options: FlowRunnerOptions = {},
  ) {
    this.now = options.now ?? (() => Date.now());
    this.createId =
      options.createId ??
      (() =>
        typeof crypto !== "undefined" && "randomUUID" in crypto
          ? crypto.randomUUID()
          : `${Date.now()}-${Math.random()}`);
    this.maxRuns = options.maxRuns ?? MAX_RUNS;
    this.snapshot = {
      document: { source, compiledSource: null },
      contract: { schema: null, warning: null, compiling: false },
      input: { values: {}, failure: null },
      execution: { running: false, selectedRunId: null, runs: [] },
    };
  }

  getSnapshot(): FlowRunnerSnapshot {
    return this.snapshot;
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  setSource(source: string): Status {
    try {
      this.abortCompile();
      this.snapshot = {
        ...this.snapshot,
        document: { ...this.snapshot.document, source },
        contract: { ...this.snapshot.contract, compiling: false },
        input: { ...this.snapshot.input, failure: null },
        failure: undefined,
      };
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not update the Flow source.");
    }
  }

  setInput(name: string, value: unknown): Status {
    try {
      this.snapshot = {
        ...this.snapshot,
        input: {
          values: { ...this.snapshot.input.values, [name]: value },
          failure: null,
        },
      };
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, `Could not update Flow input '${name}'.`);
    }
  }

  async compile(): Promise<Status> {
    try {
      if (this.disposed)
        return failedPreconditionError("The Flow runner has been disposed.");
      this.abortCompile();
      const controller = new AbortController();
      this.compileController = controller;
      const generation = ++this.compileGeneration;
      const source = this.snapshot.document.source;
      this.snapshot = {
        ...this.snapshot,
        contract: { ...this.snapshot.contract, compiling: true },
        failure: undefined,
      };
      const emitted = this.emit();
      if (!isOk(emitted)) return emitted;
      const compiled = await noexcept(
        () => this.compiler.compile(source, controller.signal),
        "Could not compile the Flow interface.",
      );
      if (
        controller.signal.aborted ||
        generation !== this.compileGeneration ||
        this.disposed
      ) {
        return okStatus("Superseded");
      }
      this.compileController = null;
      if (!isOk<SchemaEntry>(compiled)) {
        this.applyCompileFailure(source, compiled);
        const notified = this.emit();
        return isOk(notified) ? compiled : notified;
      }
      this.applyCompiledSchema(source, compiled);
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not compile the Flow interface.");
    }
  }

  async run(): Promise<Status> {
    try {
      if (this.disposed)
        return failedPreconditionError("The Flow runner has been disposed.");
      if (this.snapshot.execution.running)
        return failedPreconditionError("A Flow is already running.");
      if (!this.snapshot.contract.schema && !this.snapshot.contract.warning)
        return failedPreconditionError("The Flow has no runnable interface.");
      const prepared = prepareFlowInputs(
        this.snapshot.contract.schema,
        this.snapshot.input.values,
      );
      if (!isOk<PreparedFlowInputs>(prepared)) {
        this.snapshot = {
          ...this.snapshot,
          input: { ...this.snapshot.input, failure: prepared.message },
          failure: prepared,
        };
        this.emit();
        return prepared;
      }

      const controller = new AbortController();
      this.runController = controller;
      const startedAt = this.now();
      const run = new RunAccumulator({
        id: this.createId(),
        action: `Flow · ${this.snapshot.contract.schema?.name ?? "run"}`,
        inputs: prepared.recorded,
        headers: {},
        startedAt,
      });
      const updateRun = () => this.record(run.getSnapshot());
      const unsubscribe = run.subscribe(updateRun);
      this.snapshot = {
        ...this.snapshot,
        input: { ...this.snapshot.input, failure: null },
        execution: {
          ...this.snapshot.execution,
          running: true,
          selectedRunId: run.getSnapshot().id,
        },
        failure: undefined,
      };
      let status: Status = updateRun();
      const opened = isOk(status)
        ? await noexcept(
            () =>
              this.runner.run(
                {
                  source: this.snapshot.document.source,
                  inputs: prepared.sending,
                },
                controller.signal,
              ),
            "Could not start the Flow run.",
          )
        : status;
      if (!isOk<AsyncIterator<WireMessage, Status>>(opened)) status = opened;
      else {
        const reader = new FlowReader();
        let outcome: Status | undefined;
        status = await consumeStatusIterator(opened, async (message) => {
          const traffic = this.options.onTraffic?.(message) ?? okStatus();
          if (!isOk(traffic)) return traffic;
          const entries = await reader.accept(message);
          if (!isOk<FlowEntry[]>(entries)) return entries;
          for (const entry of entries) {
            const accepted = this.acceptFlowEntry(run, entry);
            if (!isOk(accepted)) return accepted;
            if (entry.kind === "outcome") outcome = entry.status;
          }
          return okStatus();
        });
        if (isOk(status) && outcome !== undefined) status = outcome;
      }

      const endedAt = this.now();
      const finalStatus = controller.signal.aborted
        ? cancelledError("The Flow run was cancelled.")
        : status;
      const state = controller.signal.aborted
        ? "cancelled"
        : isOk(finalStatus)
          ? "succeeded"
          : "failed";
      const ended = run.accept({
        kind: "ended",
        state,
        at: endedAt,
        status: finalStatus,
      });
      unsubscribe();
      run.dispose();
      this.runController = null;
      this.snapshot = {
        ...this.snapshot,
        execution: { ...this.snapshot.execution, running: false },
        failure: isOk(finalStatus) ? undefined : finalStatus,
      };
      const emitted = this.emit();
      if (!isOk(ended)) return ended;
      if (!isOk(emitted)) return emitted;
      return finalStatus;
    } catch (error) {
      this.runController = null;
      this.snapshot = {
        ...this.snapshot,
        execution: { ...this.snapshot.execution, running: false },
      };
      return statusFromUnknown(error, "Could not run the Flow.");
    }
  }

  selectRun(id: string | null): Status {
    try {
      if (
        id !== null &&
        !this.snapshot.execution.runs.some((run) => run.id === id)
      ) {
        return invalidArgumentError(`Flow run '${id}' is not available.`);
      }
      this.snapshot = {
        ...this.snapshot,
        execution: { ...this.snapshot.execution, selectedRunId: id },
      };
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not select the Flow run.");
    }
  }

  clearRuns(): Status {
    try {
      this.snapshot = {
        ...this.snapshot,
        execution: {
          ...this.snapshot.execution,
          runs: [],
          selectedRunId: null,
        },
      };
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not clear the Flow runs.");
    }
  }

  cancel(): Status {
    try {
      this.runController?.abort();
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Could not cancel the Flow run.");
    }
  }

  dispose(): Status {
    try {
      this.disposed = true;
      this.abortCompile();
      this.runController?.abort();
      this.runController = null;
      this.listeners.clear();
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Could not dispose the Flow runner.");
    }
  }

  private applyCompiledSchema(source: string, schema: SchemaEntry): void {
    const current = this.snapshot.input.values;
    const values = Object.fromEntries(
      (schema.inputs ?? []).map((port) => [
        port.name,
        Object.prototype.hasOwnProperty.call(current, port.name)
          ? current[port.name]
          : port.unary === false
            ? []
            : blankFor(asSchema(port.json_schema)),
      ]),
    );
    this.snapshot = {
      ...this.snapshot,
      document: { ...this.snapshot.document, compiledSource: source },
      contract: { schema, warning: null, compiling: false },
      input: { values, failure: null },
      failure: undefined,
    };
  }

  private applyCompileFailure(source: string, status: NonOkStatus): void {
    const previewUnavailable = schemaMayBeUnavailable(status.code);
    this.snapshot = {
      ...this.snapshot,
      document: { ...this.snapshot.document, compiledSource: source },
      contract: {
        schema: null,
        compiling: false,
        warning: previewUnavailable
          ? `The Flow input and output preview is unavailable: ${status.message} You can still run this Flow; output previews will adapt to the data received.`
          : null,
      },
      input: { ...this.snapshot.input, failure: null },
      failure: previewUnavailable ? undefined : status,
    };
  }

  private acceptFlowEntry(run: RunAccumulator, entry: FlowEntry): Status {
    if (entry.kind === "started") {
      if (run.getSnapshot().dispatchStatusAt !== undefined) return okStatus();
      return run.accept({ kind: "dispatch-status", at: this.now() });
    }
    if (entry.kind === "value") {
      return run.accept({
        kind: "output",
        port: entry.port,
        value: entry.value,
        mimetype: entry.mimetype,
        textual: entry.mimetype.startsWith("text/"),
      });
    }
    if (entry.kind === "log") {
      return run.accept({
        kind: "log",
        log: { ...entry, at: this.now() },
      });
    }
    if (entry.kind === "closed") {
      return isOk(entry.status)
        ? okStatus()
        : run.accept({
            kind: "log",
            log: {
              level: "error",
              channel: "status",
              text: `${entry.port}: ${entry.status.message}`,
              at: this.now(),
            },
          });
    }
    return run.accept({ kind: "status", at: this.now() });
  }

  private record(next: RunSnapshot): Status {
    this.snapshot = {
      ...this.snapshot,
      execution: {
        ...this.snapshot.execution,
        selectedRunId: next.id,
        runs: retainRuns(this.snapshot.execution.runs, next, this.maxRuns),
      },
    };
    return this.emit();
  }

  private abortCompile(): void {
    this.compileGeneration += 1;
    this.compileController?.abort();
    this.compileController = null;
  }

  private emit(): Status {
    for (const listener of this.listeners) {
      try {
        listener();
      } catch (error) {
        return statusFromUnknown(
          error,
          "A Flow runner subscriber could not receive an update.",
        );
      }
    }
    return okStatus();
  }
}

interface PreparedFlowInputs {
  sending: Record<string, unknown>;
  recorded: Record<string, readonly unknown[]>;
}

function prepareFlowInputs(
  schema: SchemaEntry | null,
  inputs: Readonly<Record<string, unknown>>,
): StatusOr<PreparedFlowInputs> {
  try {
    const sending: Record<string, unknown> = {};
    const recorded: Record<string, readonly unknown[]> = {};
    for (const port of schema?.inputs ?? []) {
      const held = inputs[port.name];
      const values =
        port.unary === false
          ? Array.isArray(held)
            ? held.filter((value) => !isEmpty(value))
            : []
          : [held];
      const reason =
        port.unary === false
          ? port.required && values.length === 0
            ? `${port.name} needs at least one value`
            : null
          : missing(
              asSchema(port.json_schema),
              held,
              port.name,
              port.required === true,
            );
      if (reason) return invalidArgumentError(reason);
      if (isEmpty(held)) continue;
      recorded[port.name] = values;
      sending[port.name] = port.unary === false ? values : held;
    }
    return { sending, recorded };
  } catch (error) {
    return statusFromUnknown(error, "Could not prepare the Flow inputs.");
  }
}

function schemaMayBeUnavailable(code: StatusCode): boolean {
  return (
    code === StatusCode.NOT_FOUND ||
    code === StatusCode.UNIMPLEMENTED ||
    code === StatusCode.UNAVAILABLE ||
    code === StatusCode.UNKNOWN
  );
}

const MODEL_ACTIONS = ["ask_model", "interact_with_llm"];
const FLOW_TOOL_ACTIONS = new Set(["flow_actions", "flow_check", "flow_run"]);

/** Studio's starter Flow, selected from the actions a peer advertises. */
export function flowTemplate(
  actions: readonly SchemaEntry[],
  runsOnPeer = false,
): StatusOr<string> {
  try {
    const model = MODEL_ACTIONS.find((name) =>
      actions.some((action) => action.name === name),
    );
    if (model) {
      const described = actions.find((action) => action.name === model)!;
      return modelTemplate(
        model,
        runsOnPeer && described.runnable ? "run" : "call",
      );
    }
    const action = actions.find(
      (candidate) =>
        !candidate.name.startsWith("__") &&
        !FLOW_TOOL_ACTIONS.has(candidate.name),
    );
    if (!action) {
      return 'flow ask {\n  out reply: string stream\n  # said = call some-action(port: "value")\n  # said.output -> reply\n  drain reply\n}\n';
    }
    const input = action.inputs?.[0]?.name;
    const output = action.outputs?.[0]?.name ?? "output";
    const verb = runsOnPeer && action.runnable ? "run" : "call";
    const call = input
      ? `${verb} ${action.name}(${input}: "hello")`
      : `${verb} ${action.name}()`;
    return [
      "flow ask {",
      "  out reply: string stream",
      `  said = ${call}`,
      `  said.${output} -> reply`,
      "  drain reply",
      "}",
      "",
    ].join("\n");
  } catch (error) {
    return statusFromUnknown(error, "Could not build the starter Flow.");
  }
}

function modelTemplate(name: string, verb: "call" | "run"): string {
  return `flow ask {
  describe "Ask the model something, and watch it think and answer."

  out reply:    string stream  "The answer, as it is written."
  out thinking: string stream  "The model's reasoning, as it arrives."

  # Everything worth playing with is here.
  let question = "What is RPC and how to make them distributed?"
  let system   = "Answer in one or two sentences. Be concrete."

  let asked = now()

  said = ${verb} ${name}(
    interactions: a11.sdk.Interaction{
      role: "user",
      system_instructions: [to_chunk(system)],
      content: [to_chunk({
        role: "user",
        content: [{type: "text", text: question}]
      })]
    },
    config: {think: true}
  )
    with "x-a11-llm-provider": "ollama"
    with "x-a11-llm-model": "glm-5.3-flash:cloud"
    with "x-a11-llm-base-url": "https://ollama.com"
    with "x-a11-llm-api-key": "use-a11-demo-resources"
    timeout 4m

  said.text_output -> reply

  # Streamed as it arrives, so a long think shows progress rather than a blank
  # panel -- and logged once, whole, since a token per line is not a thought.
  # \`-> _\` performs the pipeline and keeps nothing.
  said.thoughts | join "" | logf debug "thought: %s" it -> _
  said.thoughts -> thinking

  # \`after done\` is what makes this the last line rather than the first.
  done = wait said
  logf info "asked at %s, answered %s later" asked, (now() - asked) after done
}
`;
}
