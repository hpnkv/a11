/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import assert from "node:assert/strict";
import test from "node:test";

import {
  applyFlowEdits,
  countFlowDiagnostics,
  decorateFlowTokens,
  diagnosticsOf,
  EMPTY_RUN,
  FlowEditorController,
  FlowRunnerController,
  foldRun,
  filterFlowCompletions,
  hoverOf,
  insertIndentedFlowNewline,
} from "../dist/presentation/index.js";
import { isOk, StatusCode, okStatus, unavailableError } from "../dist/index.js";

test("Flow language replies retain the native versioned contract", () => {
  assert.deepEqual(
    diagnosticsOf({
      ok: true,
      result: { format: "flow.diagnostics/v1", diagnostics: [] },
    }),
    { format: "flow.diagnostics/v1", diagnostics: [] },
  );
  assert.equal(
    hoverOf({ ok: true, result: { format: "something-else", found: true } }),
    null,
  );
  assert.equal(
    applyFlowEdits("one two three", [
      { start: 0, end: 3, text: "1" },
      { start: 8, end: 13, text: "3" },
    ]),
    "1 two 3",
  );
});

test("Flow editor helpers retain Studio completion, indent, and diagnostic policy", () => {
  assert.deepEqual(
    filterFlowCompletions({
      format: "flow.completions/v1",
      prefix: "sk",
      prefix_start: 0,
      proposals: [
        { name: "ask", kind: "flow", insert: "ask" },
        { name: "echo", kind: "flow", insert: "echo" },
      ],
    }).proposals.map(({ name }) => name),
    ["ask"],
  );
  assert.deepEqual(insertIndentedFlowNewline("flow ask {}", 10, 10), {
    value: "flow ask {\n  \n}",
    caret: 13,
  });
  const diagnostic = {
    code: "error",
    severity: "error",
    message: "broken",
    range: {
      start: { offset: 1, line: 1, column: 2 },
      end: { offset: 3, line: 1, column: 4 },
    },
  };
  assert.deepEqual(countFlowDiagnostics([diagnostic]), {
    error: 1,
    warning: 0,
    suggestion: 0,
  });
  assert.deepEqual(
    decorateFlowTokens(
      [{ kind: "plain", text: "abcd", start: 0, end: 4 }],
      [diagnostic],
      4,
    ).map(({ text, diagnostic: covering }) => [text, covering?.code]),
    [
      ["a", undefined],
      ["bc", "error"],
      ["d", undefined],
    ],
  );
});

test("Flow run folding is incremental and uses the shared stream accumulator", () => {
  const first = foldRun(EMPTY_RUN, [
    { kind: "value", port: "text", mimetype: "text/plain", value: "a" },
  ]);
  const retained = first.blocks[0];
  const second = foldRun(first, [
    { kind: "value", port: "text", mimetype: "text/plain", value: "b" },
    { kind: "started", flow: "demo", ports: ["text"] },
  ]);
  assert.equal(retained.text, "a");
  assert.equal(second.blocks[0].text, "ab");
  assert.equal(second.notes[0].kind, "started");
});

test("Flow editor applies native replies without owning another parser", async () => {
  const editor = new FlowEditorController(
    {
      async request(request) {
        if (request.method === "check") {
          return {
            ok: true,
            result: {
              format: "flow.diagnostics/v1",
              diagnostics: [
                { code: "demo", severity: "warning", message: "Seen" },
              ],
            },
          };
        }
        return {
          ok: true,
          result: {
            format: "flow.completions/v1",
            prefix: "as",
            prefix_start: 5,
            proposals: [{ name: "ask", kind: "flow", insert: "ask" }],
          },
        };
      },
    },
    "flow ask {}",
  );

  assert.ok(isOk(await editor.check()));
  assert.equal(editor.getSnapshot().diagnostics[0].code, "demo");
  assert.ok(isOk(await editor.complete(7)));
  assert.equal(editor.getSnapshot().completion.proposals[0].insert, "ask");
});

test("Flow editor turns rejected and invalid service replies into statuses", async () => {
  const rejected = new FlowEditorController({
    async request() {
      throw new Error("offline");
    },
  });
  const transport = await rejected.check();
  assert.equal(isOk(transport), false);
  assert.match(transport.message, /Flow check request failed/i);

  const malformed = new FlowEditorController({
    async request() {
      return { ok: true, result: { format: "other" } };
    },
  });
  const document = await malformed.check();
  assert.equal(document.code, StatusCode.DATA_LOSS);
});

test("superseded Flow language requests cannot overwrite the current document", async () => {
  let release;
  const editor = new FlowEditorController(
    {
      request(_request, signal) {
        return new Promise((resolve) => {
          release = () => resolve(unavailableError("late"));
          signal?.addEventListener("abort", release, { once: true });
        });
      },
    },
    "flow old {}",
  );
  const pending = editor.check();
  assert.ok(isOk(editor.setDocument("flow current {}")));
  release();
  assert.ok(isOk(await pending));
  assert.equal(editor.getSnapshot().document.source, "flow current {}");
  assert.equal(editor.getSnapshot().failure, undefined);
});

test("Flow runner reconciles compiled inputs and consumes a Status iterator", async () => {
  const requests = [];
  const controller = new FlowRunnerController(
    "flow ask {}",
    {
      async compile() {
        return {
          name: "ask",
          inputs: [
            {
              name: "prompt",
              type: "text/plain",
              required: true,
              unary: true,
              json_schema: { type: "string" },
            },
            {
              name: "items",
              type: "application/json",
              unary: false,
              json_schema: { type: "integer" },
            },
          ],
          outputs: [],
        };
      },
    },
    {
      async run(request) {
        requests.push(request);
        return (async function* () {
          return okStatus();
        })();
      },
    },
    { createId: () => "run-1", now: () => 10 },
  );

  assert.ok(isOk(await controller.compile()));
  assert.deepEqual(controller.getSnapshot().input.values, {
    prompt: undefined,
    items: [],
  });
  assert.ok(isOk(controller.setInput("prompt", "hello")));
  assert.ok(isOk(controller.setInput("items", [1, undefined, 2])));
  assert.ok(isOk(await controller.run()));
  assert.deepEqual(requests[0].inputs, { prompt: "hello", items: [1, 2] });
  assert.equal(controller.getSnapshot().execution.runs[0].state, "succeeded");
  assert.equal(controller.getSnapshot().execution.running, false);
});

test("Flow runner preserves stream failures and Studio preview warnings", async () => {
  const warning = new FlowRunnerController(
    "flow ask {}",
    {
      async compile() {
        return unavailableError("compiler offline");
      },
    },
    {
      async run() {
        return (async function* () {
          return unavailableError("run offline");
        })();
      },
    },
    { createId: () => "run-2", now: () => 20 },
  );
  const compiled = await warning.compile();
  assert.equal(isOk(compiled), false);
  assert.match(warning.getSnapshot().contract.warning, /compiler offline/);
  const ran = await warning.run();
  assert.equal(isOk(ran), false);
  assert.equal(ran.message, "run offline");
  assert.equal(warning.getSnapshot().execution.runs[0].state, "failed");
});
