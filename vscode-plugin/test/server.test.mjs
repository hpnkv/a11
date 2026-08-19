/**
 * The contract between what the client *spawns* and what the tool *accepts*.
 *
 * The bug this exists for, which cost two rounds to find: `vscode-languageclient`
 * appends `--stdio` to the arguments of its own accord when a server declares
 * `TransportKind.stdio`. `a11-flow` refused unknown options and exited 2 before
 * reading a byte, and the client reported that as "Pending response rejected since
 * connection got disposed" — a sentence about a disposed connection, two layers
 * above an unrecognised flag. Every hand-driven test of the server passed, because
 * none of them passed the flag the client adds.
 *
 * So this drives the binary with **the exact argv the extension causes**, read off
 * the source rather than written down again, and requires a real handshake out of
 * it. It is the one test that would have caught the thing.
 *
 * Skipped, loudly, with no binary for this platform: the extension is meant to
 * degrade to colouring without one, so a failure here would report the absence of a
 * build rather than a fault.
 */

import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {existsSync, readFileSync} from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import {test} from 'node:test';
import {fileURLToPath} from 'node:url';

const root_ = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const root = root_;

/** Where a build for this machine would be, by the extension's own rule. */
function bundled() {
  const arch = process.arch === 'arm64' ? 'aarch64' : 'x86_64';
  const dir =
    os.platform() === 'darwin'
      ? `macos-${arch}`
      : os.platform() === 'win32'
        ? `windows-${arch}`
        : `linux-${arch}`;
  const name = os.platform() === 'win32' ? 'a11-flow.exe' : 'a11-flow';
  return path.join(root, 'bin', dir, name);
}

/**
 * The arguments the extension asks for, read out of the source.
 *
 * Read rather than repeated, so this test is about the real contract: changing the
 * client's `args` without the tool accepting them is what has to fail here.
 */
