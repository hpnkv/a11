// Action.log: what it writes, where it goes, and when it refuses.
//
// The lifecycle is the same as the C++ layer's and is pinned there; what is worth
// pinning here is that the TypeScript port agrees about the port name, the
// metadata and the refusals, so a JS peer and a C++ peer read one contract.

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';

import {
  ACTION_LOG_OUTPUT,
  Action,
  ActionPortSchema,
  ActionSchema,
  DEFAULT_LOG_LEVEL,
  LOG_CHANNEL_ATTRIBUTE,
  LOG_FILE_ATTRIBUTE,
  LOG_INTERNAL_ATTRIBUTE,
  LOG_INTERNAL_FALSE,
  LOG_INTERNAL_TRUE,
  LOG_LINENO_ATTRIBUTE,
  LOG_LEVELS,
  LOG_LEVEL_ATTRIBUTE,
  NodeMap,
  TEXT_MIMETYPE,
  isOk,
  isStatusChunk,
  logRecordFromChunk,
  parseLogLevel,
  setActionLogSink,
} from '../dist/index.js';

function schema(name = 'quiet') {
  return new ActionSchema({
    name,
    outputs: new Map([['out', new ActionPortSchema({ name: 'out', type: 'text/plain' })]]),
  });
}

/// Runs `body` as the action's handler and waits for it.
async function runWith(action, body) {
  action.bindHandler(async (running) => body(running));
  const started = await action.run();
  assert.ok(isOk(started), JSON.stringify(started));
  await action.wait();
}

/// Collects what the process sink is handed, and puts the sink back after.
function capture() {
  const records = [];
  setActionLogSink((record) => records.push(record));
  return {
    records,
    release() {
      setActionLogSink(null);
    },
  };
}

test('the log port cannot be declared in a schema', () => {
  const invalid = new ActionSchema({
    name: 'declared',
    outputs: new Map([
      [ACTION_LOG_OUTPUT, new ActionPortSchema({ name: ACTION_LOG_OUTPUT, type: 'text/plain' })],
    ]),
  });
  const status = invalid.validate();
  assert.ok(!isOk(status));
  assert.match(status.message, /is reserved/);
});

test('the log port is in no schema and no action message', () => {
  const action = Action.create(schema(), { id: 'hidden' });
  assert.ok(isOk(action));
  assert.ok(!action.getSchema().outputs.has(ACTION_LOG_OUTPUT));
  assert.deepEqual(
    action.getActionMessage().outputs.map((port) => port.name),
    ['out'],
  );
});

test('a string is text and reaches the sink once', async () => {
  const sink = capture();
  try {
    const action = Action.create(schema(), { id: 'text' });
    await runWith(action, async (running) => {
      await running.log('a line', { channel: 'work' });
    });
    assert.equal(sink.records.length, 1);
    assert.equal(sink.records[0].mimetype, TEXT_MIMETYPE);
    assert.equal(new TextDecoder().decode(sink.records[0].data), 'a line');
    assert.equal(sink.records[0].level, DEFAULT_LOG_LEVEL);
    assert.equal(sink.records[0].channel, 'work');
    assert.equal(sink.records[0].actionName, 'quiet');
    assert.ok(sink.records[0].timestamp instanceof Date);
  } finally {
    sink.release();
  }
});

test('logf fills %s and logfWith carries the options', async () => {
  const sink = capture();
  try {
    const action = Action.create(schema(), { id: 'formatted' });
    await runWith(action, async (running) => {
      await running.logf('read %s of %s pages', 3, 12);
      await running.logfWith({ level: 'warning' }, 'retrying %s', 'a-url');
    });
    const texts = sink.records.map((record) => new TextDecoder().decode(record.data));
    assert.deepEqual(texts, ['read 3 of 12 pages', 'retrying a-url']);
    assert.equal(sink.records[1].level, 'warning');
  } finally {
    sink.release();
  }
});

