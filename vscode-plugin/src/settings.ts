/**
 * What the page needs to reach the gateway and a provider.
 *
 * The extension holds the API key; the gateway holds no provider configuration of
 * its own. That is the same arrangement the JetBrains plugin has, and the same
 * reason: the key belongs to the person at the editor, so it lives in the editor's
 * own secret store rather than in a service anybody on the machine can reach.
 *
 * `context.secrets` is this editor's `PasswordSafe`: backed by the OS keychain, and
 * never in `settings.json` where it would end up in a dotfiles repository.
 */

import * as vscode from 'vscode';
import {listDescriptors} from './tools/index.js';

/** The key under which the provider key is kept. */
const API_KEY = 'a11.apiKey';

export interface A11Config {
  url: string;
  provider: string;
  model: string;
  apiKey: string;
  baseUrl: string;
  allowedTools: string[];
  /** What the running editor is, which the page shows and a flow may read. */
  ide: string;
  ideVersion: string;
  projectName: string;
  projectPath: string | null;
}

/**
 * A gateway URL, completed.
 *
 * A bare `host:port` is accepted and finished, as the JetBrains setting is: a
 * person who typed what they see in `a11 gateway`'s own output should not have to
 * know the scheme or the path.
 */
export function completeUrl(given: string): string {
  const trimmed = given.trim();
  if (!trimmed) return 'ws://127.0.0.1:8011/a11';
  const withScheme = /^wss?:\/\//.test(trimmed) ? trimmed : `ws://${trimmed}`;
  // A URL with no path at all wants the gateway's, which is `/a11`.
  try {
    const parsed = new URL(withScheme);
    if (parsed.pathname === '/' || parsed.pathname === '') parsed.pathname = '/a11';
    return parsed.toString();
  } catch {
    return withScheme;
  }
}

export async function readConfig(
  context: vscode.ExtensionContext,
): Promise<A11Config> {
  const settings = vscode.workspace.getConfiguration('a11');
  const folder = vscode.workspace.workspaceFolders?.[0];
  return {
    url: completeUrl(settings.get<string>('gatewayUrl', '')),
    provider: settings.get<string>('provider', 'claude'),
    model: settings.get<string>('model', ''),
    apiKey: (await context.secrets.get(API_KEY)) ?? '',
    baseUrl: settings.get<string>('baseUrl', ''),
    // This editor's tools, plus the patterns admitting the gateway's own. The
    // gateway adds a `shell_*` tool for every registered name a pattern matches,
    // which is why emptying the setting is how the shell is turned off.
    allowedTools: [
      ...listDescriptors().map((one) => one.name),
      ...settings.get<string[]>('extraAllowedTools', ['shell_.*']),
    ],
    ide: vscode.env.appName,
    ideVersion: vscode.version,
    projectName: folder?.name ?? 'no folder',
    projectPath: folder?.uri.fsPath ?? null,
  };
}

/** Ask for the provider key and keep it in the OS keychain. */
export function registerKeyCommands(
  context: vscode.ExtensionContext,
): vscode.Disposable[] {
  return [
    vscode.commands.registerCommand('a11.setApiKey', async () => {
      const provider = vscode.workspace
        .getConfiguration('a11')
        .get<string>('provider', 'claude');
      const key = await vscode.window.showInputBox({
        title: `A11: the ${provider} API key`,
        prompt: 'Kept in this machine’s keychain, never in settings.json.',
        password: true,
        ignoreFocusOut: true,
      });
      if (key === undefined) return;
      await context.secrets.store(API_KEY, key.trim());
      void vscode.window.showInformationMessage('A11: API key stored.');
    }),
    vscode.commands.registerCommand('a11.clearApiKey', async () => {
      await context.secrets.delete(API_KEY);
      void vscode.window.showInformationMessage('A11: API key forgotten.');
    }),
  ];
}
