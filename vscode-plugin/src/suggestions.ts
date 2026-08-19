/**
 * The review flow's findings, on the ranges they are about.
 *
 * A flow the extension ships (`flows/suggest-fixes.flow`) reviews a file and
 * writes two streams: a comment about a range, and a patch that fixes it. Each
 * record arrives here through the webview bridge and is put on the range it names,
 * so the reader sees it where the problem is rather than in a list somewhere else.
 *
 * **Where this differs from the JetBrains plugin, and why.** That one renders a
 * popup of its own (`highlights/SuggestionPopup.kt`) with the comment and an
 * "apply" button, because the platform gives it a gutter and a custom popup and no
 * natural home for a machine-written comment. VSCode has one: a diagnostic carries
 * the comment in its hover, and a code action carries the patch. Same information,
 * each platform's own affordance — which is the whole argument for not forcing one
 * implementation across two editors.
 */

import * as vscode from 'vscode';
import {runByName} from './tools/index.js';

/**
 * One record about one range of one file, as the review flow produces it.
 *
 * The same shape the shared UI declares (`webview/src/bridge.ts`), because it is
 * the same record: the page reads it off the flow's ports and hands it straight
 * over. Lines and columns are 0-based and `end_column` is exclusive.
 */
export interface HighlightNote {
  path: string;
  id?: string;
  origin?: 'reported' | 'found';
  comment?: string;
  patch?: string;
  start_line: number;
  start_column: number;
  end_line: number;
  end_column: number;
}

/** A suggestion, and the patch that goes with it where there is one. */
interface Suggestion {
  note: HighlightNote;
  range: vscode.Range;
}

export class Suggestions {
  private readonly collection =
    vscode.languages.createDiagnosticCollection('a11-review');

  /** By file, then by the id the flow gave the suggestion. */
  private readonly held = new Map<string, Map<string, Suggestion>>();

  private readonly disposables: vscode.Disposable[] = [];

  constructor() {
    this.disposables.push(
      this.collection,
      // The patch is offered where the comment is, as a code action.
      vscode.languages.registerCodeActionsProvider(
        {scheme: 'file'},
        new SuggestionActions(this),
        {providedCodeActionKinds: [vscode.CodeActionKind.QuickFix]},
      ),
    );
  }

  /**
   * Attach one record to the range it is about.
   *
   * A record whose `id` names a suggestion already attached is *merged* into it
   * rather than marking the range twice: the flow writes the comment and the patch
   * on two ports so the sentence need not wait for the diff, and the id is what
   * puts them back together.
   */
  add(note: HighlightNote): void {
    const key = vscode.Uri.file(note.path).toString();
    const forFile = this.held.get(key) ?? new Map<string, Suggestion>();
    // A record with no id stands alone, so it gets a key nothing else will share.
    const id = note.id ?? `${note.start_line}:${note.start_column}:${forFile.size}`;
    const existing = forFile.get(id);
    const merged: HighlightNote = existing
      ? {
          ...existing.note,
          ...note,
          // Neither half overwrites the other with nothing.
          comment: note.comment || existing.note.comment,
          patch: note.patch || existing.note.patch,
        }
      : note;
    forFile.set(id, {note: merged, range: rangeOf(merged)});
    this.held.set(key, forFile);
    this.publish(key);
  }

  /**
   * Drop what a previous run left: one file's, or every one of them.
   *
   * A run replaces what the last one left rather than piling on top of it.
   */
  clear(path?: string): void {
    if (!path) {
      this.held.clear();
      this.collection.clear();
      return;
    }
    const key = vscode.Uri.file(path).toString();
    this.held.delete(key);
    this.collection.delete(vscode.Uri.parse(key));
  }

  /** The suggestions on `document` that cover `range`. */
  covering(document: vscode.TextDocument, range: vscode.Range): Suggestion[] {
    const forFile = this.held.get(document.uri.toString());
    if (!forFile) return [];
    return [...forFile.values()].filter((one) => one.range.intersection(range));
  }

  private publish(key: string): void {
    const forFile = this.held.get(key);
    if (!forFile) return;
    const diagnostics = [...forFile.values()].map((one) => {
      const diagnostic = new vscode.Diagnostic(
        one.range,
        one.note.comment || 'A suggested patch.',
        vscode.DiagnosticSeverity.Information,
      );
      // "reported" means the editor already underlines this range itself, so the
      // source says which of the two this is: a range the model *found* has no
      // squiggle of the editor's own underneath it.
      diagnostic.source =
        one.note.origin === 'found' ? 'a11 review (found)' : 'a11 review';
      if (one.note.patch) {
        diagnostic.tags = undefined;
        diagnostic.code = 'has-patch';
      }
      return diagnostic;
    });
    this.collection.set(vscode.Uri.parse(key), diagnostics);
  }

  dispose(): void {
    for (const one of this.disposables) one.dispose();
  }
}

function rangeOf(note: HighlightNote): vscode.Range {
  return new vscode.Range(
    Math.max(0, note.start_line),
    Math.max(0, note.start_column),
    Math.max(0, note.end_line),
    Math.max(0, note.end_column),
  );
}

/** "Apply this suggestion", where the suggestion brought a patch. */
class SuggestionActions implements vscode.CodeActionProvider {
  constructor(private readonly suggestions: Suggestions) {}

  provideCodeActions(
    document: vscode.TextDocument,
    range: vscode.Range,
  ): vscode.CodeAction[] {
    const actions: vscode.CodeAction[] = [];
    for (const one of this.suggestions.covering(document, range)) {
      if (!one.note.patch) continue;
      const action = new vscode.CodeAction(
        one.note.comment
          ? `Apply: ${firstSentence(one.note.comment)}`
          : 'Apply the suggested patch',
        vscode.CodeActionKind.QuickFix,
      );
      // Through `apply_patch` rather than as a `WorkspaceEdit` built here: that
      // tool is where the context-matching and the refusal-on-a-near-miss live,
      // and a second path into the same file would be a second set of rules.
      action.command = {
        command: 'a11.applySuggestion',
        title: 'Apply the suggested patch',
        arguments: [document.uri.fsPath, one.note.patch],
      };
      actions.push(action);
    }
    return actions;
  }
}

function firstSentence(text: string): string {
  const stop = text.search(/[.!?](\s|$)/);
  const one = stop === -1 ? text : text.slice(0, stop + 1);
  return one.length > 72 ? `${one.slice(0, 69)}...` : one;
}

/** Register the command a suggestion's code action runs. */
export function registerApply(): vscode.Disposable {
  return vscode.commands.registerCommand(
    'a11.applySuggestion',
    async (path: string, patch: string) => {
      try {
        await runByName('apply_patch', {path, patch});
      } catch (error) {
        // A patch that no longer matches is the expected failure — the file has
        // been edited since the review — and the message says what is there now.
        void vscode.window.showWarningMessage(
          error instanceof Error ? error.message : String(error),
        );
      }
    },
  );
}
