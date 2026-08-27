/**
 * Flows written inside another language's string literals.
 *
 * **Why this exists.** Most flows do not
 * live in `.flow` files. A flow is meant to
 * travel as text, so it is usually written where it is handed over:
 * `flow.loads("""...""")` in Python, a
 * template literal in TypeScript. An editor
 * that only understood `.flow` would understand almost none of the flows
 * anybody writes.
 *
 * JetBrains uses `MultiHostInjector` to expose fragments as injected language
 * documents. VS Code has no equivalent, so an injection grammar highlights the
 * fragment while this module sends its text to the language service and rebases
 * returned offsets into the host document.
 *
 * **The rule for what counts.** The same
 * one the JetBrains injector uses: a string
 * whose first real word is `flow`, followed by a name and a `{`. Prose merely
 * mentioning a flow is not one, in either editor.
 */

import * as vscode from 'vscode';
import {fragmentSpans} from './fragmentSpans.js';
import type {FlowServer} from './flowClient.js';

/** One flow found inside a host document. */
export interface Fragment {
  /** The text of the flow itself, without the quotes around it. */
  text: string;
  /** Where that text starts in the host document. */
  start: vscode.Position;
  /** The whole fragment, for a range that has to cover it. */
  range: vscode.Range;
  /** The offset of `start` in the host document, in UTF-16 units. */
  offset: number;
}

/** Every flow written in `document`. */
export function fragmentsIn(document: vscode.TextDocument): Fragment[] {
  const text = document.getText();
  return fragmentSpans(text).map((span) => ({
    text: text.slice(span.start, span.end),
    start: document.positionAt(span.start),
    range: new vscode.Range(
      document.positionAt(span.start),
      document.positionAt(span.end),
    ),
    offset: span.start,
  }));
}

/** The fragment containing `position`, if any. */
export function fragmentAt(
  document: vscode.TextDocument,
  position: vscode.Position,
): Fragment | undefined {
  return fragmentsIn(document).find((one) => one.range.contains(position));
}

/**
 * A position inside a fragment, as an offset into the fragment's own text.
 *
 * The whole of the translation, and it is an addition rather than a mapping: a
 * fragment is one contiguous run of the host document, so its offset zero is
 * the host's `fragment.offset`. This is why the module needs no virtual
 * documents.
 */
export function offsetIn(
  document: vscode.TextDocument,
  fragment: Fragment,
  position: vscode.Position,
): number {
  return document.offsetAt(position) - fragment.offset;
}

/** A range of the fragment, as a range of the host document. */
export function rangeInHost(
  document: vscode.TextDocument,
  fragment: Fragment,
  start: number,
  end: number,
): vscode.Range {
  return new vscode.Range(
    document.positionAt(fragment.offset + start),
    document.positionAt(fragment.offset + end),
  );
}

/** How a diagnostic's severity reads to this editor. */
function severityOf(name: string): vscode.DiagnosticSeverity {
  switch (name) {
    case 'error':
      return vscode.DiagnosticSeverity.Error;
    case 'warning':
      return vscode.DiagnosticSeverity.Warning;
    case 'information':
      return vscode.DiagnosticSeverity.Information;
    default:
      // `weak-warning` is the language's fourth level and this editor's third:
      // "this works, and part of it is doing nothing" is a hint.
      return vscode.DiagnosticSeverity.Hint;
  }
}

interface Envelope {
  ok?: boolean;
  result?: {
    diagnostics?: Array<{
      code: string;
      severity: string;
      message: string;
      range: {start: {offset: number}; end: {offset: number}};
      fixes?: Array<{label: string; edits: Array<{start: number; end: number; text: string}>}>;
    }>;
    proposals?: Array<{
      name: string;
      kind: string;
      insert: string;
      tail?: string;
      type?: string;
      documentation?: string;
      caret?: number;
    }>;
    prefix_start?: number;
    found?: boolean;
    markdown?: string;
    range?: {start: {offset: number}; end: {offset: number}};
  };
}

/**
 * Everything wrong with the flows in one document, as this editor's
 * diagnostics.
 *
 * Each diagnostic carries its fixes. The code-action provider preserves those
 * edits so the frontend does not derive a separate repair from the message.
 */
export async function checkFragments(
  server: FlowServer,
  document: vscode.TextDocument,
): Promise<vscode.Diagnostic[]> {
  const out: vscode.Diagnostic[] = [];
  for (const fragment of fragmentsIn(document)) {
    const answer = (await server.request({
      method: 'check',
      source: fragment.text,
      offsets: 'utf16',
    })) as Envelope | undefined;
    for (const one of answer?.result?.diagnostics ?? []) {
      const diagnostic = new vscode.Diagnostic(
        rangeInHost(document, fragment, one.range.start.offset, one.range.end.offset),
        one.message,
        severityOf(one.severity),
      );
      diagnostic.source = 'a11flow';
      diagnostic.code = one.code;
      // Carried, not re-derived. Read back by the code-action provider.
      (diagnostic as FixCarrier).a11Fixes = (one.fixes ?? []).map((fix) => ({
        label: fix.label,
        edits: fix.edits.map((edit) => ({
          range: rangeInHost(document, fragment, edit.start, edit.end),
          text: edit.text,
        })),
      }));
      out.push(diagnostic);
    }
  }
  return out;
}

/** A diagnostic with the language's own fixes on it. */
export interface FixCarrier extends vscode.Diagnostic {
  a11Fixes?: Array<{
    label: string;
    edits: Array<{range: vscode.Range; text: string}>;
  }>;
}

/** What may be written at a position inside a fragment. */
export async function completeInFragment(
  server: FlowServer,
  document: vscode.TextDocument,
  position: vscode.Position,
): Promise<vscode.CompletionItem[]> {
  const fragment = fragmentAt(document, position);
  if (!fragment) return [];
  const answer = (await server.request({
    method: 'complete',
    source: fragment.text,
    offset: offsetIn(document, fragment, position),
    offsets: 'utf16',
  })) as Envelope | undefined;
  const proposals = answer?.result?.proposals ?? [];
  return proposals.map((proposal, index) => {
    const item = new vscode.CompletionItem(proposal.name);
    item.insertText = proposal.insert;
    item.detail = proposal.type || proposal.tail?.trim();
    if (proposal.documentation) {
      item.documentation = new vscode.MarkdownString(proposal.documentation);
    }
    // The language returns them in the order they should be offered, and this
    // keeps that order: a list re-sorted alphabetically would bury the stage
    // somebody most likely wanted under the one that starts with `b`.
    item.sortText = String(index).padStart(4, '0');
    return item;
  });
}

/** What is at a position inside a fragment. */
export async function hoverInFragment(
  server: FlowServer,
  document: vscode.TextDocument,
  position: vscode.Position,
): Promise<vscode.Hover | undefined> {
  const fragment = fragmentAt(document, position);
  if (!fragment) return undefined;
  const answer = (await server.request({
    method: 'describe',
    source: fragment.text,
    offset: offsetIn(document, fragment, position),
    offsets: 'utf16',
  })) as Envelope | undefined;
  const about = answer?.result;
  if (!about?.found || !about.markdown) return undefined;
  const range = about.range
    ? rangeInHost(document, fragment, about.range.start.offset, about.range.end.offset)
    : undefined;
  return new vscode.Hover(new vscode.MarkdownString(about.markdown), range);
}
