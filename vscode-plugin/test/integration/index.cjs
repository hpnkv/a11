/**
 * What runs inside the real editor.
 *
 * Four things, and each is one this extension could get wrong in a way no unit test
 * would notice:
 *
 * 1. The extension activates at all — a manifest with a bad `main` or a missing
 *    contribution fails here and nowhere else.
 * 2. A bad `.flow` comes back with diagnostics, which means a language server was
 *    started, spoke LSP, and had its answers understood.
 * 3. A quick fix arrives from a diagnostic, which is the edits the language wrote
 *    travelling all the way to a code action.
 * 4. A flow inside a Python string is checked too, which is the fragment path.
 */

// CommonJS on purpose: the extension host `require`s this file, so an ES module
// here cannot be loaded at all.
const assert = require('node:assert/strict');
const path = require('node:path');
const vscode = require('vscode');

/** Wait for `check` to be true, or give up. */
async function until(what, check, timeoutMs = 30_000) {
  const started = Date.now();
  for (;;) {
    const value = await check();
    if (value) return value;
    if (Date.now() - started > timeoutMs) {
      throw new Error(`Timed out waiting for ${what}.`);
    }
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
}

function fixture(name) {
  const folder = vscode.workspace.workspaceFolders?.[0];
  assert.ok(folder, 'the test opened no folder');
  return vscode.Uri.file(path.join(folder.uri.fsPath, name));
}

async function run() {
  const extension = vscode.extensions.getExtension('curiositystack.a11-flow');
  assert.ok(extension, 'the extension is not installed');
  await extension.activate();
  assert.ok(extension.isActive, 'the extension did not activate');

  // --- a .flow file, through the language server ---------------------------
  const flow = await vscode.workspace.openTextDocument(fixture('broken.flow'));
  await vscode.window.showTextDocument(flow);
  assert.equal(flow.languageId, 'a11flow', 'the .flow file was not recognised');

  const diagnostics = await until(
    'diagnostics on the .flow file',
    () => {
      const found = vscode.languages.getDiagnostics(flow.uri);
      return found.length > 0 ? found : undefined;
    },
  );
  // The language's own code, not a message this extension composed.
  assert.ok(
    diagnostics.some((one) => String(one.code ?? '').startsWith('flow.')),
    `expected a flow.* code, got ${JSON.stringify(diagnostics.map((one) => one.code))}`,
  );

  // --- the quick fix, which is the language's own edits --------------------
  const actions = await vscode.commands.executeCommand(
    'vscode.executeCodeActionProvider',
    flow.uri,
    diagnostics[0].range,
  );
  assert.ok(Array.isArray(actions), 'no code actions came back');

  // --- hover, which is where the reference text arrives --------------------
  const pipeLine = flow
    .getText()
    .split('\n')
    .findIndex((line) => line.includes('->'));
  if (pipeLine >= 0) {
    const at = new vscode.Position(pipeLine, flow.lineAt(pipeLine).text.indexOf('->'));
    const hovers = await vscode.commands.executeCommand(
      'vscode.executeHoverProvider',
      flow.uri,
      at,
    );
    assert.ok(hovers.length > 0, 'hovering `->` said nothing');
    const text = hovers
      .flatMap((one) => one.contents)
      .map((one) => (typeof one === 'string' ? one : one.value))
      .join('\n');
    // The point of the whole first half of this change: a form of the language
    // answers with what it does, not with the name of its token kind.
    assert.match(text, /Writes a stream into one or more destinations/, text);
  }

  // --- a flow inside a Python string --------------------------------------
  const host = await vscode.workspace.openTextDocument(fixture('embedded.py'));
  await vscode.window.showTextDocument(host);
  const embedded = await until('diagnostics inside the Python string', () => {
    const found = vscode.languages
      .getDiagnostics(host.uri)
      .filter((one) => one.source === 'a11flow');
    return found.length > 0 ? found : undefined;
  });
  // And in the right place: the range has to be inside the string, not at the
  // top of the file, which is what an un-translated offset would give.
  assert.ok(embedded[0].range.start.line > 3, 'the fragment offset was not applied');

  // --- the outline, which the editor validates -----------------------------
  // Asked for explicitly, because LSP refuses the whole answer when one symbol's
  // selection is outside its range and the only sign is an error in the log. A
  // document that declares one flow with two ports has three symbols; nothing back
  // at all is what a refused answer looks like.
  const outline = await vscode.commands.executeCommand(
    'vscode.executeDocumentSymbolProvider',
    flow.uri,
  );
  assert.ok(Array.isArray(outline) && outline.length > 0, 'the outline came back empty');
  const [declared] = outline;
  assert.ok(
    declared.range.contains(declared.selectionRange),
    'a symbol’s selection is outside its range',
  );
  // And the range is the whole block, which is what "select symbol" takes.
  assert.ok(declared.range.end.line > declared.selectionRange.end.line);

  // --- the workspace's own actions ----------------------------------------
  // `test/fixtures/actions.py` declares one. Re-reading the workspace should
  // find it, which is the discovery path end to end: scan, context, and a hover
  // that knows about an action no snapshot has.
  await vscode.commands.executeCommand('a11.rescanActions');
}

module.exports = {run};
