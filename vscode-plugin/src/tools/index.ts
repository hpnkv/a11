/**
 * The tools this editor exposes to the model.
 *
 * The same ten names, the same descriptions and the same coordinate conventions as
 * `IdeTools.kt` in the JetBrains plugin, implemented in TypeScript against VS Code APIs.
 * What is shared is the tool schema, name list, and parameter conventions.
 * thing worth sharing — the descriptor JSON, which is the contract the model and
 * the UI both read, and which this file is the single source of on this side.
 *
 * Two conventions carried over exactly, because a mismatch would be silent:
 *
 * * **Lines are 0-based and `end_line` is inclusive.** A highlight's own numbers
 *   read back the lines it sits on with `read_file` and no arithmetic.
 * * **An output flagged `user_facing` is the run log**, written for the person
 *   watching and never part of the tool result. The gateway's LLM tool runner is
 *   what holds it back; this side only has to flag it.
 */

import * as path from 'node:path';
import * as vscode from 'vscode';
import {applyPatch} from './patch.js';

/**
 * The key every tool's result map carries its narration under, for the user's
 * eyes.
 *
 * A key, not a port: narration travels on the action's own log port, which no
 * schema declares. The A11 handler picks this out of the result and logs it, so
 * it reaches whoever is watching without ever being one of the tool's outputs.
 */
export const RUN_LOG_KEY = 'run_log';

export interface PortDescriptor {
  name: string;
  type: string;
  required: boolean;
  unary: boolean;
  description?: string;
  schema?: Record<string, unknown>;
  user_facing?: boolean;
}

export interface ActionDescriptor {
  name: string;
  description: string;
  inputs: PortDescriptor[];
  outputs: PortDescriptor[];
  output_to_json_field?: Record<string, string>;
}

type Handler = (inputs: Record<string, unknown>) => Promise<Record<string, unknown>>;

interface Tool {
  descriptor: ActionDescriptor;
  run: Handler;
}

// --- descriptor helpers ------------------------------------------------------

function text(name: string, description: string, unary = true): PortDescriptor {
  return {name, type: 'text/plain', required: false, unary, description};
}

function required(name: string, description: string): PortDescriptor {
  return {name, type: 'text/plain', required: true, unary: true, description};
}

function json(
  name: string,
  description: string,
  schema?: Record<string, unknown>,
): PortDescriptor {
  return {
    name,
    type: 'application/json',
    required: false,
    unary: true,
    description,
    ...(schema ? {schema} : {}),
  };
}

/** A run log: one summary line, then whatever detail is worth reading. */
function log(summary: string, ...detail: string[]): string[] {
  return [summary, ...detail.filter((one) => one.length > 0)];
}

function bullets(items: readonly string[]): string {
  return items.map((one) => `- \`${one}\``).join('\n');
}

// --- the workspace -----------------------------------------------------------

/** The file in the active editor, or undefined. */
function activeDocument(): vscode.TextDocument | undefined {
  return vscode.window.activeTextEditor?.document;
}

/**
 * `given` as an absolute path.
 *
 * A tool may be given an absolute path or one relative to the workspace, exactly
 * as the JetBrains tools accept both: a model that read a path out of a diagnostic
 * has whichever the diagnostic carried.
 */
function resolvePath(given: string): string | undefined {
  if (!given) return undefined;
  if (path.isAbsolute(given)) return given;
  const root = vscode.workspace.workspaceFolders?.[0];
  return root ? path.join(root.uri.fsPath, given) : undefined;
}

async function openDocument(given: string): Promise<vscode.TextDocument> {
  const resolved = resolvePath(given);
  if (!resolved) throw new Error(`No such file: ${given}`);
  return vscode.workspace.openTextDocument(vscode.Uri.file(resolved));
}

/** The lines of `document` in `[from, to]`, `to` inclusive as the contract says. */
function slice(
  document: vscode.TextDocument,
  from: number | undefined,
  to: number | undefined,
  numbered: boolean,
): string[] {
  const first = Math.max(0, from ?? 0);
  const last = Math.min(document.lineCount - 1, to ?? document.lineCount - 1);
  const out: string[] = [];
  for (let line = first; line <= last; line += 1) {
    const body = document.lineAt(line).text;
    out.push(numbered ? `${line}\t${body}` : body);
  }
  return out;
}

function number_(inputs: Record<string, unknown>, key: string): number | undefined {
  const held = inputs[key];
  if (typeof held === 'number') return held;
  if (typeof held === 'string' && held.trim() !== '') return Number(held);
  return undefined;
}

