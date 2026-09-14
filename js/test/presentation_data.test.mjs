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
  ActionCatalogueController,
  MAX_OUTPUT_VALUES,
  PresentationStorage,
  RunAccumulator,
  appendOutput,
  blankFor,
  consumeStatusIterator,
  describePortInput,
  compactJsonPreview,
  headerFieldsFor,
  groupLogs,
  indentJson,
  jsonTokens,
  missing,
  parseFlowDuration,
  presentValue,
  resolveCallHeaders,
  safeJson,
  sendWithRelativeDelays,
  TrafficController,
  schemaShape,
  typeLabel,
} from "../dist/presentation/index.js";
import {
  isOk,
  noexcept,
  okStatus,
  statusFromUnknown,
  WireMessage,
} from "../dist/index.js";

test("Studio schema semantics preserve nullable unions and nested errors", () => {
  const nullable = { anyOf: [{ type: "string" }, { type: "null" }] };
  assert.equal(schemaShape(nullable).nullable, true);
  assert.equal(typeLabel(nullable), "string | null");
  assert.equal(blankFor({ type: "boolean" }), false);
  assert.equal(
    missing(
      {
        type: "object",
        required: ["person"],
        properties: {
          person: {
            type: "object",
            required: ["name"],
            properties: { name: { type: "string" } },
          },
        },
      },
      { person: {} },
      "input",
      true,
    ),
    "input.person.name is required",
  );
});

test("Studio output reduction keeps text joined and structured values bounded", () => {
  let text = appendOutput([], "text", "hello", true, "text/plain");
  text = appendOutput(text, "text", " world", true, "text/plain");
  assert.equal(text[0].text, "hello world");
  assert.deepEqual(text[0].parts, ["hello", " world"]);
  let values = [];
  for (let index = 0; index <= MAX_OUTPUT_VALUES; index += 1) {
    values = appendOutput(values, "items", index, false, "application/json");
  }
  assert.equal(values[0].values.length, MAX_OUTPUT_VALUES);
  assert.equal(values[0].omitted, 1);
  assert.equal(values[0].values[0], 1);
});

test("JSON tokens are semantic ranges and indentation is renderer neutral", () => {
  assert.deepEqual(
    jsonTokens('{"ok": true, "n": 2}').map(({ kind }) => kind),
    [
      "punctuation",
      "key",
      "punctuation",
      "keyword",
      "punctuation",
      "key",
      "punctuation",
      "number",
      "punctuation",
    ],
  );
  assert.deepEqual(indentJson("{}", 1, 1), {
    text: "{  }",
    selectionStart: 3,
    selectionEnd: 3,
  });
});

test("Flow durations use Studio units and reject partial input", () => {
  assert.equal(parseFlowDuration("1m30s534ms"), 90_534);
  assert.equal(parseFlowDuration("1.5s"), 1_500);
  assert.equal(parseFlowDuration("1 second"), null);
});

test("scheduled input turns rejected adapters and failed sends into statuses", async () => {
  const rejected = await sendWithRelativeDelays(
    ["one"],
    [0],
    async () => {
      throw new Error("bridge closed");
    },
    { startedAt: 0, waitUntil: async () => true },
  );
  assert.equal(isOk(rejected), false);
  assert.match(rejected.message, /scheduled input/i);

  const failed = await sendWithRelativeDelays(
    ["one"],
    [0],
    async () => ({ code: 14, message: "offline" }),
    { startedAt: 0, waitUntil: async () => true },
  );
  assert.equal(isOk(failed), false);
  assert.equal(failed.message, "offline");
});

test("value presentation is MIME-first and falls back to bounded exact bytes", () => {
  const image = presentValue(Uint8Array.of(1, 2), "image/png");
  assert.equal(isOk(image), true);
  assert.equal(image.kind, "image");
  assert.equal(image.extension, "png");
  const broken = presentValue(Uint8Array.of(0xc1), "application/msgpack");
  assert.equal(isOk(broken), true);
  assert.equal(broken.kind, "binary");
  assert.equal(broken.preview, "\\xc1");
  const json = presentValue('{"ok":true}', "application/json");
  assert.equal(isOk(json), true);
  assert.equal(json.kind, "json");
  assert.equal(json.value.ok, true);
});

test("semantic input widgets and action lists remain framework neutral", () => {
  const widget = describePortInput(
    {
      name: "request",
      type: "application/json",
      required: true,
      json_schema: {
        type: "object",
        required: ["count"],
        properties: { count: { type: "integer", minimum: 1 } },
      },
    },
    { count: 2 },
  );
  assert.equal(isOk(widget), true);
  assert.equal(widget.kind, "object");
  assert.equal(widget.children[0].kind, "integer");
  assert.equal(widget.children[0].minimum, 1);

  const actions = new ActionCatalogueController({
    format: "a11.actions/v1",
    actions: [
      { name: "zeta", description: "Last" },
      { name: "alpha", description: "First" },
    ],
  });
  assert.equal(isOk(actions.setQuery("first")), true);
  assert.deepEqual(
    actions.getSnapshot().visible.map(({ name }) => name),
    ["alpha"],
  );
});

