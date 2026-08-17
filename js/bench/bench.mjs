// Copyright 2026 The A11 Authors.

// A11's client-side benchmarks.
//
// The Python suite measures the runtime an A11 *service* is built on. This one
// measures the runtime an A11 *client* is built on, and the two are different
// programs with different bottlenecks: the browser has no fibers, no GIL and no
// native extension, so every number here is JavaScript all the way down.
//
// It emits the same record shape as `bench/harness.py`, so
// `python -m bench --baseline` can diff a JS run the same way it diffs a Python
// one, and a table can put "encode a chunk" in Python beside "encode a chunk" in
// the browser's language.
//
//   node bench/bench.mjs
//   node bench/bench.mjs --json ../bench/runs/js.json --scale 0.2
//   node bench/bench.mjs --suite wire
//
// Run `npm run build` first; this imports from `dist/`.

import { writeFileSync } from 'node:fs';
import { argv, memoryUsage, version as nodeVersion } from 'node:process';

import {
  AsyncNode,
  Chunk,
  ChunkMetadata,
  LocalChunkStore,
  NodeFragment,
  toChunk,
  fromChunk,
  WireMessage,
  InProcessWireStream,
  isOk,
} from '../dist/index.js';

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

const results = [];

function record(suite, name, metrics, params = {}, note = '') {
  results.push({ suite, name, metrics, params, note });
  const rendered = Object.entries(params)
    .map(([key, value]) => `${key}=${value}`)
    .join(',');
  const label = rendered ? `${suite}/${name}[${rendered}]` : `${suite}/${name}`;
  console.log(`  ok   ${label}`);
}

/** Rate over a whole batch. Use where one operation is too fast to time. */
function throughputSync(operation, { iterations, warmup = 0, perOpItems = 1, perOpBytes = 0 }) {
  for (let index = 0; index < warmup; index += 1) operation(index);
  const started = process.hrtime.bigint();
  for (let index = 0; index < iterations; index += 1) operation(warmup + index);
  return rateMetrics(process.hrtime.bigint() - started, iterations, perOpItems, perOpBytes);
}

async function throughput(operation, { iterations, warmup = 0, perOpItems = 1, perOpBytes = 0 }) {
  for (let index = 0; index < warmup; index += 1) await operation(index);
  const started = process.hrtime.bigint();
  for (let index = 0; index < iterations; index += 1) await operation(warmup + index);
  return rateMetrics(process.hrtime.bigint() - started, iterations, perOpItems, perOpBytes);
}

function rateMetrics(elapsedNs, iterations, perOpItems, perOpBytes) {
  const seconds = Number(elapsedNs) / 1e9 || 1e-9;
  const metrics = {
    ops_per_s: iterations / seconds,
    ns_per_op: Number(elapsedNs) / iterations,
    elapsed_s: seconds,
  };
  if (perOpItems !== 1) metrics.items_per_s = (iterations * perOpItems) / seconds;
  if (perOpBytes) metrics.mib_per_s = (iterations * perOpBytes) / seconds / 1048576;
  return metrics;
}

/** Per-call timing, for anything above a microsecond. */
async function latency(operation, { iterations, warmup = 0 }) {
  for (let index = 0; index < warmup; index += 1) await operation(index);
  const samples = [];
  for (let index = 0; index < iterations; index += 1) {
    const started = process.hrtime.bigint();
    await operation(warmup + index);
    samples.push(Number(process.hrtime.bigint() - started));
  }
  return percentiles(samples);
}

function percentiles(samplesNs) {
  const ordered = [...samplesNs].sort((a, b) => a - b);
  const at = (fraction) =>
    ordered[Math.min(ordered.length - 1, Math.max(0, Math.ceil(fraction * ordered.length) - 1))] / 1000;
  const total = ordered.reduce((sum, value) => sum + value, 0);
  return {
    p50_us: at(0.5),
    p90_us: at(0.9),
    p99_us: at(0.99),
    max_us: ordered[ordered.length - 1] / 1000,
    mean_us: total / ordered.length / 1000,
    ops_per_s: ordered.length / (total / 1e9),
  };
}

