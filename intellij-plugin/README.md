# A11 Chat for JetBrains IDEs

An AI chat tool window for JetBrains IDEs — IntelliJ IDEA, PyCharm, CLion,
GoLand, Rider, WebStorm, ... — powered by [A11](https://github.com/hpnkv/a11).
The chat UI is a **JCEF** (embedded Chromium) page that runs the **TypeScript
A11 library** and talks to the **A11 gateway** (`a11 gateway`) over a WebSocket,
letting the model call **tools backed by the IDE** — open editors, the current
selection, and project file indexes — as well as the gateway's own **shell
tools**, so answers are grounded in your actual project.

Nothing in the plugin is product-specific: it declares only
`com.intellij.modules.platform` and uses platform API alone, so it installs in
any IDE from build 243 (2024.3) up. CLion is only the *development* target —
what it is compiled and `runIde`-ed against (`platformVersion` in
`gradle.properties`).

The gateway is a service you run; the plugin has no backend of its own and does
not start one. It is expected at `ws://127.0.0.1:8011/a11`, which is where
`a11 gateway` listens by default, and the URL is a setting.

There is also an **A11 Actions** tool window: an action explorer that lists every
tool the IDE exposes and lets you run it with arbitrary JSON input, for fast
manual testing — independent of the LLM and the gateway.

## The A11 Flow language

The plugin also brings [A11 Flow](../doc/docs/guides/flow.md) — the language for
describing a composition of actions — to the IDE: `.flow` files, and flows
written **inside string literals**, which is where most of them live because a
flow is meant to travel as text.

```python
program = flow.loads("""
    flow shout {                          # highlighted from here
      in  words:   string stream
      out loudest: string

      say = call text-upper(text: words)
      say.upper | first 1 -> loudest
    }
""")
```

Nothing has to be configured: a string whose first real word is `flow` followed
by a name and a `{` is treated as one, whatever language the file is in. That is
the same treatment SQL gets in a Python string, reached the same way — a
`MultiHostInjector` registered for `PsiLanguageInjectionHost` itself, so it is
one implementation for every host language and every IDE. For a fragment the
content cannot vouch for, the platform's own markers still work, because the
language is registered like any other:

```python
# language=A11Flow
fragment = "shout.upper | first 1 -> loudest"
```

`@Language("A11Flow")` does the same for a Java or Kotlin host, and Alt+Enter →
*Inject language or reference* → **A11 Flow** for anything else.

What a word means depends on the tokens around it, which the lexer tracks: a
word is a stage after a `|`, a function only where it is called, and whatever
follows a `.` is a member however it is spelled — so `page.text` reads as a port
and `| truncate 200` as a stage. The two stages that may drop the pipe are read
from what follows instead: `then` and `where` are stages with an operand after
them (`history then asked`), and names without one, which is what keeps a port
really called `then` highlighted as the port it is. `node` is the same kind of
rule — the keyword only where its parentheses open, as in `said = node()`. A port declaration is a context of
its own, from `in`/`out` to the end of the line, which is what lets a type be
written as the tag a serialisation registry knows it by:

```
in  frames: list[a11.NodeFragment] stream
out audio:  a11.sdk.AudioBuffer stream required
```

Everything up to `stream`/`required` is the type, dots and brackets included —
there is no list of tags to fall behind, because the position says it is one.
Colours are all customisable under
Settings | Editor | Color Scheme | **A11 Flow**.

### Where the language actually is

Nothing in this plugin knows what a flow means. There used to be 4,000 lines of
Kotlin that did — a lexer, a permissive parser, a resolver, five inspections and
five word lists, each of which had to be taught every word the compiler learned —
and all of it is gone. The language is `cpp/a11/flow/`, and the plugin runs it:

```
FlowEngine ──▶ a11-flow serve --protocol json      (one process per IDE)
   │
   ├── FlowLexer            replays the token stream: colour, braces, folding
   ├── FlowAnnotator        the diagnostics, and Alt+Enter from their own edits
   ├── FlowFormattingService  Ctrl+Alt+L
   └── FlowCompletionContributor  what may be written at the caret
```

So a stage added to the grammar is a stage this plugin colours, offers and checks
with no change here at all — and what it reports is word for word what
`a11 flow check` and CI report, because it is the same code.

The binary comes from `bin/<os>-<arch>/a11-flow` in the plugin's resources, or from
`-Da11.flow.tool=...`/`A11_FLOW_TOOL`, or from the path. Build one with

```sh
cmake --build --preset debug --target a11_flow_tool
mkdir -p intellij-plugin/bin/macos-aarch64
cp build/debug/cpp/a11-flow intellij-plugin/bin/macos-aarch64/
```

**With no binary for the platform**, the editor degrades on purpose rather than
breaking: comments, strings, numbers and punctuation are still coloured (the one
lexing rule left in Kotlin knows the *shape* of a flow and none of its words),
completion and diagnostics are silent, and one balloon says why.

### What it tells you about a flow

Completion is position-driven, because that is how the language works: stages after
a `|`, call modifiers after a `)`, types after a port's `:`, status codes after
`fail`, node maps after `via`, a sibling flow's input ports inside its argument
list, and after `x.` whatever `x` actually is — a call's ports, a node's `id`, the
fields of a status. `it` is offered inside `where`/`map`/`group` and nowhere else,
and a name is never offered above the statement that binds it. All of that is
decided by `CompleteAt` in the language; the plugin turns the answers into lookup
elements.

**Actions this project declares** are found by reading it. The language ships a
snapshot of what the SDK registers, so hovering `interact_with_llm` has always said
something useful; an action somebody wrote this afternoon was in no snapshot, so
hovering it said "action name" and Ctrl+B had nowhere to go. `a11-flow scan` reads
the project's own `.py`, `.cc` and `.ts` for `ActionSchema` declarations — on open,
and again a moment after one is saved — and `FlowCatalogueService` hands the result
to `FlowEngine`. Hovering that action then shows its description and every port it
has, completion offers its ports, and **Ctrl+B goes to the declaration**, in
whichever language it was written in. A tolerant textual read rather than a parser
for three languages: `a11 flow scan` prints exactly what it can see, which is the
way to check what it made of a particular schema.

Diagnostics arrive as one external annotator rather than as inspections, because
the checks, their messages, their severities and their families are the language's:
a malformed form, an unknown or misused name, an impossible or redundant sequence,
a barrier or loop tail that cannot hold, and — the one worth having — a `try` whose
status nothing reads. `a11 flow codes` lists every one of them.

Quick fixes are the edits the diagnostic came with, applied as they are. Nothing
here works out what a repair should be: a fix that re-derived it would be a second
implementation of the check, and would corrupt a file the day the two disagreed.

## Architecture

```
 ┌─────────────────────────────────────────┐                         ┌──────────────────────┐
 │  JCEF page (Chromium, in the IDE)        │   WebSocket (RFC 6455)  │  a11 gateway          │
 │   TypeScript A11 lib (@curiositystack/a11)│◀──────────────────────▶│  (a11/gateway/)       │
 │   • WebSocketWireStream + Session         │   chat ────────────────▶│  interact_with_llm    │
 │   • IDE-tool registry (reverse dispatch)  │◀── tool call ───────────│   + RemoteToolBridge  │
 └───────┬───────────────────────────────────┘                         │   + shell_* tools     │
         │                                                             └──────────┬───────────┘
         │ window.__a11Bridge (JBCefJsQuery)                                        │ provider SDK
         ▼                                                                          ▼ Claude / Gemini / Ollama
 ┌────────────────────────────┐
 │   IDE plugin (Kotlin/JVM)   │   IdeTools.runByName / listDescriptors / getConfig
 │   • IdeTools (IDE handlers) │   ← single source of truth for tool schemas + logic
 │   • A11 Kotlin runtime kept │   ← still available for richer Kotlin-driven UX
 └────────────────────────────┘
```

- The **web UI** lives in [`../webview`](../webview) and is **shared with the
  VSCode extension**: the chat, the action explorer, the conversation list, the
  markdown renderer, the forms built from a port schema, and the `index.html` they
  are styled by. It imports the TypeScript A11 library from [`../js`](../js) and
  owns the WebSocket to the gateway directly (no Kotlin broker). Chat renders
  markdown turns, streams tokens, auto-scrolls, and shows a live "thinking"
  affordance.
  [`webview/`](webview) *here* is this host's half: an entry point that installs
  the JCEF bridge, bundled with esbuild into `src/main/resources/webview/app.js`.
  The seam between them is six methods (`bridge.ts`) — `listActions`, `runAction`,
  `getConfig`, `readFlow`, `suggestOnHighlight`, `clearSuggestions` — which is the
  whole of what the UI asks of an editor, and why one UI serves two.
- The **A11 compatibility layer** in [`../kotlin`](../kotlin) — a byte-compatible
  Kotlin port of the A11 client runtime — is **kept intact** and still consumed
  as a Gradle composite build, so future Kotlin-driven experiences can reuse it.
- **Tool bridge:** [`IdeTools`](src/main/kotlin/dev/curiositystack/a11/clion/tools/IdeTools.kt)
  is the single source of truth for the IDE tools. The page mirrors the schemas
  *dynamically* from `listDescriptors()` and delegates execution back to Kotlin
  via `window.__a11Bridge.runAction` (a `JBCefJsQuery`); the same path serves both
  the model's reverse-dispatched tool calls and the action explorer.
- **Both ends' tools, one flat list.** The IDE's tools are announced to the
  gateway (`__register_tools__`) and served here; the gateway's own — its
  `shell_*` tools — are added by the gateway for every registered name the
  **allowed-tools header** matches, and run there. That header is the IDE tool
  names plus the patterns from the *Extra allowed tools* setting (`shell_.*` by
  default), so turning the shell off is emptying a field.
- **A tool's run log reaches the gateway but never the model.** Every action has
  a reserved log port that no schema declares, and a handler narrates itself onto
  it with `log()`. The IDE tools return their narration under `RUN_LOG_KEY` and
  the A11 handler logs it; the gateway's shell tools call `log()` directly. The
  LLM **tool runner** reads that port separately from the action's outputs, keeps
  it out of the tool result, and files it under the tool-call id
  ([`a11/sdk/llm_tools/runner.py`](../a11/sdk/llm_tools/runner.py)); the bridge
  re-emits what a remote tool logged onto the local action's log
  ([`a11/gateway/tool_bridge.py`](../a11/gateway/tool_bridge.py)). That is why the
  log can be recorded with the conversation — a reopened chat shows what a tool
  did, not merely that it ran — while the model's contract cannot mention it,
  because the port is not part of any schema.
- The plugin holds the API key (IDE `PasswordSafe`) and hands it to the page via
  `getConfig` alongside the gateway URL and provider/model; the gateway holds no
  provider config of its own.
- **No bespoke chat contract:** the page calls `interact_with_llm` directly, with
  the same inputs `a11 chat` uses — the conversation on `interactions`, the tool
  descriptors on `tools`, and `config` closed empty so each provider applies its
  own defaults. There is no plugin-specific action in between.
- **The conversation is a list of native `Interaction`s**, held in the page and
  threaded back into every turn, exactly as `ChatUI` does in
  [`a11/cli/chat_ui.py`](../a11/cli/chat_ui.py). They are the provider's own
  objects — so a turn's tool calls and their results are still in front of the
  model next turn, rather than flattened into a text transcript. Holding them in
  the page is also what makes a gateway restart survivable: the retry replays the
  whole history onto a fresh session.
- **Chat history is those same interactions, stored.** Each turn, the gateway
  appends the conversation's new `Interaction`s to a per-conversation A11 node
  backed by an `SQLiteChunkStore` under `~/.cache/a11/gateway/conversations`
  ([`a11/gateway/conversations.py`](../a11/gateway/conversations.py)), with a small
  `sqlite3` index beside it for the list. A conversation's node id is the id of its *first*
  interaction — minted in the page — so both sides name a conversation the same
  way without a handshake. The page browses it through two ordinary actions,
  `get_conversations` and `get_conversation`
  ([`a11/gateway/conversation_actions.py`](../a11/gateway/conversation_actions.py));
  reopening
  a conversation hands the page back the provider's own objects, so it continues
  in it rather than replaying a transcript, and the next turn lands on the same
  node. Each tool run's log is recorded alongside, in the interaction's
  `backend_specific_metadata` — the one part of an interaction no backend turns
  into provider content — so a replayed tool box reads like the live one did.
- **Cross-language types:** an interaction only survives the crossing because
  Python and TypeScript name the same classes the same way on the wire —
  `a11.Chunk`, `a11.Status`, `a11.sdk.Interaction`. That table lives once per
  language ([`js/src/serial_tags.ts`](../js/src/serial_tags.ts),
  [`a11/data/serial_tags.py`](../a11/data/serial_tags.py)) and is pinned by
  `testdata/serial_tags.json`, which every language's test suite checks itself
  against.

### Transport note

The native A11 runtime speaks WebSocket-over-HTTP/2, which the JVM WebSocket
client cannot do — so the gateway serves WebSocket over **HTTP/1.1** as well
(one binary frame = one msgpack `WireMessage`), and a JVM or browser client
connects to it directly. All A11
*data* (WireMessage/NodeFragment/Chunk/Status/serialization) stays byte-for-byte
identical — proven by the golden-vector and live-interop tests in `../kotlin`.

## Build & run

Prerequisites: JDK 21, **Node.js ≥ 20** (to bundle the JCEF webview), a CLion
install matching `platformVersion` in `gradle.properties`, and a Python env with
`a11` installed (`pip install "a11-kit[llm]"` plus `websockets`).

```sh
cd intellij-plugin
./gradlew buildWebview    # bundles ../js + ../webview + webview/ into resources/webview/
./gradlew runIde          # launches a sandbox CLion (2026.1) with the plugin
./gradlew buildPlugin     # produces a distributable .zip under build/distributions
./gradlew test            # headless BasePlatformTestCase tool tests
```

`processResources` depends on `buildWebview`, so the standard Gradle tasks build
the web bundle automatically; run `buildWebview` on its own only to iterate on
the UI. To iterate on the webview directly without Gradle:

```sh
cd webview
npm ci
npm run build            # or `npm run build:dev` for an inline sourcemap
```

Start the gateway (from a Python env with `a11` installed):

```sh
a11 gateway               # ws://127.0.0.1:8011/a11
```

Configure under **Preferences → Tools → A11 Chat**:
- **Gateway URL**: defaults to `ws://127.0.0.1:8011/a11`; a bare `host:port` is
  accepted and completed.
- **Extra allowed tools**: comma-separated patterns for the gateway's own tools,
  `shell_.*` by default.
- **Provider / model / base URL** and the **API key** (stored in PasswordSafe).

Open the **A11 Chat** tool window (right dock), type a prompt, and the reply
streams in. Ask something like *"what file am I looking at?"* to exercise the
`get_active_file` tool. **History** lists the conversations recorded under
`~/.cache/a11/gateway/conversations` — pick one to reopen it and keep going in it — and
**+ New chat** starts a fresh one. Deleting that directory clears the history;
nothing else depends on it.

Open the **A11 Actions** tool window to explore and run the IDE tools directly:
pick an action, fill in the fields its input schema declares, hit **Run**, and
inspect the result — no provider key or gateway needed, since it calls the IDE
handlers straight through the bridge. The view builds itself from
`IdeTools.listDescriptors()`, so a tool added there shows up here with no UI
change. `get_error_highlights` is a good one to try: give it a `path` (absolute
or project-relative) and, optionally, a 0-based `start_line`/`end_line`, and it
streams one entry per red or yellow underline in that range — position, the text
underlined, and the explanation the tooltip gives. A file already open in an
editor is read straight from the analysis the daemon has done for it; any other
file is analyzed on demand.

`read_file` takes the same 0-based, `end_line`-inclusive range, so a highlight's
own coordinates read back the lines it sits on; `include_line_numbers` prefixes
each line with its number, which is for reasoning about positions rather than for
quoting text back. `get_file_symbols` takes an optional `path` too, so it is not
limited to the active editor. And `apply_patch` takes two plain-text inputs — a
`path` and a unified diff — and applies it as **one IDE command**, so a single
Undo reverses the whole patch, exactly as it does for `rename_symbol`. Hunks are
placed by their context rather than by the numbers in their `@@` header, and a
hunk that does not match the file is refused with what is there instead: nothing
is applied on a near miss, because a fuzzy match is how a tool silently rewrites
the wrong lines. Two slips a written-by-hand diff makes — a context line missing
its leading space, or the markers indented — are read for what they are, since
the file itself settles which line is which.

### A note on `buildSearchableOptions`

The build disables it (`intellijPlatform { buildSearchableOptions = false }`).
CLion 2026.1 cannot run the `traverseUI` starter it needs: CLion replaces that
starter with one that relaunches the IDE "with the Radler language plugin" and
assembles the child command line with a `-D` JVM flag where the executable
should be, so the task dies immediately in `CLionTraverseUIStarter` —
identically with or without this plugin's extensions. The task only pre-indexes
the Settings search field; the settings pages themselves are unaffected. Re-enable
it when that starter is fixed, or when building against an IDE that does not
override `traverseUI`.

## Fast dev loop / hot reload

- `./gradlew runIde` keeps a sandbox IDE running; rebuilding the plugin
  (`./gradlew build` in another terminal, or the IDE's *Build*) triggers the
  platform's plugin **auto-reload** — no full restart for most changes.
- The gateway is a plain Python process you own: restart `a11 gateway` after
  editing anything under [`../a11/gateway`](../a11/gateway).

## Testing

- Gateway: `python -m pytest a11/gateway a11/sdk/llm_tools` from the repo root
  covers the tool bridge's schema mapping, and the tool runner's handling of a
  run log and of the tools a caller's patterns admit.
- Flow language: what a *word means* is tested in `cpp/tests/` -- one lexer, one
  set of expectations -- so what is tested here is this plugin's own two jobs.
  `FlowEngineTest` drives the protocol: a request out, an envelope back, and the
  same text answered from the last answer rather than a fresh round trip.
  `FlowLexerTest` covers the platform's contract (every character in exactly one
  token, tokens that advance, a restart from any boundary the way incremental
  relexing does) always, and the translation of a meaning into a token type when a
  built `a11-flow` is on the machine -- it skips those cases rather than passing
  vacuously without one. Neither needs a platform fixture:
  `./gradlew test --tests "*Flow*Test*"` runs them in seconds. `FlowInjectionTest`
  does need one, and proves a flow inside a string literal is really injected and
  that prose merely mentioning one is not; it shares whatever fate `IdeToolsTest`
  meets -- on a machine where the fixture hangs during
  `CodeInsightTestFixtureImpl.setUp`, both hang, and neither is the plugin's doing.
  The Python suite holds the other half: `a11/flow/tests/test_editor_support.py`
  checks that the generated Sublime grammar is current and that no word list has
  crept back into this plugin.
- Plugin: `IdeToolsTest` (`BasePlatformTestCase`) builds the tool registry,
  drives `get_active_file` through an A11 action, and exercises the direct
  `IdeTools.runByName` / `listDescriptors` path the JCEF bridge uses, plus the
  reverse-dispatch proxy's handling of a tool's run log.
- Webview: `cd ../webview && npm run typecheck` type-checks the shared UI, and
  `cd webview && npm run typecheck` this host's entry point with it.
- Library: `../kotlin` has round-trip, **golden byte-vector** (decodes bytes
  produced by the native Python encoder), end-to-end (call + reverse tool
  dispatch), and **live interop** (`A11_BACKEND_URL=... ./gradlew test`) tests.

## Status

Verified: the Kotlin A11 layer, the streaming chat path, and cross-language wire
interop with the gateway. The reverse-dispatch **tool bridge** follows
the same Session/Action semantics the end-to-end test proves, but exercising it
against a real model requires a provider API key.
