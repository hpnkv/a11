/**
 * Finding `a11-flow`, the binary that *is* the language.
 *
 * Nothing in this extension knows what a flow means. The lexer, parser, resolver,
 * inspector, formatter, completer and the scanner that reads a project's own
 * actions are all `cpp/a11/flow/`, and this extension runs them. So the whole of
 * the "language support" here is a language client plus the wiring below, and a
 * stage added to the grammar is a stage this extension colours, completes and
 * checks with no change at all.
 *
 * The same three places the JetBrains plugin looks, in the same order, so the two
 * behave alike on one machine: the extension's own `bin/<os>-<arch>/`, then the
 * `a11.flow.toolPath` setting, then the `PATH`.
 */

import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import * as vscode from 'vscode';

/** The `<os>-<arch>` directory a bundled binary for this machine would be in. */
function platformDirectory(): string {
  const arch = process.arch === 'arm64' ? 'aarch64' : 'x86_64';
  switch (os.platform()) {
    case 'darwin':
      return `macos-${arch}`;
    case 'win32':
      return `windows-${arch}`;
    default:
      return `linux-${arch}`;
  }
}

function executableName(): string {
  return os.platform() === 'win32' ? 'a11-flow.exe' : 'a11-flow';
}

function runnable(candidate: string): boolean {
  try {
    fs.accessSync(candidate, fs.constants.X_OK);
    return fs.statSync(candidate).isFile();
  } catch {
    return false;
  }
}

/** Where the tool is, or `undefined` if this machine has none. */
export function findFlowTool(context: vscode.ExtensionContext): string | undefined {
  const configured = vscode.workspace
    .getConfiguration('a11')
    .get<string>('flow.toolPath', '')
    .trim();
  if (configured) {
    // An explicit setting is a statement, so it is not silently fallen back
    // from: a path that does not work is worth a complaint of its own.
    return runnable(configured) ? configured : undefined;
  }

  const bundled = path.join(
    context.extensionPath,
    'bin',
    platformDirectory(),
    executableName(),
  );
  if (runnable(bundled)) return bundled;

  // The `PATH`, which is where a developer building the repository has it.
  const dirs = (process.env.PATH ?? '').split(path.delimiter);
  for (const dir of dirs) {
    if (!dir) continue;
    const candidate = path.join(dir, executableName());
    if (runnable(candidate)) return candidate;
  }
  return undefined;
}

/**
 * Say once that there is no binary, and what that costs.
 *
 * A missing tool is a degraded editor rather than an error: a `.flow` is still
 * coloured by the generated TextMate grammar, which knows the language's words
 * and nothing about what they mean. Saying so twice is worse than saying it once,
 * so this is remembered for the workspace.
 */
export async function complainOnce(context: vscode.ExtensionContext): Promise<void> {
  const key = 'a11.flow.toolComplaint';
  if (context.workspaceState.get<boolean>(key)) return;
  await context.workspaceState.update(key, true);
  const choice = await vscode.window.showWarningMessage(
    'No `a11-flow` for this platform, so flows are coloured but not checked.' +
      ' Build one with `cmake --build --preset debug --target a11_flow_tool`,' +
      ' or point `a11.flow.toolPath` at it.',
    'Open settings',
  );
  if (choice === 'Open settings') {
    await vscode.commands.executeCommand(
      'workbench.action.openSettings',
      'a11.flow.toolPath',
    );
  }
}