/**
 * Marginal heap bytes per object, from a fit across stages.
 *
 * Same reasoning as the Python probe: one before/after delta is dominated by
 * whatever the allocator had already reserved, so build the population in
 * stages and take the slope. `global.gc` is only there under `--expose-gc`;
 * without it the fit is noisier but still a fit.
 */
async function memorySlope(make, { counts }) {
  const held = [];
  let total = 0;
  const points = [];
  for (const count of counts) {
    held.push(await make(count));
    total += count;
    if (global.gc) global.gc();
    points.push([total, memoryUsage().heapUsed]);
  }
  const usable = points.length > 2 ? points.slice(1) : points;
  const meanX = usable.reduce((sum, [x]) => sum + x, 0) / usable.length;
  const meanY = usable.reduce((sum, [, y]) => sum + y, 0) / usable.length;
  const covariance = usable.reduce((sum, [x, y]) => sum + (x - meanX) * (y - meanY), 0);
  const variance = usable.reduce((sum, [x]) => sum + (x - meanX) ** 2, 0);
  held.length = 0;
  const trail = points.map(([x, y]) => `${x}:${Math.round(y / 1024)}K`).join(' ');
  return [variance ? Math.max(covariance / variance, 0) : 0, `${usable.length}-point fit over ${trail}`];
}

function unwrap(value) {
  if (!isOk(value)) throw new Error(`not ok: ${JSON.stringify(value)}`);
  return value;
}

// ---------------------------------------------------------------------------
// Suites
// ---------------------------------------------------------------------------

const SIZES = [64, 1024, 65536];

function human(size) {
  if (size >= 1048576) return `${size / 1048576}M`;
  if (size >= 1024) return `${size / 1024}K`;
  return `${size}B`;
}

async function dataSuite(scale) {
  for (const size of SIZES) {
    const value = { id: 'bench', body: 'x'.repeat(Math.max(size - 24, 1)) };
    const iterations = Math.max(Math.round((size <= 1024 ? 50000 : 4000) * scale), 20);
    for (const [label, mimetype] of [
      ['json', 'application/json'],
      ['msgpack', 'application/x-msgpack'],
    ]) {
      const chunk = unwrap(await toChunk(value, mimetype));
      const bytes = chunk.data.length;
      record(
        'data',
        'to_chunk',
        await throughput(async () => unwrap(await toChunk(value, mimetype)), {
          iterations,
          warmup: Math.round(iterations / 10),
          perOpBytes: bytes,
        }),
        { repr: label, size: human(size) },
      );
      record(
        'data',
        'from_chunk',
        await throughput(async () => unwrap(await fromChunk(chunk)), {
          iterations,
          warmup: Math.round(iterations / 10),
          perOpBytes: bytes,
        }),
        { repr: label, size: human(size) },
      );
    }
  }

  for (const fragments of [1, 8, 64]) {
    const message = new WireMessage({
      nodeFragments: Array.from(
        { length: fragments },
        (_unused, index) =>
          new NodeFragment({ id: 'bench', data: new Chunk({ data: new Uint8Array(256) }), seq: index }),
      ),
    });
    const encoded = unwrap(message.toMsgpack());
    const iterations = Math.max(Math.round((100000 / fragments) * scale), 50);
    record(
      'data',
      'wire_to_msgpack',
      throughputSync(() => unwrap(message.toMsgpack()), {
        iterations,
        warmup: Math.round(iterations / 10),
        perOpItems: fragments,
        perOpBytes: encoded.length,
      }),
      { frags: fragments, size: '256B' },
    );
    record(
      'data',
      'wire_from_msgpack',
      throughputSync(() => unwrap(WireMessage.fromMsgpack(encoded)), {
        iterations,
        warmup: Math.round(iterations / 10),
        perOpItems: fragments,
        perOpBytes: encoded.length,
      }),
      { frags: fragments, size: '256B' },
    );
  }

  const [chunkBytes, chunkTrail] = await memorySlope(
    (count) => Array.from({ length: count }, () => new Chunk({ data: new Uint8Array(64) })),
    { counts: Array(6).fill(Math.max(Math.round(50000 * scale), 1000)) },
  );
  record('data', 'chunk_resident', { bytes_each: chunkBytes }, { payload: '64B' }, chunkTrail);
}

