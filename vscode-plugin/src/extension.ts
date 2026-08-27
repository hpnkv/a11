/**
 * A11 Flow language support and chat integration for VS Code.
 *
 * The extension runs the native language implementation from `cpp/a11/flow/`
 * through `a11-flow serve --protocol lsp`. It owns service discovery,
 * VS Code-backed tools, embedded Flow fragments, and the webview host bridge.
 *
 * If no binary is available, the generated TextMate grammar still highlights
 * `.flow` files and one notification explains why semantic features are absent.
 */

import * as vscode from 'vscode';
import {FlowServer, FLOW_LANGUAGE} from './flowClient.js';
import {
  checkFragments,
  completeInFragment,
  hoverInFragment,
  type FixCarrier,
} from './fragments.js';
import {indentAfter} from './flowIndent.js';
import {registerKeyCommands} from './settings.js';
import {Suggestions, registerApply} from './suggestions.js';
import {complainOnce, findFlowTool} from './tool.js';
import {A11ViewProvider} from './views.js';

/** The languages a flow may be written inside a string of. */
const HOSTS = ['python', 'typescript', 'javascript', 'typescriptreact', 'javascriptreact', 'java', 'kotlin', 'go', 'cpp'];

/** How long to let a burst of edits settle before checking fragments again. */
const SETTLE_MILLIS = 400;

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  const suggestions = new Suggestions();
  context.subscriptions.push(suggestions, registerApply());

  // The two views. Before the language server, because a chat that works
  // without an `a11-flow` binary should not wait for one.
  const views = new Map<'chat' | 'actions', A11ViewProvider>();
  for (const view of ['chat', 'actions'] as const) {
    const provider = new A11ViewProvider(context, view, suggestions);
    views.set(view, provider);
    context.subscriptions.push(
      vscode.window.registerWebviewViewProvider(`a11.${view}`, provider, {
        webviewOptions: {retainContextWhenHidden: true},
      }),
    );
  }

  // The language, if this machine has it. A `FlowServer` that never started is
  // a state the commands below have to answer for rather than a reason not to
  // register them: a command in the palette that reports "command not found"
  // tells the reader their editor is broken, when the truth is that a binary is
  // missing and there is something they can do about it.
  const flow = new FlowSupport(context);

  // Continuation-aware indent on Enter, needing no server and no binary: a
  // `skip` list past a `,`, a pipeline left open after `|`/`->`, an unclosed
  // `(`/`[`. See flowIndent.ts for why this never asks the language server.
  context.subscriptions.push(
    vscode.languages.registerOnTypeFormattingEditProvider(
      {language: FLOW_LANGUAGE},
      {
        provideOnTypeFormattingEdits(document, position, _ch, options) {
          if (position.line === 0) return [];
          const previousLineEnd = document.lineAt(position.line - 1).range.end;
          const before = document.getText(new vscode.Range(0, 0, previousLineEnd.line, previousLineEnd.character));
          const unit = options.insertSpaces ? ' '.repeat(options.tabSize) : '\t';
          const desired = indentAfter(before, unit);
          if (desired === null) return [];
          const line = document.lineAt(position.line);
          const currentIndent = line.text.match(/^[ \t]*/)?.[0] ?? '';
          if (currentIndent === desired) return [];
          return [
            vscode.TextEdit.replace(
              new vscode.Range(position.line, 0, position.line, currentIndent.length),
              desired,
            ),
          ];
        },
      },
      '\n',
    ),
  );

  // Every command this extension declares, registered here and nowhere else, in
  // one place that nothing returns before. `commands.test.mjs` holds that: a
  // command contributed in `package.json` and registered behind a conditional
  // is exactly the bug this shape prevents.
  context.subscriptions.push(
    ...registerKeyCommands(context),
    vscode.commands.registerCommand('a11.clearSuggestions', () => suggestions.clear()),
    vscode.commands.registerCommand('a11.newChat', () => views.get('chat')?.newChat()),
    vscode.commands.registerCommand('a11.rescanActions', () => flow.rescan()),
    vscode.commands.registerCommand('a11.restartFlowServer', () => flow.restart()),
    flow,
  );

  await flow.start();
}

/**
 * The language server, and everything that needs one.
 *
 * A holder rather than a bare local, so the commands can be registered before
 * the binary has been looked for and still do the right thing afterwards —
 * including the case that matters most to somebody setting this up: **the
 * binary appears later**. `a11.restartFlowServer` looks again, so building
 * `a11-flow` and running that command is enough, with no window reload.
 */