function string_(inputs: Record<string, unknown>, key: string): string {
  const held = inputs[key];
  if (typeof held === 'string') return held;
  if (Array.isArray(held) && typeof held[0] === 'string') return held[0];
  return '';
}

function object_(inputs: Record<string, unknown>, key: string): Record<string, unknown> {
  const held = inputs[key];
  if (held && typeof held === 'object' && !Array.isArray(held)) {
    return held as Record<string, unknown>;
  }
  if (typeof held === 'string' && held.trim() !== '') {
    try {
      const parsed = JSON.parse(held);
      if (parsed && typeof parsed === 'object') return parsed as Record<string, unknown>;
    } catch {
      // A request that is not JSON is a request for the whole file, which is
      // what an absent one means too.
    }
  }
  return {};
}

// --- the tools ---------------------------------------------------------------

const RANGE_INPUT = json(
  'request',
  'Which lines to read: {"start_line": n, "end_line": n, "include_line_numbers": bool}.' +
    ' 0-based, end_line inclusive. Omit to read the whole file.',
  {
    type: 'object',
    properties: {
      start_line: {type: 'integer', minimum: 0},
      end_line: {type: 'integer', minimum: 0},
      include_line_numbers: {type: 'boolean'},
    },
  },
);

function activeFileTool(): Tool {
  return {
    descriptor: {
      name: 'get_active_file',
      description:
        'Return the path of the file in the active editor and its text. Pass a request to' +
        ' read only part of a large file; with none, the whole file is returned.',
      inputs: [RANGE_INPUT],
      outputs: [
        text(
          'lines',
          'The requested lines of the file, one value per line; each prefixed with its' +
            ' 0-based number and a tab when the request asked for that.',
          false,
        ),
        text(
          'path',
          'Absolute path of the file in the active editor; absent when no file is open.',
        ),
      ],
    },
    async run(inputs) {
      const document = activeDocument();
      if (!document) {
        return {
          lines: [],
          path: null,
          [RUN_LOG_KEY]: log('No file is open in an editor'),
        };
      }
      const asked = object_(inputs, 'request');
      const lines = slice(
        document,
        number_(asked, 'start_line'),
        number_(asked, 'end_line'),
        asked.include_line_numbers === true,
      );
      const where = document.uri.fsPath;
      const from = number_(asked, 'start_line') ?? 0;
      return {
        lines,
        path: where,
        [RUN_LOG_KEY]: log(
          lines.length === 0
            ? `Read no lines of ${path.basename(where)}`
            : `Read ${lines.length} lines of ${path.basename(where)}`,
          `\`${where}\``,
          lines.length === 0 ? '' : `Lines ${from + 1}–${from + lines.length}.`,
        ),
      };
    },
  };
}

function openEditorsTool(): Tool {
  return {
    descriptor: {
      name: 'get_open_editors',
      description: 'List the paths of all files open in editors.',
      inputs: [],
      outputs: [
        text('files', 'Absolute path of each file open in an editor.', false),
      ],
    },
    async run() {
      // Every tab of every group, not only the visible editors: "open" is what a
      // person sees in their tab bar.
      const open = vscode.window.tabGroups.all
        .flatMap((group) => group.tabs)
        .map((tab) => tab.input)
        .filter((input): input is vscode.TabInputText => input instanceof vscode.TabInputText)
        .map((input) => input.uri.fsPath);
      const unique = [...new Set(open)];
      return {
        files: unique,
        [RUN_LOG_KEY]: log(
          unique.length === 0
            ? 'No files are open in editors'
            : `Listed ${unique.length} open editors`,
          bullets(unique),
        ),
      };
    },
  };
}

function selectionTool(): Tool {
  return {
    descriptor: {
      name: 'get_selection',
      description:
        'Return the current editor selection: where it sits, and the lines it covers.',
      inputs: [],
      outputs: [
        json('metadata', 'Where the selection is: path, start_line, start_column, end_line, end_column.', {
          type: 'object',
          properties: {
            path: {type: 'string'},
            start_line: {type: 'integer'},
            start_column: {type: 'integer'},
            end_line: {type: 'integer'},
            end_column: {type: 'integer'},
          },
        }),
        text('lines', 'The selected lines, one value per line.', false),
      ],
    },
    async run() {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.selection.isEmpty) {
        return {
          metadata: null,
          lines: [],
          [RUN_LOG_KEY]: log('Nothing is selected in the active editor'),
        };
      }
      const {start, end} = editor.selection;
      const where = editor.document.uri.fsPath;
      const lines = slice(editor.document, start.line, end.line, false);
      return {
        metadata: {
          path: where,
          start_line: start.line,
          start_column: start.character,
          end_line: end.line,
          end_column: end.character,
        },
        lines,
        [RUN_LOG_KEY]: log(
          `Read the selection in ${path.basename(where)} (lines ${start.line}–${end.line})`,
          lines.length === 0 ? '' : '```\n' + lines.join('\n') + '\n```',
        ),
      };
    },
  };
}