async function storesSuite(scale) {
  const payload = new Uint8Array(256);
  const iterations = Math.max(Math.round(20000 * scale), 100);

  const store = unwrap(LocalChunkStore.create('bench-put'));
  record(
    'stores',
    'put',
    await latency(
      (index) =>
        store.put(new NodeFragment({ data: new Chunk({ data: payload }), seq: index, continued: true })),
      { iterations, warmup: Math.round(iterations / 10) },
    ),
    { backend: 'local' },
  );

  for (const batch of [8, 64, 256]) {
    const batched = unwrap(LocalChunkStore.create(`bench-many-${batch}`));
    const calls = Math.max(Math.round((4000 / batch) * scale), 10);
    record(
      'stores',
      'put_many',
      await throughput(
        (index) =>
          batched.putMany(
            Array.from(
              { length: batch },
              (_unused, offset) =>
                new NodeFragment({
                  data: new Chunk({ data: payload }),
                  seq: index * batch + offset,
                  continued: true,
                }),
            ),
          ),
        { iterations: calls, warmup: 2, perOpItems: batch, perOpBytes: batch * 256 },
      ),
      { backend: 'local', batch },
    );
  }

  const filled = unwrap(LocalChunkStore.create('bench-read'));
  const count = Math.max(Math.round(20000 * scale), 200);
  for (let start = 0; start < count; start += 500) {
    const size = Math.min(500, count - start);
    await filled.putMany(
      Array.from(
        { length: size },
        (_unused, offset) =>
          new NodeFragment({
            data: new Chunk({ data: payload }),
            seq: start + offset,
            continued: start + offset < count - 1,
          }),
      ),
    );
  }
  for (const limit of [1, 64]) {
    const cursor = unwrap(LocalChunkStore.create(`bench-drain-${limit}`));
    for (let start = 0; start < count; start += 500) {
      const size = Math.min(500, count - start);
      await cursor.putMany(
        Array.from(
          { length: size },
          (_unused, offset) =>
            new NodeFragment({
              data: new Chunk({ data: payload }),
              seq: start + offset,
              continued: start + offset < count - 1,
            }),
        ),
      );
    }
    const started = process.hrtime.bigint();
    let drained = 0;
    for (;;) {
      const batch = await cursor.next(Date.now() + 30000, limit);
      const values = isOk(batch) ? batch : [];
      drained += values.filter((fragment) => fragment !== null).length;
      if (values.length && values[values.length - 1] === null) break;
      if (!values.length) break;
    }
    const elapsed = Number(process.hrtime.bigint() - started) / 1e9;
    record(
      'stores',
      limit === 1 ? 'read_one_at_a_time' : 'read_batched',
      {
        ops_per_s: drained / elapsed,
        items_per_s: drained / elapsed,
        p50_us: (elapsed / drained) * 1e6,
        elapsed_s: elapsed,
      },
      { backend: 'local', limit },
    );
  }
}

let residentCounter = 0;

async function nodesSuite(scale) {
  const iterations = Math.max(Math.round(10000 * scale), 100);
  record(
    'nodes',
    'node_create',
    await throughput(async (index) => unwrap(await AsyncNode.create(`bench-node-${index}`)), {
      iterations,
      warmup: Math.round(iterations / 10),
    }),
    {},
  );

  const chunk = new Chunk({
    data: new TextEncoder().encode('{"seq":0,"text":"a token"}'),
    metadata: new ChunkMetadata({ mimetype: 'application/json' }),
  });
  const node = unwrap(await AsyncNode.create('bench-write'));
  record(
    'nodes',
    'put',
    await latency(() => node.putChunk(chunk), { iterations, warmup: 100 }),
    { path: 'chunk' },
  );

  const filled = unwrap(await AsyncNode.create('bench-read'));
  const count = Math.max(Math.round(20000 * scale), 200);
  for (let index = 0; index < count - 1; index += 1) await filled.putChunk(chunk);
  await filled.putChunk(chunk, null, true);
  const started = process.hrtime.bigint();
  let seen = 0;
  for (;;) {
    const fragment = await filled.nextFragment();
    if (!isOk(fragment) || fragment === null) break;
    seen += 1;
  }
  const elapsed = Number(process.hrtime.bigint() - started) / 1e9;
  record(
    'nodes',
    'drain',
    { ops_per_s: seen / elapsed, items_per_s: seen / elapsed, elapsed_s: elapsed },
    { path: 'chunk' },
    `${seen} values`,
  );

  const [nodeBytes, trail] = await memorySlope(
    async (n) => {
      const made = [];
      for (let index = 0; index < n; index += 1) {
        made.push(unwrap(await AsyncNode.create(`resident-${residentCounter++}`)));
      }
      return made;
    },
    { counts: Array(6).fill(Math.max(Math.round(2000 * scale), 100)) },
  );
  record('nodes', 'node_resident', { bytes_each: nodeBytes }, {}, trail);
}

