/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * The TypeScript half of the presentation contract.
 *
 * `testdata/presentation_events.json` pairs conversations with the blocks they
 * must be drawn as. Python derives them with `a11/sdk/presentation.py` and this
 * derives them with `js/src/sdk/presentation.ts`; both are held to the same
 * file, which is what keeps a terminal and the IDE webview showing the same
 * conversation the same way.
 *
 * Regenerate the fixture from Python (see
 * `a11/sdk/tests/test_presentation_golden.py`) when the derivation changes on
 * purpose, and expect this test to fail until this side agrees.
 */

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

import {
  BlockKind,
  Chunk,
  ChunkMetadata,
  PresentationReducer,
  StatusCode,
  isOk,
  makeInteraction,
  normalizeByShape,
  parseInteraction,
  presentInteraction,
  presentConversation,
  valueOrThrow,
} from '../dist/index.js';

const GOLDEN = fileURLToPath(
  new URL('../../testdata/presentation_events.json', import.meta.url),
);

const golden = JSON.parse(readFileSync(GOLDEN, 'utf8'));

test('raw image chunks normalize and render with their bytes', async () => {
  const interaction = valueOrThrow(makeInteraction({
    role: 'user',
    content: [new Chunk({
      metadata: new ChunkMetadata({ mimetype: 'image/png' }),
      data: new Uint8Array([1, 2, 3]),
    })],
  }));

  const normalized = normalizeByShape(interaction);
  assert.equal(normalized.parts[0].type, 'image');
  assert.equal(normalized.parts[0].data, 'AQID');
  assert.equal(normalized.parts[0].mime_type, 'image/png');

  const turn = await presentInteraction(interaction);
  assert.equal(turn.blocks[0].kind, BlockKind.IMAGE);
  assert.equal(turn.blocks[0].mimeType, 'image/png');
  assert.deepEqual(turn.blocks[0].data, new Uint8Array([1, 2, 3]));
});

test('shape normalization finds images in provider step envelopes', () => {
  const interaction = valueOrThrow(makeInteraction({
    role: 'model',
    content: [new Chunk({
      metadata: new ChunkMetadata({ mimetype: 'application/json' }),
      data: new TextEncoder().encode(JSON.stringify({
        steps: [{
          type: 'model_output',
          content: [{ type: 'image', data: 'AQID', mime_type: 'image/jpeg' }],
        }],
      })),
    })],
  }));

  assert.deepEqual(normalizeByShape(interaction).parts, [{
    type: 'image',
    data: 'AQID',
    mime_type: 'image/jpeg',
  }]);
});

test('interaction status accepts failures and rejects invalid codes', () => {
  const failed = parseInteraction({
    status: { code: StatusCode.DEADLINE_EXCEEDED, message: 'model timed out' },
  });
  assert.equal(isOk(failed), true);
  assert.equal(failed.status.code, StatusCode.DEADLINE_EXCEEDED);

  const invalid = parseInteraction({
    status: { code: 99, message: 'unknown' },
  });
  assert.equal(isOk(invalid), false);
  assert.equal(invalid.code, StatusCode.INVALID_ARGUMENT);
});

/** One interaction from its tagged JSON, as another language would receive it. */
function decode(payload) {
  return valueOrThrow(parseInteraction(JSON.parse(payload)));
}

/** The fixture's portable shape for a block, so the two languages can compare. */
function portable(block) {
  const entry = {
    kind: String(block.kind),
    role: String(block.role),
    text: block.text,
  };
  if (block.id) entry.id = block.id;
  if (block.toolName) entry.tool_name = block.toolName;
  if (block.status) entry.status_code = StatusCode[block.status.code];
  if (block.usage) {
    entry.usage = {
      input_tokens: block.usage.input_tokens ?? null,
      output_tokens: block.usage.output_tokens ?? null,
    };
  }
  return entry;
}

for (const [index, expected] of golden.cases.entries()) {
  test(`presentation golden: ${expected.name}`, async () => {
    const interactions = expected.interactions.map(decode);
    const turns = await presentConversation(interactions);
    const blocks = turns.flatMap((turn) => turn.blocks).map(portable);
    assert.deepEqual(blocks, expected.blocks, `case ${index}: ${expected.name}`);
  });
}

test('deltas coalesce into one block per run', () => {
  const reducer = new PresentationReducer();
  for (const piece of ['Hel', 'lo ', 'world']) reducer.onText(piece);
  reducer.endTurn();

  assert.deepEqual(
    reducer.blocks.map((block) => block.kind),
    [BlockKind.TEXT],
  );
  assert.equal(reducer.blocks[0].text, 'Hello world');
  // Closed, so a client knows not to draw a cursor.
  assert.equal(reducer.blocks[0].partial, false);
});

test('thoughts and text form separate blocks in arrival order', () => {
  const reducer = new PresentationReducer();
  reducer.onThought('I should check');
  reducer.onText('It is noon');
  reducer.onThought('second thought');
  reducer.endTurn();

  assert.deepEqual(
    reducer.blocks.map((block) => block.kind),
    [BlockKind.THOUGHT, BlockKind.TEXT, BlockKind.THOUGHT],
  );
});

test('streamed text is not repeated by the interaction that follows', async () => {
  // The tool-round-trip case, replayed through the live feeder: the same prose
  // arrives twice and must be drawn once, and the log lands one interaction
  // after the call it belongs to.
  const roundTrip = golden.cases.find((entry) => entry.blocks.some((b) => b.kind === 'tool_run'));
  assert.ok(roundTrip, 'the fixture must contain a tool round trip');
  const interactions = roundTrip.interactions.map(decode);

  const reducer = new PresentationReducer();
  for (const interaction of interactions) await reducer.onInteraction(interaction);
  reducer.endTurn();

  const run = reducer.blocks.find((block) => block.kind === BlockKind.TOOL_RUN);
  assert.ok(run, 'the tool run must be drawn');
  assert.ok(run.text.length > 0, 'the run must carry the log from the next interaction');
  // One reducer, two feeders: the same blocks as the pure function derives.
  const direct = (await presentConversation(interactions)).flatMap((turn) => turn.blocks);
  assert.deepEqual(
    reducer.blocks.map((block) => block.kind),
    direct.map((block) => block.kind),
  );
});

test('a sink sees open, append and close', () => {
  const events = [];
  const reducer = new PresentationReducer({
    onBlockOpened: (block) => events.push(['open', block.kind]),
    onBlockAppended: (_block, delta) => events.push(['append', delta]),
    onBlockClosed: (block) => events.push(['close', block.kind]),
  });
  reducer.onText('ab');
  reducer.onText('cd');
  reducer.endTurn();

  assert.deepEqual(events, [
    ['open', 'text'],
    ['append', 'ab'],
    ['append', 'cd'],
    ['close', 'text'],
  ]);
});