class FlowSupport implements vscode.Disposable {
  private server: FlowServer | undefined;
  private started = false;
  private readonly parts: vscode.Disposable[] = [];

  /**
   * One channel for the whole session, however many servers pass through it.
   */
  private readonly log = vscode.window.createOutputChannel('A11 Flow');

  constructor(private readonly context: vscode.ExtensionContext) {}

  /**
   * Look for the tool and, if it is there, start everything that needs it.
   *
   * A start that fails is *reported*, not thrown. Letting it out of here failed
   * the whole activation — so the chat and the tools went with the language,
   * over a binary that will not run — and left a dead client assigned for the
   * next call to trip over. The message says what actually happened, because
   * "activating extension failed" says nothing anybody can act on.
   */
  async start(): Promise<boolean> {
    const executable = findFlowTool(this.context);
    if (!executable) {
      await complainOnce(this.context);
      return false;
    }
    const server = new FlowServer(executable, this.log);
    try {
      await server.start();
    } catch (error) {
      await server.stop();
      this.server = undefined;
      const why = error instanceof Error ? error.message : String(error);
      void vscode.window.showErrorMessage(
        `A11: the Flow language server would not start. ${why}`,
        'Show the log',
      ).then((choice) => {
        if (choice === 'Show the log') server.showLog();
      });
      return false;
    }
    this.server = server;
    if (!this.started) {
      // The document listeners are attached once and outlive a restart: they
      // read through `this.server`, so a fresh one is picked up without
      // re-subscribing.
      startScanning(() => this.server, this.parts);
      registerFragmentSupport(() => this.server, this.parts);
      this.started = true;
    }
    await this.rescan(/*quiet=*/ true);
    return true;
  }

  /**
   * Start the server, or stop and start it again.
   *
   * Also the way *in* after building the binary: it looks for the tool again,
   * so `cmake --build … --target a11_flow_tool` followed by this command is
   * enough, with no window reload. Silent on failure, because `start` has
   * already said what went wrong and saying it twice is worse than saying it
   * once.
   */
  async restart(): Promise<void> {
    await this.server?.stop();
    this.server = undefined;
    if (await this.start()) {
      void vscode.window.showInformationMessage(
        'A11: the Flow language server is running.',
      );
    }
  }

  /** Read the workspace's own actions again. */
  async rescan(quiet = false): Promise<void> {
    if (!this.server) {
      if (!quiet) void missing();
      return;
    }
    const roots = (vscode.workspace.workspaceFolders ?? []).map(
      (folder) => folder.uri.fsPath,
    );
    if (roots.length === 0) {
      if (!quiet) {
        void vscode.window.showInformationMessage('A11: no folder is open to read.');
      }
      return;
    }
    const found = await this.server.scan(roots);
    if (found?.reached_file_limit) {
      // Said rather than silently applied: a half-read workspace otherwise
      // looks exactly like a workspace with two actions in it.
      void vscode.window.showWarningMessage(
        `A11: stopped after ${found.files_read} files, so some of the workspace was` +
          ' not read for actions.',
      );
    } else if (!quiet) {
      void vscode.window.showInformationMessage(
        found
          ? `A11: ${found.actions} action(s) declared in ${found.files_read} files.`
          : 'A11: nothing to scan.',
      );
    }
  }

  dispose(): void {
    void this.server?.stop();
    for (const one of this.parts) one.dispose();
    this.log.dispose();
  }
}

/**
 * What to say when a command needs the
 * language and this machine has no binary.
 */
async function missing(): Promise<void> {
  const choice = await vscode.window.showWarningMessage(
    'A11: no `a11-flow` binary, so the language is not running. Build one with' +
      ' `cmake --build --preset debug --target a11_flow_tool`, then run' +
      ' “A11: Restart the Flow language server”.',
    'Set its path',
  );
  if (choice === 'Set its path') {
    await vscode.commands.executeCommand(
      'workbench.action.openSettings',
      'a11.flow.toolPath',
    );
  }
}

/**
 * Read the workspace's own actions again when one of them changes.
 *
 * The language ships a snapshot of what the SDK registers, so hovering
 * `interact_with_llm` has always said
 * something useful. An action somebody wrote
 * this afternoon is in no snapshot: this is what makes it hover with its
 * description and its ports, and gives F12 somewhere to go.
 *
 * Debounced, because a save-all over twenty files is one interesting event. The
 * server is read through a getter rather than captured, so a restart is picked
 * up without re-subscribing these listeners.
 */