async function wireSuite(scale) {
  const pair = unwrap(InProcessWireStream.createPair());
  const [client, server] = pair;
  const received = [];
  let resolveNext = null;
  await server.accept(
    async (message) => {
      if (resolveNext) {
        resolveNext(message);
        resolveNext = null;
      } else {
        received.push(message);
      }
    },
    async () => {},
  );
  await client.start(
    async () => {},
    async () => {},
  );

  const nextMessage = () =>
    received.length ? Promise.resolve(received.shift()) : new Promise((resolve) => (resolveNext = resolve));

  for (const size of [64, 4096, 65536]) {
    const message = new WireMessage({
      nodeFragments: [new NodeFragment({ id: 'bench', data: new Chunk({ data: new Uint8Array(size) }) })],
    });
    const iterations = Math.max(Math.round(2000 * scale), 50);
    const metrics = await latency(
      async () => {
        client.send(message);
        await nextMessage();
      },
      { iterations, warmup: 20 },
    );
    metrics.mib_per_s = (metrics.ops_per_s * size) / 1048576;
    record('wire', 'one_way_delivery', metrics, { transport: 'in-process', size: human(size) });
  }
  client.halfClose();
  await client.drainOutgoingMessages();
}

const SUITES = {
  data: dataSuite,
  stores: storesSuite,
  nodes: nodesSuite,
  wire: wireSuite,
};

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

function option(name, fallback) {
  const index = argv.indexOf(`--${name}`);
  return index === -1 ? fallback : argv[index + 1];
}

const scale = Number(option('scale', '1'));
const only = argv.filter((value, index) => argv[index - 1] === '--suite');
const chosen = only.length ? only : Object.keys(SUITES);

console.log(`a11 bench (js): ${chosen.join(', ')}, scale=${scale}`);
console.log(`  node: ${nodeVersion}`);
console.log(`  gc exposed: ${Boolean(global.gc)}`);

for (const name of chosen) {
  const suite = SUITES[name];
  if (!suite) {
    console.error(`unknown suite ${name}`);
    process.exitCode = 1;
    continue;
  }
  try {
    await suite(scale);
  } catch (failure) {
    console.error(`  FAIL ${name}: ${failure?.stack ?? failure}`);
  }
}

const table = results.map((result) => ({
  benchmark: `${result.name} ${Object.entries(result.params).map(([k, v]) => `${k}=${v}`).join(' ')}`,
  'ops/s': result.metrics.ops_per_s?.toFixed(0) ?? '-',
  'p50 us': result.metrics.p50_us?.toFixed(2) ?? '-',
  'MiB/s': result.metrics.mib_per_s?.toFixed(1) ?? '-',
  'bytes ea': result.metrics.bytes_each?.toFixed(0) ?? '-',
}));
console.table(table);

const jsonPath = option('json', null);
if (jsonPath) {
  writeFileSync(
    jsonPath,
    `${JSON.stringify(
      {
        environment: { runtime: `node ${nodeVersion}`, gc_exposed: Boolean(global.gc) },
        recorded_at: Date.now() / 1000,
        results,
      },
      null,
      2,
    )}\n`,
  );
  console.log(`wrote ${jsonPath}`);
}
