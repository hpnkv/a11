/**
 * Every command the manifest advertises is one the extension registers.
 *
 * The bug this exists for: `a11.newChat` was contributed in `package.json` and
 * registered nowhere, and two more were registered *after* an early return taken
 * when no `a11-flow` binary is present. All three showed up in the palette and
 * answered "command 'a11....' not found" — which tells the reader their editor is
 * broken, when the truth is a missing binary they can do something about.
 *
 * A static check, deliberately. The failure only appears on the path where the
 * language is *not* available, which is the one path an integration test that needs
 * `a11-flow` cannot cover. Reading the manifest against the source covers it on
 * every machine, in milliseconds.
 *
 * It is a grep, so it proves the id is registered and not that the registration is
 * reachable. What holds *that* is the shape it enforces: all of them in one call in
 * `activate`, before anything can return.
 */

import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import * as path from 'node:path';
import {test} from 'node:test';
import {fileURLToPath} from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const manifest = JSON.parse(readFileSync(path.join(root, 'package.json'), 'utf8'));

/** Every `.ts` under `src/`, as one string. */
function sources() {
  const files = [
    'src/extension.ts',
    'src/settings.ts',
    'src/suggestions.ts',
    'src/views.ts',
    'src/tool.ts',
    'src/flowClient.ts',
    'src/fragments.ts',
    'src/fragmentSpans.ts',
    'src/tools/index.ts',
    'src/tools/patch.ts',
  ];
  return files.map((one) => readFileSync(path.join(root, one), 'utf8')).join('\n');
}

test('every contributed command is registered', () => {
  const source = sources();
  const declared = manifest.contributes.commands.map((one) => one.command);
  assert.ok(declared.length > 0, 'the manifest contributes no commands');
  for (const id of declared) {
    assert.ok(
      source.includes(`registerCommand('${id}'`),
      `${id} is in the manifest and is never registered`,
    );
  }
});

test('every registered command is either contributed or internal', () => {
  // The other direction, which catches a command nothing can reach. `applySuggestion`
  // is the one exception and is meant to be: it is invoked by a code action rather
  // than typed, so contributing it would put a command in the palette that does
  // nothing without arguments.
  const internal = new Set(['a11.applySuggestion']);
  const declared = new Set(manifest.contributes.commands.map((one) => one.command));
  const registered = [...sources().matchAll(/registerCommand\('([^']+)'/g)].map(
    (found) => found[1],
  );
  assert.ok(registered.length > 0, 'nothing registers a command');
  for (const id of registered) {
    assert.ok(
      declared.has(id) || internal.has(id),
      `${id} is registered and neither contributed nor marked internal`,
    );
  }
});

test('the commands are registered in one place, before anything returns', () => {
  // The shape that makes the first test meaningful: a registration inside a
  // conditional, or after an `await` that may bail, is a command that exists on
  // some machines and not others. Everything goes in the one `push` at the top of
  // `activate`, and the only `return` before it is the one that cannot be reached.
  const extension = readFileSync(path.join(root, 'src/extension.ts'), 'utf8');
  const activate = extension.slice(
    extension.indexOf('export async function activate'),
    extension.indexOf('\n}\n', extension.indexOf('export async function activate')),
  );
  const registered = [...activate.matchAll(/registerCommand\('([^']+)'/g)].length;
  assert.ok(registered >= 3, `expected the commands in activate, found ${registered}`);
  assert.doesNotMatch(
    activate,
    /if \([^)]*\)\s*\{[^}]*registerCommand/s,
    'a command is registered inside a conditional',
  );
});