test('only a running action may log, and only at a known level', async () => {
  const action = Action.create(schema(), { id: 'early' });
  const early = await action.log('too soon');
  assert.ok(!isOk(early));

  const sink = capture();
  try {
    const running = Action.create(schema(), { id: 'bad-level' });
    let refused = null;
    await runWith(running, async (self) => {
      refused = await self.log('noisy', { level: 'verbose' });
    });
    assert.ok(refused !== null && !isOk(refused));
    assert.equal(sink.records.length, 0);
  } finally {
    sink.release();
  }
});

test('a claimed log port carries the chunks and closes itself', async () => {
  const sink = capture();
  try {
    const action = Action.create(schema(), { id: 'claimed', nodeMap: new NodeMap() });
    const logs = await action.getLogNode();
    assert.ok(isOk(logs));

    await runWith(action, async (running) => {
      await running.log('first', { channel: 'work' });
      await running.log('second', { level: 'warning' });
    });

    // A claimed port owns presentation, so the sink is not also told.
    assert.equal(sink.records.length, 0);

    const seen = [];
    for (;;) {
      const chunk = await logs.nextChunk();
      assert.ok(isOk(chunk));
      if (chunk === null) break; // Closed with the other outputs; nobody did it.
      if (isStatusChunk(chunk)) continue;
      seen.push(chunk);
    }
    assert.deepEqual(
      seen.map((chunk) => new TextDecoder().decode(chunk.data)),
      ['first', 'second'],
    );
    const first = logRecordFromChunk(seen[0]);
    assert.equal(first.channel, 'work');
    assert.equal(first.level, DEFAULT_LOG_LEVEL);
    assert.ok(first.timestamp instanceof Date);
    assert.equal(
      new TextDecoder().decode(seen[0].metadata.attributes.get(LOG_CHANNEL_ATTRIBUTE)),
      'work',
    );
    assert.equal(
      new TextDecoder().decode(seen[1].metadata.attributes.get(LOG_LEVEL_ATTRIBUTE)),
      'warning',
    );
  } finally {
    sink.release();
  }
});

test('the level names are the five every language agrees on', () => {
  assert.deepEqual([...LOG_LEVELS], ['debug', 'info', 'warning', 'error', 'critical']);
  assert.equal(parseLogLevel('warn'), 'warning');
  assert.equal(parseLogLevel('FATAL'), 'critical');
  assert.equal(parseLogLevel(''), DEFAULT_LOG_LEVEL);
  assert.equal(parseLogLevel('chatty'), null);
});

const fixture = JSON.parse(
  readFileSync(
    fileURLToPath(new URL('../../testdata/log_chunk.json', import.meta.url)),
    'utf8',
  ),
);

test('the reserved port and its metadata match the fixture', () => {
  // The log port and its attribute names are a cross-language contract: a peer in
  // another language reads these chunks, so the words have to be the same words.
  assert.equal(fixture.port, ACTION_LOG_OUTPUT);
  assert.equal(fixture.attributes.level, LOG_LEVEL_ATTRIBUTE);
  assert.equal(fixture.attributes.internal, LOG_INTERNAL_ATTRIBUTE);
  assert.equal(fixture.attributes.channel, LOG_CHANNEL_ATTRIBUTE);
  assert.equal(fixture.attributes.file, LOG_FILE_ATTRIBUTE);
  assert.equal(fixture.attributes.lineno, LOG_LINENO_ATTRIBUTE);
  assert.equal(fixture.internal_true, LOG_INTERNAL_TRUE);
  assert.equal(fixture.internal_false, LOG_INTERNAL_FALSE);
  assert.deepEqual(fixture.levels, [...LOG_LEVELS]);
  assert.equal(fixture.default_level, DEFAULT_LOG_LEVEL);
  for (const [written, meant] of Object.entries(fixture.level_aliases)) {
    assert.equal(parseLogLevel(written), meant);
  }
});