function declaredArgs() {
  const source = readFileSync(path.join(root, 'src/flowClient.ts'), 'utf8');
  const found = /args:\s*\[([^\]]+)\]/.exec(source);
  assert.ok(found, 'flowClient.ts declares no args for the server');
  return found[1]
    .split(',')
    .map((one) => one.trim().replace(/^['"]|['"]$/g, ''))
    .filter((one) => one.length > 0);
}

/**
 * What the transport adds on top.
 *
 * `TransportKind.stdio` makes the client append `--stdio`. Asserted rather than
 * assumed: if the declaration stops saying `stdio`, this list is wrong and the test
 * should say so rather than quietly checking the wrong argv.
 */
function transportArgs() {
  const source = readFileSync(path.join(root, 'src/flowClient.ts'), 'utf8');
  assert.match(
    source,
    /transport:\s*TransportKind\.stdio/,
    'the transport is no longer stdio, so --stdio may no longer be appended',
  );
  return ['--stdio'];
}

function frame(message) {
  const body = Buffer.from(JSON.stringify(message));
  return Buffer.concat([
    Buffer.from(`Content-Length: ${body.length}\r\n\r\n`),
    body,
  ]);
}

/** Every LSP frame in `buffer`. */
function frames(buffer) {
  const out = [];
  let rest = buffer;
  for (;;) {
    const header = /^Content-Length: (\d+)\r?\n\r?\n/.exec(rest.toString('latin1'));
    if (!header) break;
    const from = header[0].length;
    const length = Number(header[1]);
    out.push(JSON.parse(rest.subarray(from, from + length).toString('utf8')));
    rest = rest.subarray(from + length);
  }
  return out;
}

/** Run the tool with `args`, feed it `messages`, and give back what came out. */
function handshake(executable, args, messages) {
  return new Promise((resolve) => {
    const child = spawn(executable, args);
    const out = [];
    const err = [];
    child.stdout.on('data', (chunk) => out.push(chunk));
    child.stderr.on('data', (chunk) => err.push(chunk));
    child.on('close', (code) =>
      resolve({
        code,
        frames: frames(Buffer.concat(out)),
        stderr: Buffer.concat(err).toString('utf8'),
      }),
    );
    for (const message of messages) child.stdin.write(frame(message));
    child.stdin.end();
  });
}

const executable = bundled();
const missing = !existsSync(executable);
if (missing) {
  console.log(
    `Skipping the server tests: no build at ${executable}. Make one with` +
      ' `cmake --build --preset debug --target a11_flow_tool` and copy it there.',
  );
}

test('the tool accepts the exact argv the client spawns', {skip: missing}, async () => {
  const args = [...declaredArgs(), ...transportArgs()];
  const result = await handshake(executable, args, [
    {jsonrpc: '2.0', id: 0, method: 'initialize', params: {capabilities: {}}},
  ]);
  // The failure this is about: an unknown option exits 2 before reading anything,
  // and the client only ever sees a connection that went away.
  assert.notEqual(
    result.code,
    2,
    `\`a11-flow ${args.join(' ')}\` was refused: ${result.stderr.trim()}`,
  );
  assert.equal(result.frames.length > 0, true, 'the tool answered nothing');
  const answer = result.frames.find((one) => one.id === 0);
  assert.ok(answer, 'no answer to initialize');
  assert.ok(answer.result?.capabilities, 'initialize answered no capabilities');
});

test('a bad flow comes back with its diagnostics', {skip: missing}, async () => {
  const args = [...declaredArgs(), ...transportArgs()];
  const result = await handshake(executable, args, [
    {jsonrpc: '2.0', id: 0, method: 'initialize', params: {capabilities: {}}},
    {jsonrpc: '2.0', method: 'initialized', params: {}},
    {
      jsonrpc: '2.0',
      method: 'textDocument/didOpen',
      params: {
        textDocument: {
          uri: 'file:///probe/broken.flow',
          languageId: 'a11flow',
          version: 1,
          // The fixture's own mistake: a misspelled type.
          text: 'flow t {\n  in q: strig\n}\n',
        },
      },
    },
  ]);
  const published = result.frames.find(
    (one) => one.method === 'textDocument/publishDiagnostics',
  );
  assert.ok(published, 'nothing was published for a flow with an error in it');
  const [first] = published.params.diagnostics;
  assert.ok(first, 'the flow was published as having no problems');
  assert.match(first.code, /^flow\./, 'the code is not the language’s');
  // On the word, not at the top of the file: a range one line out is how a
  // squiggle ends up under the wrong thing.
  assert.equal(first.range.start.line, 1);
  assert.equal(first.range.start.character, 8);
});

test('a completion replaces the partial word and nothing else', {skip: missing}, async () => {
  // The one answer in the protocol that *edits the document*, so the one where the
  // language being wrong costs somebody their file rather than a wrong colour. With
  // nothing typed yet the edit has to be an empty range at the caret; it used to
  // start at line 0 character 0, which replaces everything before it.
  const args = [...declaredArgs(), ...transportArgs()];
  const text =
    'flow inner {\n  in topic: string required "What to research."\n' +
    '  out report: string\n  topic -> report\n}\n\n' +
    'flow outer {\n  in q: string\n  out r: string\n  x = run inner()\n  x.report -> r\n}\n';
  const at = text.lastIndexOf('inner(') + 'inner('.length;
  const line = text.slice(0, at).split('\n').length - 1;
  const character = text.slice(0, at).split('\n').pop().length;

  const result = await handshake(executable, args, [
    {jsonrpc: '2.0', id: 0, method: 'initialize', params: {capabilities: {}}},
    {jsonrpc: '2.0', method: 'initialized', params: {}},
    {
      jsonrpc: '2.0',
      method: 'textDocument/didOpen',
      params: {
        textDocument: {uri: 'file:///probe/t.flow', languageId: 'a11flow', version: 1, text},
      },
    },
    {
      jsonrpc: '2.0',
      id: 1,
      method: 'textDocument/completion',
      params: {textDocument: {uri: 'file:///probe/t.flow'}, position: {line, character}},
    },
  ]);
  const answer = result.frames.find((one) => one.id === 1);
  assert.ok(answer, 'no answer to a completion request');
  const [item] = answer.result.items;
  assert.ok(item, 'nothing was offered inside a call');

  // The port of the flow being called. *How* its description is rendered is the
  // next test's business; this one is about the edit.
  assert.equal(item.label, 'topic');

  // The edit: empty, at the caret.
  const {range} = item.textEdit;
  assert.deepEqual(
    range.start,
    {line, character},
    'the edit does not start at the caret, so taking it would delete text',
  );
  assert.deepEqual(range.end, {line, character});
});

/** The document both completion tests below ask about. */
const CALLING = {
  uri: 'file:///probe/call.flow',
  text:
    'flow inner {\n  in topic: string required "What to research."\n' +
    '  in depth: integer "How many passes."\n' +
    '  out report: string\n  topic -> report\n}\n\n' +
    'flow outer {\n  in q: string\n  out r: string\n  x = run inner(REST)\n' +
    '  x.report -> r\n}\n',
};

/** A completion at the caret marked by `REST`, with `inside` written before it. */
async function completeInCall(capabilities, inside = '') {
  const text = CALLING.text.replace('REST', inside);
  const at = text.lastIndexOf('inner(') + 'inner('.length + inside.length;
  const line = text.slice(0, at).split('\n').length - 1;
  const character = text.slice(0, at).split('\n').pop().length;
  const result = await handshake(executable, [...declaredArgs(), ...transportArgs()], [
    {jsonrpc: '2.0', id: 0, method: 'initialize', params: {capabilities}},
    {jsonrpc: '2.0', method: 'initialized', params: {}},
    {
      jsonrpc: '2.0',
      method: 'textDocument/didOpen',
      params: {
        textDocument: {uri: CALLING.uri, languageId: 'a11flow', version: 1, text},
      },
    },
    {
      jsonrpc: '2.0',
      id: 1,
      method: 'textDocument/completion',
      params: {textDocument: {uri: CALLING.uri}, position: {line, character}},
    },
  ]);
  return {
    initialize: result.frames.find((one) => one.id === 0)?.result,
    items: result.frames.find((one) => one.id === 1)?.result.items ?? [],
  };
}

test('opening a call asks for its ports', {skip: missing}, async () => {
  // `(` and `,` have to be trigger characters or the editor never asks. Empty
  // parentheses are the case that matters: nothing has been typed, so no
  // word-based trigger fires either, and the ports of the action being called --
  // the thing hardest to remember -- were the one list that needed Ctrl+Space.
  const {initialize} = await completeInCall({});
  const triggers = initialize.capabilities.completionProvider.triggerCharacters;
  for (const one of ['(', ',']) {
    assert.ok(triggers.includes(one), `\`${one}\` is not a trigger character`);
  }
});

test('a port is offered with what it is for, however the client renders', {skip: missing}, async () => {
  // `labelDetails` is opt-in: a client that did not ask for it may ignore the
  // field, and then the row says only the port's name. So the text goes wherever
  // the client will show it.
  const rich = await completeInCall({
    textDocument: {completion: {completionItem: {labelDetailsSupport: true}}},
  });
  assert.deepEqual(rich.items.map((one) => one.label), ['topic', 'depth']);
  assert.match(rich.items[0].labelDetails.detail, /\(required\) — What to research\./);
  assert.equal(rich.items[0].detail, 'string');

  const plain = await completeInCall({});
  assert.equal(plain.items[0].labelDetails, undefined);
  // Both, in the one field this client reads, and with no doubled space.
  assert.equal(plain.items[0].detail, 'string (required) — What to research.');
  assert.equal(plain.items[1].detail, 'integer How many passes.');

  // The whole description is in the popup either way.
  for (const one of [rich, plain]) {
    assert.match(one.items[0].documentation.value, /What to research\./);
  }
});

test('after a comma the ports not yet given are offered', {skip: missing}, async () => {
  const {items} = await completeInCall({}, 'topic: q, ');
  assert.deepEqual(items.map((one) => one.label), ['depth']);
  assert.match(items[0].detail, /How many passes\./);
});

test('an action the project declares is offered, not just hoverable', {skip: missing}, async () => {
  // The whole point of reading the project: an action somebody wrote this
  // afternoon should be *suggested*, not merely explained once you have typed its
  // name from memory. Hover and go-to-declaration were given the session's
  // catalogue and completion was not, so the one gesture that does not require
  // already knowing the name was the one that did not know it.
  const root = path.resolve(root_, '..', 'a11', 'demos');
  const text =
    'flow use {\n  in text: string required\n  out out: string\n' +
    '  s = run \n  s.lines -> out\n}\n';
  const at = text.indexOf('run ') + 'run '.length;
  const line = text.slice(0, at).split('\n').length - 1;
  const character = text.slice(0, at).split('\n').pop().length;

  const result = await handshake(executable, [...declaredArgs(), ...transportArgs()], [
    {jsonrpc: '2.0', id: 0, method: 'initialize', params: {capabilities: {}}},
    {jsonrpc: '2.0', method: 'initialized', params: {}},
    // What the extension does when a workspace opens.
    {jsonrpc: '2.0', id: 9, method: 'a11flow/scan', params: {paths: [root]}},
    {
      jsonrpc: '2.0',
      method: 'textDocument/didOpen',
      params: {
        textDocument: {uri: 'file:///probe/use.flow', languageId: 'a11flow', version: 1, text},
      },
    },
    {
      jsonrpc: '2.0',
      id: 1,
      method: 'textDocument/completion',
      params: {textDocument: {uri: 'file:///probe/use.flow'}, position: {line, character}},
    },
  ]);

  const scanned = result.frames.find((one) => one.id === 9)?.result;
  assert.ok(scanned?.actions > 0, `the scan of ${root} found nothing`);

  const items = result.frames.find((one) => one.id === 1)?.result.items ?? [];
  const offered = new Map(items.map((one) => [one.label, one]));
  // One the repository really declares, in Python, with a description.
  const own = offered.get('split_lines');
  assert.ok(own, `split_lines was not offered; got ${[...offered.keys()].join(', ')}`);
  assert.match(own.detail, /non-empty lines/);
  // Its ports and where it was written, which is what the scan adds over a name.
  assert.match(own.documentation.value, /\*\*Inputs\*\*/);
  assert.match(own.documentation.value, /Declared in/);
  assert.match(own.documentation.value, /split_lines\.py/);

  // And the SDK's own actions are still there: a scan *adds* to what the tool
  // knows rather than replacing it.
  assert.ok(offered.has('interact_with_llm'), 'the embedded snapshot was lost');
});

test('an unknown option is still refused', {skip: missing}, async () => {
  // The flag is accepted because a client sends it, not because anything goes:
  // a typo in a `toolPath` argument should still be a complaint.
  const result = await handshake(executable, ['serve', '--protocol', 'lsp', '--nonsense'], []);
  assert.equal(result.code, 2, 'an unknown option was accepted');
  assert.match(result.stderr, /--nonsense/);
});
