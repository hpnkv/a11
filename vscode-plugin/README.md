# A11 Flow & Chat for VSCode

The [A11 Flow](../doc/docs/guides/flow.md) language, and an AI chat that can call
tools backed by your editor — powered by [A11](https://github.com/hpnkv/a11).

This is the second client of one language, not a second implementation of it. The
[JetBrains plugin](../intellij-plugin) is the first, and the two share everything
it makes sense to share: the whole language, and the whole chat UI.

## The A11 Flow language

`.flow` files, and flows written **inside string literals**, which is where most of
them live because a flow is meant to travel as text.

```python
program = flow.loads("""
    flow shout {                          # highlighted, checked and completed
      in  words:   string stream
      out loudest: string

      say = call text-upper(text: words)
      say.upper | first 1 -> loudest
    }
""")
```

You get diagnostics with quick fixes, semantic highlighting, formatting,
completion, hover, document symbols and go-to-declaration. None of it is
implemented here.

### Where the language actually is

Nothing in this extension knows what a flow means. The lexer, parser, resolver,
inspector, formatter, completer — and the scanner that reads your project's own
actions — are `cpp/a11/flow/`, and this extension runs them:

```
extension.ts ──▶ a11-flow serve --protocol lsp      (one process per window)
   │
   ├── vscode-languageclient   diagnostics + quick fixes, semantic tokens,
   │                           formatting, completion, hover, symbols, definition
   ├── fragments.ts            the same, for flows inside string literals
   └── a11flow/scan            what actions this workspace declares
```

So a stage added to the grammar is a stage this extension colours, offers and
checks with no change here at all — and what it reports is word for word what
`a11 flow check` and CI report, because it is the same code.

The binary comes from `bin/<os>-<arch>/a11-flow` in the extension, or from the
`a11.flow.toolPath` setting, or from the `PATH`. Build one with:

```sh
cmake --build --preset debug --target a11_flow_tool
mkdir -p vscode-plugin/bin/macos-aarch64
cp build/debug/cpp/a11-flow vscode-plugin/bin/macos-aarch64/
```

**With no binary for the platform**, the editor degrades on purpose rather than
breaking: a `.flow` is still coloured by the TextMate grammar, which knows the
language's words and nothing about what they mean, and one notification says why.

### The grammars are generated

`syntaxes/` is copied at build time from `editors/vscode/`, which
`a11 flow syntax --target vscode` writes from the language's own word tables. A
hand-written grammar would be a copy of those tables, and a copy falls behind; CI
gates on `a11 flow syntax`, which exits 1 when nobody has regenerated it.

There are two: the grammar for `.flow` files, and an **injection** grammar that
colours a flow inside a host language's string. Both are the fallback rather than
the whole story — the real colours come from the server's semantic tokens, which
are the language's own judgement about every token.

### Actions your project declares

The language ships a snapshot of what the A11 SDK registers, so hovering
`interact_with_llm` has always said something useful. An action you wrote this
afternoon is in no snapshot.

So the extension reads the workspace's own `.py`, `.cc` and `.ts` for
`ActionSchema` declarations — on open, and again a moment after you save one — and
tells the language what it found. Hovering that action then shows its description
and every port it has, completion offers its ports by name, and **F12 goes to the
declaration**, in whichever language it was written in.

It is a tolerant textual read rather than a parser for three languages, and it is
happy to come away with less than everything. A schema written as a constructor
call with literal arguments comes back whole; one assembled statement by statement
(the C++ shape) comes back with its name, description and port names but thinner
port types; a name computed at run time is not found at all. `a11 flow scan` prints
exactly what it can see, which is the way to check.

Turn it off with `a11.flow.scanWorkspace` if a very large workspace makes it
expensive.

## The chat, and the editor's tools

An **A11** container in the activity bar with two views:

- **Chat** — talks to the **A11 gateway** (`a11 gateway`) over a WebSocket and
  streams a reply. The model can call tools backed by this editor as well as the
  gateway's own `shell_*` tools, so answers are grounded in your actual project.
- **Actions** — an explorer that lists every tool this editor exposes and runs it
  with arbitrary JSON input, independent of the model and the gateway. Good for
  seeing what a tool actually returns.

Ten tools, the same ten the JetBrains plugin has and with the same names,
descriptions and coordinate conventions: `get_active_file`, `get_open_editors`,
`get_selection`, `get_file_symbols`, `read_file`, `apply_patch`,
`get_error_highlights`, `rename_symbol`, `find_file`, `search_project`.

`apply_patch` takes a path and a unified diff and applies it as **one edit**, so a
single Undo reverses the whole patch. Hunks are placed by their context rather than
by the numbers in their `@@` header, and a hunk that does not match is refused with
what is there instead: nothing is applied on a near miss, because a fuzzy match is
how a tool silently rewrites the wrong lines.

## What is shared with the JetBrains plugin, and what is not

**Shared, in [`../webview`](../webview).** The whole UI: the chat, the action
explorer, the conversation list, the markdown renderer, the forms built from a port
schema, and the `index.html` they are styled by. It is ordinary browser code behind
six methods — `listActions`, `runAction`, `getConfig`, `readFlow`,
`suggestOnHighlight`, `clearSuggestions` — and each editor implements those its own
way. That seam is why one UI serves two editors.

**Shared, in `cpp/a11/flow/`.** The language, and the scanner. Both editors run the
same binary and read the same versioned envelopes
([the formats](../doc/docs/guides/flow-tooling.md)).

**Deliberately not shared.** Three things, and in each case forcing it would mean
shipping something worse:

- **The IDE tools.** `IdeTools.kt` is Kotlin against the IntelliJ platform;
  `src/tools/` is TypeScript against VSCode's. There is no version of "reuse" here
  that is not either running a JVM inside an extension or pretending two editors
  have one API. What *is* shared is the descriptor JSON, which is the contract the
  model and the UI read.
- **The patch algorithm.** Same decisions, same test cases, two implementations,
  because it is written against two different document APIs. The test fixtures in
  `test/patch.test.mjs` are the cases `Patch.kt` makes.
- **Embedded flows.** The JetBrains plugin registers a `MultiHostInjector` and the
  platform then treats a fragment as a real document. VSCode has no equivalent, so
  this extension colours fragments with an injection grammar and asks the language
  about them as text, translating offsets by where the fragment starts. Same
  capability, each platform's own means.

The suggestion highlights are the fourth: the JetBrains side renders a popup of its
own, and here a review comment is a diagnostic and its patch is a code action —
same information, this editor's own affordance.

## Build & run

Prerequisites: **Node.js ≥ 20**, and a Python env with `a11` installed for the
gateway (`pip install "a11-kit[llm]"` plus `websockets`).

```sh
cd vscode-plugin
npm ci
npm run build          # both bundles, plus the generated grammars and index.html
npm run typecheck      # the extension and the webview host
npm test               # the patch algorithm and the fragment spans, in plain Node
```

Then **open this folder in VSCode and press F5** — `.vscode/launch.json` runs the
extension in a second window with `test/fixtures` open, which is a workspace with a
broken `.flow`, a flow inside a Python string, and an `ActionSchema` to discover.

From a shell instead:

```sh
code --extensionDevelopmentPath="$(pwd)" test/fixtures
```

`--extensionDevelopmentPath` has to name **this** folder — the one with
`package.json` in it. Point it at the repository root and VSCode finds no
`package.json` there, decides the path is a *directory of extensions*, scans one
level down and tries to load `js/` and `webview/` as extensions too:

```
Failed loading extensions '.../js', '.../webview' under development because they are
invalid: property `engines.vscode` is mandatory ...
```

Those two are ordinary npm packages, not extensions. The extension itself loads
fine in that case, so the message is noise rather than a failure — but F5, or the
`$(pwd)` above from this directory, avoids it.

Start the gateway, from a Python env with `a11` installed:

```sh
a11 gateway            # ws://127.0.0.1:8011/a11
```

Then **A11: Set the provider API key** from the command palette — it goes into this
machine's keychain through `context.secrets`, never into `settings.json` — and open
the **A11** container.

### Settings

Under **A11** in the settings UI:

| Setting | Means |
| --- | --- |
| `a11.gatewayUrl` | Where `a11 gateway` listens. A bare `host:port` is completed. |
| `a11.provider` / `a11.model` / `a11.baseUrl` | Which model answers. |
| `a11.extraAllowedTools` | Patterns for the gateway's *own* tools, `shell_.*` by default. Emptying this turns the shell off. |
| `a11.flow.toolPath` | Where `a11-flow` is, when it is not bundled or on the `PATH`. |
| `a11.flow.scanWorkspace` | Read the workspace for its own actions. |
| `a11.flow.checkFragments` | Check flows inside string literals, not only `.flow` files. |

### Commands

`A11: New chat`, `A11: Set the provider API key`,
`A11: Forget the provider API key`, `A11: Re-read the workspace's actions`,
`A11: Restart the Flow language server`, `A11: Clear review suggestions`.

## Testing

- `npm test` — the two pure modules, in plain Node with no editor: the patch
  algorithm and the fragment spans. Those are the two places an off-by-one does
  real damage, which is why they are separated from anything that needs `vscode`.
- `npm run test:integration` — the real editor, through `@vscode/test-electron`: the
  extension activates, a bad `.flow` comes back with `flow.*` diagnostics from a
  server this extension started, hovering a `->` explains what it does, and a flow
  inside a Python string is checked *at the right offset*. It skips itself, loudly,
  when there is no `a11-flow` on the machine, since the extension is meant to
  degrade to colouring without one.
- What a *word means* is tested in `cpp/tests/` — one lexer, one set of
  expectations — so nothing here re-tests the language.

## Status

The language half is exercised end to end. The chat path follows the same
Session/Action semantics the JetBrains plugin's tests prove and the TypeScript
library's own tests cover, but exercising it against a real model needs a provider
API key.