test("Studio header precedence and compact value previews are shared", () => {
  const fields = headerFieldsFor({
    headers: [
      { name: "x-otel-traceparent" },
      { name: "x-a11-llm-provider", default: "ollama" },
    ],
  });
  assert.equal(isOk(fields), true);
  assert.deepEqual(
    fields.map(({ name }) => name),
    ["x-a11-llm-provider", "x-otel-traceparent"],
  );
  const headers = resolveCallHeaders(
    fields,
    {},
    [{ name: "X-Project", value: "" }],
    { "X-A11-LLM-PROVIDER": "claude", "x-project": "studio" },
  );
  assert.equal(isOk(headers), true);
  assert.deepEqual(headers, {
    "x-a11-llm-provider": "claude",
    "X-Project": "studio",
  });
  assert.equal(
    compactJsonPreview({ answer: 42 }, "application/json"),
    '{"answer":42}',
  );
});

test("run and persistence state are bounded and exception-free", () => {
  const run = new RunAccumulator({
    id: "1",
    action: "demo",
    inputs: {},
    headers: {},
    startedAt: 10,
  });
  assert.equal(
    isOk(
      run.accept({
        kind: "output",
        port: "text",
        value: "hi",
        mimetype: "text/plain",
        textual: true,
      }),
    ),
    true,
  );
  assert.equal(run.getSnapshot().outputs[0].text, "hi");

  const held = new Map();
  const storage = new PresentationStorage(
    {
      getItem: (key) => held.get(key) ?? null,
      setItem: (key, value) => held.set(key, value),
      removeItem: (key) => held.delete(key),
    },
    "test",
  );
  assert.equal(
    isOk(storage.write("editor", { bytes: Uint8Array.of(1, 2) })),
    true,
  );
  const restored = storage.read("editor");
  assert.equal(isOk(restored), true);
  assert.deepEqual(restored.bytes, Uint8Array.of(1, 2));
});

test("nested action logs retain Studio grouping and completion timing", () => {
  const grouped = groupLogs([
    {
      callId: "call-1",
      actionName: "search",
      channel: "tool",
      text: "searching",
      timestampMs: 10,
    },
    {
      callId: "call-1",
      actionName: "search",
      channel: "status",
      text: "done",
      timestampMs: 20,
      status: { code: 0, message: "done" },
    },
  ]);
  assert.equal(isOk(grouped), true);
  assert.equal(grouped[0].name, "search");
  assert.equal(grouped[0].endedAt, 20);
});

test("status-aware iteration preserves its terminal status", async () => {
  async function* values() {
    yield "one";
    yield "two";
    return okStatus();
  }
  const seen = [];
  const status = await consumeStatusIterator(values(), (value) => {
    seen.push(value);
    return okStatus();
  });
  assert.deepEqual(seen, ["one", "two"]);
  assert.equal(isOk(status), true);
});

test("wire traffic is bounded, filterable, and redacts Studio auth fields", () => {
  const traffic = new TrafficController(2);
  const first = new WireMessage({
    headers: new Map([
      ["x-a11-auth", new TextEncoder().encode("secret")],
      ["ordinary", new TextEncoder().encode("visible")],
    ]),
  });
  assert.ok(isOk(traffic.accept(first, "out", 1)));
  assert.ok(isOk(traffic.accept(new WireMessage(), "in", 2, "flow")));
  assert.equal(traffic.getSnapshot().metrics.sent, 1);
  assert.equal(traffic.getSnapshot().metrics.received, 1);
  assert.ok(isOk(traffic.setFilters({ origin: "flow" })));
  assert.equal(traffic.getSnapshot().visible.length, 1);
  const copied = traffic.copyValue();
  assert.ok(isOk(copied));
  assert.equal(copied[0].headers[0].value.text, "visible");
  assert.equal(copied[0].headers[1].value.text, "••••••••");
});

test("exception boundaries survive hostile user exceptions and thenables", async () => {
  const hostile = new Error("hidden");
  Object.defineProperty(hostile, "message", {
    get() {
      throw new Error("message getter failed");
    },
  });
  assert.doesNotThrow(() => statusFromUnknown(hostile));

  const thenable = {
    then(_resolve, reject) {
      reject(hostile);
    },
  };
  const result = await noexcept(() => thenable, "External operation failed.");
  assert.equal(isOk(result), false);
  assert.equal(result.message, "External operation failed.");

  const unprintable = new Proxy(
    {},
    {
      get(_target, name) {
        if (
          name === "toJSON" ||
          name === Symbol.toPrimitive ||
          name === "toString"
        )
          throw hostile;
        return undefined;
      },
    },
  );
  assert.equal(safeJson(unprintable), "<unprintable value>");
});
