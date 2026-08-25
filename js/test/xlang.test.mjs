// Copyright 2026 The A11 Authors.

// A folded frame is the one thing a non-TypeScript peer now receives that it did
// not before, so it is checked against the real Python runtime rather than
// against another copy of this library. Skips, with a reason, when that runtime
// cannot be imported -- the native extension is installed separately from this
// source tree and is routinely built for a different interpreter.

import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import path from 'node:path';
import test from 'node:test';

import { driveEcho } from './xlang_client.mjs';

// Absolute, and the peer runs from the repository root: `import a11` resolves
// against the source tree there, and a relative interpreter path is resolved
// against this process's directory rather than the child's.
const REPO = path.resolve(import.meta.dirname, '../..');
const PYTHON = path.join(REPO, '.venv/bin/python');
const PEER = path.join(import.meta.dirname, 'xlang_peer.py');
const COUNT = 32;

const probe = spawnSync(PYTHON, ['-c', 'import a11'], { cwd: REPO });
const available = probe.status === 0;

async function runPeer(transport, body) {
  const peer = spawn(PYTHON, [PEER, transport], {
    cwd: REPO,
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  let stderr = '';
  let stdout = '';
  const lines = [];
  peer.stderr.on('data', (chunk) => { stderr += chunk.toString(); });
  peer.stdout.on('data', (chunk) => {
    stdout += chunk.toString();
    let newline = stdout.indexOf('\n');
    while (newline !== -1) {
      lines.push(JSON.parse(stdout.slice(0, newline)));
      stdout = stdout.slice(newline + 1);
      newline = stdout.indexOf('\n');
    }
  });
  const nextLine = (timeoutMs, what) => new Promise((resolve, reject) => {
    const deadline = Date.now() + timeoutMs;
    const poll = setInterval(() => {
      if (lines.length > 0) {
        clearInterval(poll);
        resolve(lines.shift());
      } else if (Date.now() > deadline) {
        clearInterval(poll);
        reject(new Error(`peer did not report ${what}; stderr: ${stderr}`));
      }
    }, 10);
  });
  try {
    const { port } = await nextLine(30000, 'a port');
    await body(port);
    peer.stdin.end('\n');
    const { observed } = await nextLine(10000, 'its frame counts');
    return observed;
  } finally {
    peer.stdin.end();
    peer.kill('SIGTERM');
  }
}

for (const transport of ['websocket', 'sse']) {
  test(`a folded frame round-trips through the Python runtime over ${transport}`, {
    skip: available ? false : 'the Python a11 runtime could not be imported',
  }, async () => {
    let payloads;
    const observed = await runPeer(transport, async (port) => {
      payloads = await driveEcho(transport, port, COUNT);
      assert.ok(Array.isArray(payloads), `drive failed: ${JSON.stringify(payloads)}`);
      assert.equal(payloads.length, COUNT, 'every fragment came back');
      // Order is the fragment sequence, not the frame boundaries: the fold is
      // allowed to change how many frames carry them and not what order they
      // are in.
      assert.deepEqual(
        payloads.map((entry) => entry.text),
        Array.from({ length: COUNT }, (_, index) => `{"i":${index}}`),
      );
      assert.ok(
        payloads.every((entry) => entry.mimetype === 'application/json'),
        'the peer read each fragment with its mimetype intact',
      );
    });
    // The point of the whole test: the peer really did receive fewer frames
    // than were sent, so a folded frame is what it parsed. Without this the
    // test would pass just as well with the fold removed.
    assert.equal(observed.fragments, COUNT);
    assert.ok(
      observed.frames < COUNT,
      `the peer saw ${observed.frames} frames for ${COUNT} sends, so none folded`,
    );
  });
}