/** VSCode's symbol kinds, as the names the JetBrains tool reports. */
function symbolKindName(kind: vscode.SymbolKind): string {
  return vscode.SymbolKind[kind]?.toLowerCase() ?? 'unknown';
}

function flattenSymbols(
  symbols: vscode.DocumentSymbol[],
  container: string,
  into: Array<Record<string, unknown>>,
): void {
  for (const symbol of symbols) {
    into.push({
      name: symbol.name,
      kind: symbolKindName(symbol.kind),
      container,
      start_line: symbol.range.start.line,
      end_line: symbol.range.end.line,
      detail: symbol.detail || undefined,
    });
    flattenSymbols(symbol.children, symbol.name, into);
  }
}

function fileSymbolsTool(): Tool {
  return {
    descriptor: {
      name: 'get_file_symbols',
      description:
        "List the named symbols declared in a file, with each one's kind and position." +
        ' Give a `path` for any file of the project, or omit it for the one in the' +
        ' active editor.',
      inputs: [text('path', 'Absolute or project-relative path; omit for the active file.')],
      outputs: [
        json('symbols', 'One entry per declared symbol: name, kind, container and line range.', {
          type: 'array',
          items: {type: 'object'},
        }),
      ],
    },
    async run(inputs) {
      const given = string_(inputs, 'path');
      const document = given ? await openDocument(given) : activeDocument();
      if (!document) {
        return {symbols: [], [RUN_LOG_KEY]: log('No file to read symbols from')};
      }
      // The language's own provider, whichever extension supplies it: this tool
      // has no business knowing how Python declares a class.
      const found =
        (await vscode.commands.executeCommand<vscode.DocumentSymbol[]>(
          'vscode.executeDocumentSymbolProvider',
          document.uri,
        )) ?? [];
      const flat: Array<Record<string, unknown>> = [];
      flattenSymbols(found, '', flat);
      const where = document.uri.fsPath;
      return {
        symbols: flat,
        [RUN_LOG_KEY]: log(
          `Listed ${flat.length} symbols in ${path.basename(where)}`,
          `\`${where}\``,
        ),
      };
    },
  };
}

function readFileTool(): Tool {
  return {
    descriptor: {
      name: 'read_file',
      description:
        'Read a range of lines from any file of the project, open in an editor or not.' +
        ' Lines are 0-based and `end_line` is inclusive, the same coordinates' +
        ' `get_error_highlights` reports, so a highlight reads back the lines it sits on.',
      inputs: [
        required('path', 'Absolute or project-relative path of the file to read.'),
        RANGE_INPUT,
      ],
      outputs: [
        text(
          'lines',
          'The requested lines, one value per line; each prefixed with its 0-based' +
            ' number and a tab when the request asked for that.',
          false,
        ),
      ],
    },
    async run(inputs) {
      const given = string_(inputs, 'path');
      const document = await openDocument(given);
      const asked = object_(inputs, 'request');
      const from = number_(asked, 'start_line');
      const lines = slice(
        document,
        from,
        number_(asked, 'end_line'),
        asked.include_line_numbers === true,
      );
      const where = document.uri.fsPath;
      return {
        lines,
        [RUN_LOG_KEY]: log(
          `Read ${lines.length} lines of ${path.basename(where)}`,
          `\`${where}\``,
          lines.length === 0 ? '' : `Lines ${(from ?? 0) + 1}–${(from ?? 0) + lines.length}.`,
        ),
      };
    },
  };
}

