// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import assert from "node:assert/strict";
import { fileSnapshotStore } from "./persistence-node.mjs";
import { resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { rm } from "node:fs/promises";

const {createA11Wasm} = await import(process.argv[2]
  ? pathToFileURL(resolve(process.argv[2])).href : './index.mjs');
const a11 = await createA11Wasm();
const encoded = new TextEncoder();

const checked = a11.flow({ method: "check", source: "flow main { }" });
assert.equal(checked.ok, true);
assert.deepEqual(checked.result.diagnostics, []);
const formatted = a11.flow({ method: "format", source: "flow main{ }" });
assert.equal(formatted.ok, true);
assert.match(formatted.result.formatted, /flow main/);

const wire = {
  node_fragments: [{ id: "source", data: { data: "aGVsbG8=" }, seq: 0 }],
};
assert.deepEqual(a11.wire.decode(a11.wire.encode(wire)), wire);
const qualified = {
  node_fragments: [{
    id: "alias", data: { id: "source", offset: 1, length: 1,
      storage: { id: "local" } }, seq: 0,
  }],
};
assert.deepEqual(a11.wire.decode(a11.wire.encode(qualified)), qualified);
const withAccess = {
  node_fragments: [{
    id: "remote_alias", data: { id: "remote", storage: {
      id: "archive",
      services: [{ role: "objects", endpoint: "https://example.invalid",
        credentials: { scheme: "bearer", fields: { token: "test-only" } } }],
    } }, seq: 0,
  }],
};
assert.deepEqual(a11.wire.decode(a11.wire.encode(withAccess)), withAccess);

const bytes = encoded.encode("a message long enough for many little packets");
const packets = a11.framing.split(bytes, 123n, 18);
assert.ok(packets.length > 1);
const reassembler = a11.framing.reassembler({ packetSize: 18 });
let completed = null;
for (const packet of packets.toReversed()) {
  completed = reassembler.feed(packet) ?? completed;
}
assert.deepEqual(completed, bytes);
reassembler.dispose();
const defaultLimits = a11.framing.reassembler();
const withinLimit = new Uint8Array(16 * 1024 * 1024);
let assembled = null;
for (const packet of a11.framing.split(withinLimit, 789n)) {
  assembled = defaultLimits.feed(packet) ?? assembled;
}
assert.equal(assembled?.length, withinLimit.length);
defaultLimits.dispose();
const overLimit = a11.framing.reassembler();
const oversized = a11.framing.split(
  new Uint8Array(16 * 1024 * 1024 + 1), 790n);
assert.throws(() => {
  for (const packet of oversized) overLimit.feed(packet);
}, /exceeds max_message_size/);
overLimit.dispose();
const interleaved = a11.framing.reassembler({ packetSize: 18 });
const other = a11.framing.split(encoded.encode("another long message"), 456n, 18);
let firstDone = null;
let secondDone = null;
for (let i = 0; i < Math.max(packets.length, other.length); i++) {
  if (packets[i]) firstDone = interleaved.feed(packets[i]) ?? firstDone;
  if (other[i]) secondDone = interleaved.feed(other[i]) ?? secondDone;
}
assert.deepEqual(firstDone, bytes);
assert.deepEqual(secondDone, encoded.encode("another long message"));
interleaved.dispose();

const runtime = a11.runtime("local");
runtime.append("source", encoded.encode("first"), "text/plain");
runtime.append("source", encoded.encode("second"), "text/plain");
runtime.close("source");
assert.equal(runtime.count("source"), 2);
assert.equal(runtime.closed("source"), true);
assert.equal(runtime.read("source", 0).id, "source");
assert.deepEqual(runtime.readData("source", 0), encoded.encode("first"));
runtime.appendRef("alias", { id: "source", offset: 1, length: 1 });
runtime.close("alias");
assert.equal(runtime.resolve({ id: "alias" }).length, 1);
assert.equal(runtime.resolve({ id: "source", storage: { id: "local" } }).length, 2);
assert.throws(() => runtime.resolve({ id: "source" }, { maxChunks: 1 }),
  /chunk limit/);
assert.throws(() => runtime.resolve({ id: "source", storage: { id: "elsewhere" } }),
  /host resolver/);
const withResolver = a11.runtime("local", {
  resolveStorage: async (storage, ref) => {
    assert.equal(storage.id, "elsewhere");
    assert.equal(ref.id, "remote");
    return [{ id: "remote", data: { data: "cmVtb3Rl" } }];
  },
});
assert.equal((await withResolver.resolveAsync({ id: "remote", storage: { id: "elsewhere" } })).length, 1);
withResolver.dispose();

runtime.register("echo");
runtime.invoke({
  id: "call1", name: "echo",
  inputs: [{ name: "input", id: "source" }],
  outputs: [{ name: "output", id: "reply" }],
});
await runtime.runPending({
  echo: async (action, rt) => {
    assert.equal(action.id, "call1");
    rt.append("reply", encoded.encode("done"), "text/plain");
  },
});
assert.equal(runtime.closed("reply"), true);
assert.equal(runtime.count("reply"), 1);
runtime.appendRef("cycle", { id: "cycle" });
runtime.close("cycle");
assert.throws(() => runtime.resolve({ id: "cycle" }), /reference cycle/);
assert.equal(runtime.putFragment({ id: "ordered", seq: 2, continued: false,
  data: { data: "dGhpcmQ=" } }), 2);
assert.equal(runtime.putFragment({ id: "ordered", seq: 0, continued: true,
  data: { data: "Zmlyc3Q=" } }), 0);
assert.equal(runtime.putFragment({ id: "ordered", seq: 1, continued: true,
  data: { data: "c2Vjb25k" } }), 1);
assert.equal(runtime.readByArrival("ordered", 0).seq, 2);
assert.equal(runtime.arrivalSeq("ordered", 1), 0);
assert.equal(runtime.finalSeq("ordered"), 2);
assert.equal(runtime.read("ordered", 2).continued ?? false, false);
assert.equal(runtime.clearData("ordered", 1).seq, 1);
assert.equal(runtime.read("ordered", 1).data.ref, "__tombstone__");
assert.deepEqual(runtime.putMany([
  { id: "batch", data: { data: "YQ==" }, continued: true },
  { id: "batch", data: { data: "Yg==" }, continued: false },
]), [0, 1]);
assert.throws(() => runtime.putMany([
  { id: "batch2", seq: 3, data: { data: "Yw==" }, continued: true },
  { id: "batch2", seq: 3, data: { data: "ZA==" }, continued: true },
]), /already exists/);
assert.equal(runtime.count("batch"), 2);
assert.throws(() => runtime.count("batch2"), /not found/);
runtime.close("ordered");
const snapshot = runtime.exportSnapshot();
const restored = a11.runtime("local");
restored.importSnapshot(snapshot);
assert.equal(restored.count("source"), 2);
assert.equal(restored.closed("reply"), true);
assert.deepEqual(restored.readData("reply", 0), encoded.encode("done"));
assert.equal(restored.finalSeq("ordered"), 2);
assert.equal(restored.readByArrival("ordered", 0).seq, 2);
assert.throws(() => a11.runtime("other").importSnapshot(snapshot), /storage differs/);
restored.dispose();
runtime.dispose();

const grown = a11.runtime();
grown.append("large", new Uint8Array(20 * 1024 * 1024));
assert.equal(grown.readData("large", 0).length, 20 * 1024 * 1024);
grown.dispose();

const waiting = a11.runtime();
const pending = waiting.waitForFragment("stream", 0);
queueMicrotask(() => waiting.append("stream", encoded.encode("arrived")));
assert.equal((await pending).id, "stream");
const cancelled = new AbortController();
const cancelledRead = waiting.waitForFragment("stream", 1, { signal: cancelled.signal });
cancelled.abort();
await assert.rejects(cancelledRead, (error) => error.code === 1);
await assert.rejects(waiting.waitForFragment("stream", 1, { deadline: Date.now() + 5 }),
  (error) => error.code === 4);
const streamed = (async () => {
  const values = [];
  for await (const fragment of waiting.stream("stream")) values.push(fragment);
  return values;
})();
waiting.close("stream");
assert.equal((await streamed).length, 1);
waiting.dispose();

const path = fileURLToPath(new URL("./build/test-snapshot.bin", import.meta.url));
await rm(path, { force: true });
const adapter = fileSnapshotStore(path);
const durable = await a11.openPersistentRuntime(adapter);
await durable.appendDurable("saved", encoded.encode("persisted"));
await durable.closeDurable("saved");
await durable.disposeDurable();
const reopened = await a11.openPersistentRuntime(adapter);
assert.deepEqual(reopened.readData("saved", 0), encoded.encode("persisted"));
await reopened.disposeDurable();
await rm(path, { force: true });

console.log("WASM Flow, wire, framing, stores, references, and actions passed");
