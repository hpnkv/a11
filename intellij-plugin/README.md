# A11 Chat for JetBrains IDEs

An AI chat tool window for JetBrains IDEs — IntelliJ IDEA, PyCharm, CLion,
GoLand, Rider, WebStorm, … — powered by [A11](https://github.com/hpnkv/a11).
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

- The **web UI** lives in [`webview/`](webview) — a small TypeScript app bundled
  with esbuild into `src/main/resources/webview/app.js`. It imports the
  TypeScript A11 library from [`../js`](../js) and owns the WebSocket to the
  gateway directly (no Kotlin broker). Chat renders markdown turns, streams
  tokens, auto-scrolls, and shows a live "thinking" affordance.
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
- **A tool's run log reaches the gateway but never the model.** An output flagged
  `user_facing` in the Kotlin descriptor carries the plugin's narration of a run,
  written for the person watching; the gateway's shell tools write the same thing
  on their `user_facing_log` port. Either way the LLM **tool runner** is what
  holds it back — it drains that port, keeps it out of the tool result, and files
  it under the tool-call id
  ([`a11/sdk/llm_tools/runner.py`](../a11/sdk/llm_tools/runner.py)); the bridge
  merely re-points a remote tool's flagged port onto that canonical name
  ([`a11/gateway/tool_bridge.py`](../a11/gateway/tool_bridge.py)). That is why the
  log can be recorded with the conversation — a reopened chat shows what a tool
  did, not merely that it ran — while the model's contract never mentions the
  port.
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
./gradlew buildWebview    # bundles ../js + webview/ into resources/webview/app.js
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
pick an action, enter its input JSON, hit **Run**, and inspect the result — no
provider key or gateway needed, since it calls the IDE handlers straight through
the bridge.

## Fast dev loop / hot reload

- `./gradlew runIde` keeps a sandbox IDE running; rebuilding the plugin
  (`./gradlew build` in another terminal, or the IDE's *Build*) triggers the
  platform's plugin **auto-reload** — no full restart for most changes.
- The gateway is a plain Python process you own: restart `a11 gateway` after
  editing anything under [`../a11/gateway`](../a11/gateway).

## Testing

- Gateway: `python -m pytest a11/gateway a11/sdk/llm_tools` from the repo root
  covers the tool bridge's schema mapping, and the tool runner's handling of a
  user-facing run log and of the tools a caller's patterns admit.
- Plugin: `IdeToolsTest` (`BasePlatformTestCase`) builds the tool registry,
  drives `get_active_file` through an A11 action, and exercises the direct
  `IdeTools.runByName` / `listDescriptors` path the JCEF bridge uses, plus the
  reverse-dispatch proxy's handling of a tool's user-facing run log.
- Webview: `cd webview && npm run typecheck` type-checks the UI + client.
- Library: `../kotlin` has round-trip, **golden byte-vector** (decodes bytes
  produced by the native Python encoder), end-to-end (call + reverse tool
  dispatch), and **live interop** (`A11_BACKEND_URL=… ./gradlew test`) tests.

## Status

Verified: the Kotlin A11 layer, the streaming chat path, and cross-language wire
interop with the gateway. The reverse-dispatch **tool bridge** follows
the same Session/Action semantics the end-to-end test proves, but exercising it
against a real model requires a provider API key.