function applyPatchTool(): Tool {
  return {
    descriptor: {
      name: 'apply_patch',
      description:
        'Apply a unified diff to one file of the project. The edit lands as a single IDE' +
        ' command, so one Undo takes it back and the user can see exactly what' +
        ' changed. Hunks are placed by their context rather than by the numbers in' +
        ' their @@ header, and a hunk that does not match the file is refused with' +
        ' what is there instead: nothing is applied on a near miss.',
      inputs: [
        required('path', 'Absolute or project-relative path of the file to patch.'),
        required('patch', 'The unified diff to apply.'),
      ],
      outputs: [
        json('result', 'What was applied: hunks, lines added and lines removed.', {
          type: 'object',
          properties: {
            path: {type: 'string'},
            hunks: {type: 'integer'},
            added: {type: 'integer'},
            removed: {type: 'integer'},
          },
        }),
      ],
    },
    async run(inputs) {
      const given = string_(inputs, 'path');
      const patch = string_(inputs, 'patch');
      if (!patch.trim()) throw new Error('apply_patch needs a patch to apply.');
      const document = await openDocument(given);
      const outcome = applyPatch(document.getText(), patch);

      // One edit over the whole file, which is what makes one Undo reverse the
      // whole patch — the guarantee the JetBrains tool gets from running inside a
      // single IDE command.
      const edit = new vscode.WorkspaceEdit();
      const whole = new vscode.Range(
        document.positionAt(0),
        document.positionAt(document.getText().length),
      );
      edit.replace(document.uri, whole, outcome.text);
      if (!(await vscode.workspace.applyEdit(edit))) {
        throw new Error(`Could not apply the patch to ${given}.`);
      }
      const where = document.uri.fsPath;
      return {
        result: {
          path: where,
          hunks: outcome.hunks,
          added: outcome.added,
          removed: outcome.removed,
        },
        [RUN_LOG_KEY]: log(
          `Patched ${path.basename(where)}: ${outcome.hunks} hunk(s),` +
            ` +${outcome.added} −${outcome.removed}`,
          `\`${where}\``,
          '```diff\n' + patch.trim() + '\n```',
        ),
      };
    },
  };
}

function errorHighlightsTool(): Tool {
  return {
    descriptor: {
      name: 'get_error_highlights',
      description:
        "Report the problems the IDE's code analysis finds in a range of lines of a file:" +
        ' every red (error) and yellow (warning) underline, with its position, the text it' +
        ' covers and the explanation its tooltip gives. Lines are 0-based and `end_line`' +
        ' is inclusive; omit the range for the whole file.',
      inputs: [
        text('path', 'Absolute or project-relative path; omit for the active file.'),
        json('request', 'Which lines: {"start_line": n, "end_line": n}. 0-based, end inclusive.', {
          type: 'object',
          properties: {
            start_line: {type: 'integer', minimum: 0},
            end_line: {type: 'integer', minimum: 0},
          },
        }),
      ],
      outputs: [
        json('highlights', 'One entry per underline: severity, position, the text and the message.', {
          type: 'array',
          items: {type: 'object'},
        }),
      ],
    },
    async run(inputs) {
      const given = string_(inputs, 'path');
      const document = given ? await openDocument(given) : activeDocument();
      if (!document) {
        return {highlights: [], [RUN_LOG_KEY]: log('No file to read highlights from')};
      }
      const asked = object_(inputs, 'request');
      const from = number_(asked, 'start_line') ?? 0;
      const to = number_(asked, 'end_line') ?? document.lineCount - 1;
      const found = vscode.languages
        .getDiagnostics(document.uri)
        .filter(
          (one) =>
            one.severity === vscode.DiagnosticSeverity.Error ||
            one.severity === vscode.DiagnosticSeverity.Warning,
        )
        .filter((one) => one.range.end.line >= from && one.range.start.line <= to)
        .map((one) => ({
          severity:
            one.severity === vscode.DiagnosticSeverity.Error ? 'error' : 'warning',
          start_line: one.range.start.line,
          start_column: one.range.start.character,
          end_line: one.range.end.line,
          end_column: one.range.end.character,
          text: document.getText(one.range),
          message: one.message,
          source: one.source,
        }));
      const where = document.uri.fsPath;
      return {
        highlights: found,
        [RUN_LOG_KEY]: log(
          found.length === 0
            ? `No errors or warnings in ${path.basename(where)}`
            : `Found ${found.length} highlight(s) in ${path.basename(where)}`,
          `\`${where}\``,
        ),
      };
    },
  };
}

