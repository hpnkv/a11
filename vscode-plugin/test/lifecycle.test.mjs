/**
 * Starting and stopping the language server, and the two ways that went wrong.
 *
 * 1. **`stop()` on a client that never came up throws.**
 *    `LanguageClient.stop()` refuses with "Client is not running and can't be
 *    stopped. It's current state is: startFailed" — which is exactly the state a
 *    client is in after a failed start. So `restart()` turned one failure into a
 *    second, more confusing one, and the second is what the reader saw.
 * 2. **A failed start escaped `activate()`.** Which took the chat and the tools
 *    down with the language, over a binary that would not run, and left a dead
 *    client assigned for the next call to trip over.
 *
 * Checked against the source rather than a running editor, because both failures
 * are about *state the editor will not enter on demand*: there is no way to ask a
 * real VSCode for a client that failed to start. What can be checked is that the
 * code guards the state and catches the throw, and that is what these do.
 */

import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import * as path from 'node:path';
import {test} from 'node:test';
import {fileURLToPath} from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const read = (one) => readFileSync(path.join(root, one), 'utf8');

test('stop() only stops a client that is up', () => {
  const source = read('src/flowClient.ts');
  const stop = source.slice(source.indexOf('async stop('), source.indexOf('get running('));
  // The guard: `client.stop()` is reached only for a running or starting client.
  // Only `Running` is stoppable. `Starting` is not — a client mid-handshake
  // refuses the same way a failed one does — so it must fall to `dispose`.
  assert.match(
    stop,
    /State\.Running\s*\)\s*\{\s*await client\.stop\(\)/,
    'stop() stops a client that is not Running',
  );
  assert.match(stop, /client\.dispose\(\)/, 'a non-running client is not disposed');
  assert.doesNotMatch(
    stop,
    /State\.Starting[^}]*client\.stop\(\)/,
    'stop() still tries to stop a starting client',
  );
  // And it cannot throw out, whatever the client does.
  assert.match(stop, /catch/, 'stop() can still throw');
  // The client is forgotten before anything that might throw, so a second call
  // does not see the same broken one again.
  assert.ok(
    stop.indexOf('this.client = undefined') < stop.indexOf('try'),
    'stop() keeps the client if it throws',
  );
});

test('a failed start is reported rather than thrown', () => {
  const source = read('src/extension.ts');
  const start = source.slice(
    source.indexOf('async start(): Promise<boolean>'),
    source.indexOf('async restart('),
  );
  assert.match(start, /try\s*\{[\s\S]*await server\.start\(\)/, 'start() is not guarded');
  assert.match(start, /catch/, 'a failed start still escapes');
  // What the reader is told, and that the dead server is not kept.
  assert.match(start, /showErrorMessage/, 'a failed start says nothing');
  assert.match(start, /this\.server = undefined/, 'a failed server stays assigned');
  assert.match(start, /return false/, 'a failed start does not report failure');
});

test('activate() cannot be brought down by the language server', () => {
  const source = read('src/extension.ts');
  const activate = source.slice(
    source.indexOf('export async function activate'),
    source.indexOf('\n}\n', source.indexOf('export async function activate')),
  );
  // The views and every command are registered before the language is even looked
  // for, and the one call that could fail is last and returns a boolean.
  assert.ok(
    activate.indexOf('registerWebviewViewProvider') < activate.indexOf('flow.start()'),
    'the views wait on the language server',
  );
  assert.ok(
    activate.lastIndexOf('registerCommand') < activate.indexOf('flow.start()'),
    'a command waits on the language server',
  );
});

test('restart() looks for the tool again', () => {
  // The way in after building the binary: no window reload needed. `restart` goes
  // through `start`, which is what calls `findFlowTool`.
  const source = read('src/extension.ts');
  const restart = source.slice(
    source.indexOf('async restart('),
    source.indexOf('async rescan('),
  );
  assert.match(restart, /this\.start\(\)/, 'restart() does not re-run start()');
  assert.match(read('src/extension.ts'), /findFlowTool\(this\.context\)/);
});

test('a command title does not repeat its category', () => {
  // VSCode renders `category: title`, so a title carrying the category too reads
  // "A11: A11: Restart the Flow language server" in the palette.
  const manifest = JSON.parse(read('package.json'));
  for (const one of manifest.contributes.commands) {
    const category = one.category ?? '';
    assert.doesNotMatch(
      one.title,
      /^A11:/,
      `${one.command}: the title repeats the "${category}" category`,
    );
    assert.ok(one.title.length > 0, `${one.command} has no title`);
  }
});

test('the page is served with a policy in force before anything it governs', () => {
  // The editor warns about a webview it cannot find a CSP in, and it is right to:
  // this page runs a bundle and opens a WebSocket. Checked against the served HTML
  // rather than the source, because the transformation is what has to be right —
  // `<head>` is what it hooks, and a template that renamed it would break silently.
  const template = read('dist/index.html');
  assert.match(template, /<head>/, 'index.html has no <head> for the policy to go in');
  assert.match(template, /%%THEME_VARS%%/, 'index.html lost its theme seam');

  const source = read('src/views.ts');
  assert.match(
    source,
    /\.replace\(\s*'<head>'/,
    'the policy is not inserted at the top of <head>',
  );
  // The directives that matter: a bundle runs only under the nonce, and nothing
  // loads by default.
  assert.match(source, /default-src 'none'/);
  assert.match(source, /script-src 'nonce-/);
  // The chat opens a WebSocket to the gateway, so that has to be allowed.
  assert.match(source, /connect-src[^"]*wss?:/);
});