function startScanning(
  server: () => FlowServer | undefined,
  parts: vscode.Disposable[],
): void {
  const enabled = () =>
    vscode.workspace.getConfiguration('a11').get<boolean>('flow.scanWorkspace', true);

  let timer: NodeJS.Timeout | undefined;
  const later = () => {
    if (!enabled()) return;
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => {
      const roots = (vscode.workspace.workspaceFolders ?? []).map(
        (folder) => folder.uri.fsPath,
      );
      if (roots.length > 0) void server()?.scan(roots);
    }, 750);
  };

  const watcher = vscode.workspace.createFileSystemWatcher(
    '**/*.{py,pyi,cc,cpp,cxx,h,hpp,ts,tsx,mts,js,mjs,jsx}',
  );
  watcher.onDidChange(later);
  watcher.onDidCreate(later);
  watcher.onDidDelete(later);
  parts.push(watcher, {
    dispose: () => {
      if (timer) clearTimeout(timer);
    },
  });
}

/**
 * Flows written inside another language's strings: checked, completed and
 * hovered.
 *
 * The `.flow` case is the language client's and needs nothing here. This is the
 * other case, and the one most flows are actually in.
 */
function registerFragmentSupport(
  server: () => FlowServer | undefined,
  parts: vscode.Disposable[],
): void {
  const enabled = () =>
    vscode.workspace.getConfiguration('a11').get<boolean>('flow.checkFragments', true);

  const collection = vscode.languages.createDiagnosticCollection('a11flow-fragments');
  parts.push(collection);

  let timer: NodeJS.Timeout | undefined;
  const check = async (document: vscode.TextDocument) => {
    const running = server();
    if (!running || !enabled() || !HOSTS.includes(document.languageId)) return;
    collection.set(document.uri, await checkFragments(running, document));
  };
  const later = (document: vscode.TextDocument) => {
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => void check(document), SETTLE_MILLIS);
  };

  parts.push(
    vscode.workspace.onDidOpenTextDocument((document) => void check(document)),
    vscode.workspace.onDidChangeTextDocument((event) => later(event.document)),
    vscode.workspace.onDidCloseTextDocument((document) => collection.delete(document.uri)),
    {
      dispose: () => {
        if (timer) clearTimeout(timer);
      },
    },
  );
  for (const document of vscode.workspace.textDocuments) void check(document);

  const selector = HOSTS.map((language) => ({scheme: 'file', language}));

  parts.push(
    // Apply the edits carried by the diagnostic without re-deriving a repair
    // from its message.
    vscode.languages.registerCodeActionsProvider(
      selector,
      {
        provideCodeActions(document, _range, actionContext) {
          const actions: vscode.CodeAction[] = [];
          for (const diagnostic of actionContext.diagnostics) {
            for (const fix of (diagnostic as FixCarrier).a11Fixes ?? []) {
              const action = new vscode.CodeAction(
                fix.label,
                vscode.CodeActionKind.QuickFix,
              );
              action.diagnostics = [diagnostic];
              const edit = new vscode.WorkspaceEdit();
              for (const one of fix.edits) {
                edit.replace(document.uri, one.range, one.text);
              }
              action.edit = edit;
              actions.push(action);
            }
          }
          return actions;
        },
      },
      {providedCodeActionKinds: [vscode.CodeActionKind.QuickFix]},
    ),
    vscode.languages.registerCompletionItemProvider(
      selector,
      {
        provideCompletionItems: (document, position) => {
          const running = server();
          return running && enabled()
            ? completeInFragment(running, document, position)
            : [];
        },
      },
      // The same set the language server advertises, and for the same reason:
      // `(` opens an argument list and `,` separates one argument from the
      // next, so both are positions where the answer is "the ports of the
      // action being called". Kept in step by `completion.test.mjs`, since two
      // lists of trigger characters is two lists to forget.
      '.',
      '|',
      ':',
      '>',
      ' ',
      '(',
      ',',
    ),
    vscode.languages.registerHoverProvider(selector, {
      provideHover: (document, position) => {
        const running = server();
        return running && enabled()
          ? hoverInFragment(running, document, position)
          : undefined;
      },
    }),
  );
}

export function deactivate(): void {
  // Everything is in `context.subscriptions`, which the host disposes.
}

// Referenced so the language id is stated once and read from here by tests.
export {FLOW_LANGUAGE};
