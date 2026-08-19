/**
 * The integration test: does the extension actually attach to a real editor?
 *
 * Separate from `npm test` and not run by it, because it downloads a VSCode build
 * the first time and needs a display. What it proves is the one thing the unit
 * tests cannot: that the manifest is right, that activation succeeds, and that a
 * bad `.flow` comes back with diagnostics from a language server this extension
 * really started.
 *
 *   npm run build && npm run test:integration
 *
 * It skips itself, loudly, when there is no `a11-flow` on the machine: without one
 * the extension is *meant* to degrade to colouring, so a failure here would be the
 * test reporting the absence of a binary rather than a fault in the extension.
 */

import {downloadAndUnzipVSCode, runTests} from '@vscode/test-electron';
import {execFileSync} from 'node:child_process';
import {existsSync} from 'node:fs';
import * as path from 'node:path';
import {fileURLToPath} from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));

function haveTool() {
  const configured = process.env.A11_FLOW_TOOL;
  try {
    execFileSync(configured || 'a11-flow', ['version'], {stdio: 'ignore'});
    return true;
  } catch {
    return false;
  }
}

if (!haveTool()) {
  console.log(
    'Skipping: no `a11-flow` on this machine, and without one the extension is' +
      ' meant to colour flows and check nothing. Build it with' +
      ' `cmake --build --preset debug --target a11_flow_tool` and put it on the' +
      ' PATH, or set A11_FLOW_TOOL.',
  );
  process.exit(0);
}

/**
 * The editor to run, with its executable found rather than assumed.
 *
 * `@vscode/test-electron` builds the path with the name the Electron shell used to
 * have; a current build ships `Contents/MacOS/Code`, so the default path does not
 * exist and the run dies with `spawn … ENOENT`. Looking for both is a two-line fix
 * for a mismatch between two things this repository does not control.
 */
async function editor() {
  const given = await downloadAndUnzipVSCode();
  if (existsSync(given)) return given;
  for (const name of ['Code', 'Electron', 'code']) {
    const candidate = path.join(path.dirname(given), name);
    if (existsSync(candidate)) return candidate;
  }
  return given;
}

await runTests({
  vscodeExecutablePath: await editor(),
  extensionDevelopmentPath: path.resolve(here, '..'),
  extensionTestsPath: path.resolve(here, 'integration/index.cjs'),
  launchArgs: [
    path.resolve(here, 'fixtures'),
    '--disable-extensions',
    // A profile of its own, so a run neither reads nor disturbs the editor this
    // was launched from.
    '--user-data-dir',
    path.resolve(here, '..', '.vscode-test', 'user-data'),
  ],
});
