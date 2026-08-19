/**
 * The chat and the action explorer, as webview views.
 *
 * The UI is `../../webview`, shared with the JetBrains plugin: the same chat, the
 * same explorer, the same conversation list. What is here is this editor's half of
 * the bridge — the six methods behind which the shared UI hides its host — carried
 * over `postMessage`.
 *
 * **Why a correlation id.** The JetBrains host answers a call directly, because a
 * `JBCefJsQuery` is request/response. A webview channel is one-way messages in each
 * direction, so a call and its answer have to be paired up: the page sends
 * `{id, method, args}` and this answers `{id, ok, value}`. That pairing is the only
 * thing the VSCode adapter adds, and it is why the two hosts have separate adapter
 * files rather than one with a flag in it.
 */

import * as fs from 'node:fs';
import * as path from 'node:path';
import * as vscode from 'vscode';
import {readConfig} from './settings.js';
import type {Suggestions, HighlightNote} from './suggestions.js';
import {listDescriptors, runByName} from './tools/index.js';

/** What the page may ask for. The shared bridge's six methods, and nothing else. */
type Method =
  | 'listActions'
  | 'runAction'
  | 'getConfig'
  | 'readFlow'
  | 'suggestOnHighlight'
  | 'clearSuggestions';

interface Call {
  id: number;
  method: Method;
  args: unknown[];
}

export class A11ViewProvider implements vscode.WebviewViewProvider {
  /**
   * The panel, once the editor has resolved it.
   *
   * Held so a *command* can drive the page: "new chat" is the page's own action,
   * and an editor offering it in its palette should run that one rather than a
   * second implementation of it. Undefined until the view has been opened at
   * least once, which is why [newChat] reveals it first.
   */
  private panel: vscode.WebviewView | undefined;

  constructor(
    private readonly context: vscode.ExtensionContext,
    private readonly view: 'chat' | 'actions',
    private readonly suggestions: Suggestions,
  ) {}

  resolveWebviewView(panel: vscode.WebviewView): void {
    this.panel = panel;
    panel.onDidDispose(() => {
      if (this.panel === panel) this.panel = undefined;
    });
    panel.webview.options = {
      enableScripts: true,
      localResourceRoots: [vscode.Uri.joinPath(this.context.extensionUri, 'dist')],
    };
    panel.webview.html = this.html(panel.webview);
    panel.webview.onDidReceiveMessage((message: Call) => {
      void this.answer(panel.webview, message);
    });
  }

  /**
   * Start a fresh conversation in this view, opening it if it is not open.
   *
   * Revealed first, because a command that silently did nothing when the panel had
   * never been shown would be indistinguishable from a broken one — and revealing
   * is what causes the editor to resolve the view in the first place.
   */
  async newChat(): Promise<void> {
    await vscode.commands.executeCommand(`a11.${this.view}.focus`);
    this.panel?.show?.(true);
    await this.panel?.webview.postMessage({command: 'newChat'});
  }

  /** Run one call from the page and send its answer back under the same id. */
  private async answer(webview: vscode.Webview, call: Call): Promise<void> {
    if (typeof call?.id !== 'number') return;
    try {
      const value = await this.dispatch(call);
      await webview.postMessage({id: call.id, ok: true, value});
    } catch (error) {
      // The page's wrappers reject with this, exactly as the JetBrains query's
      // failure channel does, so the shared UI's error handling is unchanged.
      await webview.postMessage({
        id: call.id,
        ok: false,
        error: error instanceof Error ? error.message : String(error),
      });
    }
  }

  /**
   * One bridge method. Each answers a JSON *string*, which is what the shared
   * wrappers parse — the same contract the JetBrains bridge has.
   */
  private async dispatch(call: Call): Promise<string> {
    switch (call.method) {
      case 'listActions':
        return JSON.stringify(listDescriptors());
      case 'runAction': {
        const [name, inputs] = call.args as [string, Record<string, unknown>];
        return JSON.stringify(await runByName(name, inputs ?? {}));
      }
      case 'getConfig':
        return JSON.stringify(await readConfig(this.context));
      case 'readFlow': {
        const [name] = call.args as [string];
        return this.readFlow(name);
      }
      case 'suggestOnHighlight': {
        const [note] = call.args as [HighlightNote];
        this.suggestions.add(note);
        return '{}';
      }
      case 'clearSuggestions': {
        const [file] = call.args as [string];
        this.suggestions.clear(file || undefined);
        return '{}';
      }
      default:
        throw new Error(`The page asked for an unknown method: ${String(call.method)}`);
    }
  }