function renameSymbolTool(): Tool {
  return {
    descriptor: {
      name: 'rename_symbol',
      description: 'Rename a symbol in the active file, updating the references to it.',
      inputs: [
        required('name', 'The symbol to rename, as it is written now.'),
        required('new_name', 'What to rename it to.'),
        text('path', 'Absolute or project-relative path; omit for the active file.'),
      ],
      outputs: [
        json('result', 'What was renamed, and in how many files.', {type: 'object'}),
      ],
    },
    async run(inputs) {
      const given = string_(inputs, 'path');
      const document = given ? await openDocument(given) : activeDocument();
      if (!document) throw new Error('rename_symbol needs a file.');
      const name = string_(inputs, 'name');
      const renamed = string_(inputs, 'new_name');
      if (!name || !renamed) {
        throw new Error('rename_symbol needs both `name` and `new_name`.');
      }

      // Find the symbol's own declaration to rename *at*: renaming at an
      // arbitrary occurrence is the same refactoring, but starting from the
      // declaration is what makes the language server agree it is one symbol.
      const at = firstOccurrence(document, name);
      if (!at) throw new Error(`\`${name}\` does not appear in ${document.uri.fsPath}.`);
      const edit = await vscode.commands.executeCommand<vscode.WorkspaceEdit>(
        'vscode.executeDocumentRenameProvider',
        document.uri,
        at,
        renamed,
      );
      if (!edit || edit.size === 0) {
        throw new Error(
          `No rename provider would rename \`${name}\` here. The language's` +
            ' extension may not be active for this file.',
        );
      }
      const files = edit.size;
      if (!(await vscode.workspace.applyEdit(edit))) {
        throw new Error(`Could not rename \`${name}\`.`);
      }
      return {
        result: {name, new_name: renamed, files},
        [RUN_LOG_KEY]: log(
          `Renamed \`${name}\` to \`${renamed}\` across ${files} file(s)`,
        ),
      };
    },
  };
}

/** Where a whole-word `name` first appears in the document. */
function firstOccurrence(
  document: vscode.TextDocument,
  name: string,
): vscode.Position | undefined {
  const pattern = new RegExp(`\\b${name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\b`);
  for (let line = 0; line < document.lineCount; line += 1) {
    const found = pattern.exec(document.lineAt(line).text);
    if (found) return new vscode.Position(line, found.index);
  }
  return undefined;
}

function findFileTool(): Tool {
  return {
    descriptor: {
      name: 'find_file',
      description: 'Find project files by exact file name.',
      inputs: [required('name', 'The file name to look for, without a directory.')],
      outputs: [
        text('paths', 'Absolute path of each matching file.', false),
      ],
    },
    async run(inputs) {
      const name = string_(inputs, 'name');
      if (!name) throw new Error('find_file needs a `name`.');
      const found = await vscode.workspace.findFiles(
        `**/${name}`,
        '**/{node_modules,.git,.venv,build,dist,__pycache__}/**',
        200,
      );
      const paths = found.map((one) => one.fsPath);
      return {
        paths,
        [RUN_LOG_KEY]: log(
          paths.length === 0
            ? `No file named \`${name}\``
            : `Found ${paths.length} file(s) named \`${name}\``,
          bullets(paths),
        ),
      };
    },
  };
}

function searchProjectTool(): Tool {
  return {
    descriptor: {
      name: 'search_project',
      description: 'Find project files whose name contains a query substring.',
      inputs: [required('query', 'The substring to look for in file names.')],
      outputs: [
        text('paths', 'Absolute path of each matching file.', false),
      ],
    },
    async run(inputs) {
      const query = string_(inputs, 'query');
      if (!query) throw new Error('search_project needs a `query`.');
      const found = await vscode.workspace.findFiles(
        `**/*${query}*`,
        '**/{node_modules,.git,.venv,build,dist,__pycache__}/**',
        200,
      );
      const paths = found.map((one) => one.fsPath);
      return {
        paths,
        [RUN_LOG_KEY]: log(
          paths.length === 0
            ? `Nothing matches \`${query}\``
            : `Found ${paths.length} file(s) matching \`${query}\``,
          bullets(paths),
        ),
      };
    },
  };
}

// --- the registry ------------------------------------------------------------

/**
 * Every tool, built once.
 *
 * The order is the order the action explorer lists them in, which is roughly
 * "what a model reaches for first".
 */
function build(): Tool[] {
  return [
    activeFileTool(),
    openEditorsTool(),
    selectionTool(),
    fileSymbolsTool(),
    readFileTool(),
    applyPatchTool(),
    errorHighlightsTool(),
    renameSymbolTool(),
    findFileTool(),
    searchProjectTool(),
  ];
}

let tools: Tool[] | undefined;

function all(): Tool[] {
  tools ??= build();
  return tools;
}

/** What the page and the gateway are told this editor can do. */
export function listDescriptors(): ActionDescriptor[] {
  return all().map((tool) => tool.descriptor);
}

/** Run one tool by name, with its inputs keyed by port name. */
export async function runByName(
  name: string,
  inputs: Record<string, unknown>,
): Promise<Record<string, unknown>> {
  const tool = all().find((one) => one.descriptor.name === name);
  if (!tool) throw new Error(`No such tool: ${name}`);
  return tool.run(inputs);
}