  /**
   * The text of a flow this extension ships, by bare name.
   *
   * Returned as-is rather than as JSON: a flow is source, and the one thing the
   * page does with it is hand it to `flow_run`. The name is taken apart so that a
   * page asking for `../../../etc/passwd` gets a flow that does not exist.
   */
  private readFlow(name: string): string {
    const bare = path.basename(name).replace(/\.flow$/, '');
    const file = path.join(this.context.extensionPath, 'flows', `${bare}.flow`);
    if (!fs.existsSync(file)) throw new Error(`No flow named ${bare}.`);
    return fs.readFileSync(file, 'utf8');
  }

  /**
   * The page: the shared `index.html`, with its theme variables filled in from
   * this editor's and its script pointed at the bundle.
   *
   * `%%THEME_VARS%%` is the seam the shared HTML leaves for a host — the JetBrains
   * side substitutes the IDE's look-and-feel there, and this substitutes VSCode's
   * own CSS variables, so one stylesheet reads as native in both.
   */
  private html(webview: vscode.Webview): string {
    const file = path.join(this.context.extensionPath, 'dist', 'index.html');
    const template = fs.readFileSync(file, 'utf8');
    const script = webview.asWebviewUri(
      vscode.Uri.joinPath(this.context.extensionUri, 'dist', 'app.js'),
    );
    const nonce = String(Math.random()).slice(2);
    return template
      .replace('%%THEME_VARS%%', THEME_VARS)
      // First in `<head>`, before the stylesheet and long before the script: a
      // policy is only worth having if it is in force for everything after it, and
      // the editor warns about a webview it cannot find one in.
      .replace(
        '<head>',
        `<head>\n  <meta http-equiv="Content-Security-Policy" content="default-src 'none';` +
          ` img-src ${webview.cspSource} data:; style-src ${webview.cspSource} 'unsafe-inline';` +
          ` font-src ${webview.cspSource}; script-src 'nonce-${nonce}';` +
          ` connect-src ws: wss: http: https:;">`,
      )
      .replace(
        '</body>',
        `<script nonce="${nonce}">window.__A11_VIEW = ${JSON.stringify(this.view)};</script>` +
          `<script nonce="${nonce}" src="${script.toString()}"></script>\n</body>`,
      );
  }
}

/**
 * This editor's colours, in the variables the shared stylesheet reads.
 *
 * Mapped rather than hard-coded so the panel follows the user's theme, including
 * a light one: every value is a `--vscode-*` variable the webview host defines.
 */
const THEME_VARS = `
      --a11-bg: var(--vscode-sideBar-background);
      --a11-bg-alt: var(--vscode-editor-background);
      --a11-fg: var(--vscode-foreground);
      --a11-muted: var(--vscode-descriptionForeground);
      --a11-border: var(--vscode-panel-border, var(--vscode-editorWidget-border));
      --a11-accent: var(--vscode-button-background);
      --a11-accent-fg: var(--vscode-button-foreground);
      --a11-user-bg: var(--vscode-editorWidget-background);
      --a11-assistant-bg: var(--vscode-editor-inactiveSelectionBackground);
      --a11-error: var(--vscode-errorForeground);
      --a11-json-key: var(--vscode-symbolIcon-propertyForeground);
      --a11-json-string: var(--vscode-debugTokenExpression-string);
      --a11-json-number: var(--vscode-debugTokenExpression-number);
      --a11-json-keyword: var(--vscode-debugTokenExpression-boolean);
      --a11-font: var(--vscode-font-family);
      --a11-mono: var(--vscode-editor-font-family);
`;
