# Use context utilities with external harnesses

Choose an integration boundary that the host exposes: a provider endpoint,
a callable/command utility, a completed-result hook, or a history replacement
API. Each path uses the same context transforms. The harness retains its own
agent loop, tool permissions, and session persistence.

To prepare model context before choosing a provider, use
[portable Interaction views](interactions.md). Proposal generation, owner
acceptance, and replay are separate calls. The same accepted checkpoint works
with compatible backends and through programmatic host callbacks or utility RPC.

## Integrate through host APIs

Python host plugins can call `compress_content`, `project_messages`,
`compact_history`, and `compact_host_history` directly. These calls require no
shell process, HTTP listener, or A11 agent. A request callback can project an
editable message list before the host calls its existing provider client:

```python
from a11.sdk.llm_context import LlmContextConfig, project_messages

policy = LlmContextConfig(input_content="lossless", tool_results="lossless")
projected = await project_messages(provider_messages, policy)
```

For a host that supports checkpoint installation, bind two callbacks:

```python
from a11.sdk.llm_context import compact_host_history

result = await compact_host_history(
    read_history=host_snapshot,
    replace_history=host_replace_checkpoint,
    config=checkpoint_policy,
    summarizer=summarize_prefix,
)
```

`host_snapshot()` returns `{"history": [...], "revision": ...}`. The installer
accepts `(proposal, *, expected_revision, source_digest)`, checks those values
atomically in the host, replaces the covered prefix, and acknowledges
`{"installed": true, "revision": ...}`. Both callbacks support synchronous or
asynchronous implementations. A11 rechecks the snapshot after summarization;
the host's atomic check closes the remaining race. Explicit revisions work
across processes and machines. A changed source returns `ABORTED`. An uncertain
installation receives no implicit retry.

A plugin in another language calls the ordinary utility actions through its
A11 client. This is utility RPC over an existing A11 transport, independently
of the host's model connection. TypeScript and Kotlin clients use the same
JSON request/result contracts and context headers as Python. Ordinary action
discovery supplies the utility schema. Source validation and host installation
remain explicit after the call.

| Host integration | Programmatic path | Host responsibility |
| --- | --- | --- |
| Python harness/plugin | Import the utilities and register request/result or checkpoint callbacks. | Map native records into editable spans and acknowledge any history replacement. |
| TypeScript harness/plugin | Call utility actions with the A11 TypeScript client; use the native host event API for installation. | Keep the host's inference client and persist accepted checkpoints in its session. |
| JVM/Junie plugin | Call the same actions with the existing Kotlin A11 client from its plugin service. | Map editor/harness records and use a supported host result/history update API. |
| Codex or Claude Code integration | A host application can call the library or utility RPC from its own SDK/event callbacks. | Check the installed host's supported mutation APIs; lifecycle notifications alone do not supply history replacement. |
| Pi extension | Request a proposal from a utility client and return it through `session_before_compact`. | Preserve Pi's prepared boundary and active branch identity. |

The shared transform layer contains no harness event names. Its public message views,
reports, source IDs, proposals, and explicit installation acknowledgements let
an adapter use selected capabilities. Where replacement is unavailable, use
`compact_history` to obtain a proposal for a handoff or use deterministic
request/result projection at the host's editable boundary.

## Intercept provider requests

Start a proxy for a provider's wire format:

```sh
a11 llm-context proxy --upstream https://api.anthropic.com \
  --profile anthropic --port 8013 \
  --config '{"input_content":"lossless","tool_results":"lossless","strategy":"json"}' \
  --record /tmp/context-proxy.jsonl
```

Configure the host's supported provider base URL to point at
`http://127.0.0.1:8013`. The listener preserves incoming paths; use an upstream
origin when the client already sends `/v1/...`. An optional upstream path prefix
is prepended literally. Credentials stay in request headers and pass through to
the configured upstream. Caller `x-a11-*` headers are removed before forwarding
and cannot select proxy policy.

The available profiles are `chat` (`/chat/completions`), `responses`
(`/responses`), and `anthropic` (`/messages`). Only POST JSON requests in those
formats receive projections. Unknown endpoints and content encodings pass
through. Request bodies use configured byte limits. Proxy responses stream
through the native HTTP stack, including SSE, error bodies, duplicate headers,
and trailers. Admission, buffering, and deadlines are bounded.

An embedded host can call `create_proxy(...)`, supplying `LlmContextServices` and
native HTTP transport/TLS options. Call `stop()` on the returned native server
to close its listener. Bind authenticated remote listeners through the host's
existing HTTP deployment configuration.

An endpoint setting establishes wire routing only. Verify the installed host's
authentication mode and API support separately, especially for subscription or
login-specific endpoints. Proxy replay compaction cannot replace history kept
behind `previous_response_id`; such requests return a capability error when
summary compaction is required. The proxy recorder observes projection/relay
metadata and bounded JSON/SSE provider usage. Missing or partial usage retains
its coverage limits. Automatic owner-backed retention uses host scope resolution
and authorization callbacks; the standalone CLI requires an embedded host for
those bindings.

Choose one compression owner for a request path. When a proxy projects content,
disable the corresponding A11 action-side transform. Provenance comes from the
configured boundary or explicit source metadata; arbitrary output markers do
not establish that content has already been processed.

## Call utilities without proxying inference

```sh
printf '{  "rows": [1, 2, 3] }' | \
  a11 llm-context compress --config '{"strategy":"json","min_gain":0}'
```

The JSON result contains the view and report. A host can use the Python library
in-process, invoke the CLI over stdin/stdout, or call ordinary context actions
through an existing A11 connection. None of these paths requires an A11 agent.

For a checkpoint proposal, send JSON history to the standalone utility:

```sh
a11 llm-context compact \
  --config '{"max_tokens":12000,"reserve_tokens":2000,"retain_recent":4}' \
  --summarizer my_host.summaries:checkpoint < history.json
```

The explicitly selected host callable accepts the prefix and returns summary
text. A checkpoint already generated by the host can instead be supplied as
`checkpoint` in stdin JSON. A host installing the proposal validates its source
digest and covered IDs against the active branch before replacing history.
Appending a summary while retaining the covered prefix is a handoff aid; it
does not reclaim that prefix's context space.

## Integrate a Pi compaction callback

Map Pi's `session_before_compact` preparation into an ordinary compaction
utility call. Preserve its selected prefix and retained-entry boundary, validate
the proposal's source digest and active leaf, and return the accepted checkpoint
through Pi's event API. The host owns cancellation, opt-in configuration,
inference, and session persistence.

Use the [Pi extension contract](https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/extensions.md#session_before_compact--session_compact)
for the installed version. The shared A11 utilities provide the proposal;
the host plugin supplies the Pi event mapping.

## Intercept completed shell results

```python
from a11.sdk.llm_context import LlmContextConfig, intercept_shell_result

policy = LlmContextConfig(shell={"mode": "compress"})
projection = intercept_shell_result(
    {"stdout": captured_stdout, "stderr": captured_stderr, "exit_code": 0},
    policy,
)
model_result = projection["view"]
original_result = projection["original"]
```

`off` returns original presentation. `observe` records counts with unchanged
presentation. `compress` applies the shared transforms. Command failures,
signals, binary/unknown encodings, interactive sessions, incomplete streams,
truncation, bypasses, and transform failures pass through. The command executes
once in its existing host. stdout/stderr stay separate.

The shared model-tool bridge recognizes completed `spawn_process` and `run_command` results and
applies `shell.mode` at result presentation. A compressed line-array field is
a `{representation: "application/json", view: ...}` envelope; expanding the view
and parsing its JSON recovers the original array. Native byte ports retain
their encoded presentation. Raw typed process ports remain
available to Flow. External hooks map their completed result into the same
contract. A host-specific hook can invoke:

```sh
a11 llm-context shell-hook --config '{"shell":{"mode":"compress"}}'
```

Its stdin envelope is:

```json
{
  "schema": "a11.llm-context.shell-result/v1",
  "result": {"stdout": "captured output", "stderr": "", "exit_code": 0},
  "source": {"command": "host-provided hint"},
  "bypass": false
}
```

Map the returned `event.result` into the host's replacement-output API. The
returned reports remain outside model-visible output. Unsupported event
contracts pass through with `supported=false`; the utility implements no
pre-execution shell grammar or command rewrite.

`a11 llm-context shell-filter` provides raw stdin/stdout projection for an explicit
filter boundary. Enable it with `--config '{"shell":{"mode":"compress"}}'`.
On capture overflow it forwards the original stream. Pipeline exit-status and
approval behavior belong to the caller's shell/harness; completed-result hooks
retain those responsibilities in their existing host.

For Codex, Claude Code, and Junie, select a verified endpoint or result-hook API
from the installed version. Generic lifecycle notifications alone do not grant
custom history replacement. Claude Code's
[PostToolUse documentation](https://code.claude.com/docs/en/hooks#posttooluse)
describes its output replacement contract. A11 adds no context MCP server or
tool catalog.

## Coordinate native session compaction

Select `compaction="native"` for resumed `interact_with_codex` and
`interact_with_claude_code` actions. The same action header works through Flow.
The action awaits native completion before submitting pending user input.
Fresh sessions have no prior history to compact. Shadow mode leaves the native
session unchanged. Other provider/proxy boundaries reject native compaction
without a coordinator; they do not append a synthetic replacement summary.

An application using an existing Codex app-server connection can call:

```python
from a11.sdk.llm_context import compact_codex_thread

await compact_codex_thread(rpc.request, compact_events.get, thread_id)
```

Subscribe before calling and keep the host's ordinary event reader running.
The coordinator requires both a matching `contextCompaction` completion item
and successful completion of that item's turn. Notifications from child threads
supply no completion evidence for the requested thread. Timeout or disconnect
leaves an indeterminate result; inspect host state before another attempt.
See the [Codex app-server protocol](https://learn.chatgpt.com/docs/app-server).

For an already connected Claude Agent SDK client:

```python
from a11.sdk.llm_context import compact_claude_session

await compact_claude_session(client, on_message=observe_compaction)
await client.query(next_prompt)
```

The application serializes access to `receive_response`. The coordinator sends
`/compact` and requires a compaction boundary and successful result. Optional
`instructions` identify facts the native summary must preserve. An explicit
structured insufficient-history response returns an acknowledged no-op. The
observer receives all messages for usage attribution. The action integration
records compaction usage separately from the following generation.
See [Claude Agent SDK commands](https://code.claude.com/docs/en/agent-sdk/slash-commands).

Junie ACP clients can call `compact_acp_session(request, session_id, commands)`
when that session advertises `compress`. The adapter submits `/compress` through
`session/prompt` and returns its acknowledgement. `project_acp_prompt` projects
text blocks in a user prompt while preserving resource/image blocks. Filesystem
read responses remain canonical inputs for editing operations.
See [Junie commands](https://junie.jetbrains.com/docs/slash-commands.html) and
[ACP commands](https://agentclientprotocol.com/protocol/v1/slash-commands).

| Installed harness | Checked boundary | Runtime capability requirement |
| --- | --- | --- |
| Codex 0.164.0 | Live SDK paired requests and native compaction continuation | Native `thread/compact/start` and matching completion notifications. |
| Claude Code 2.1.266 / Python SDK 0.2.148 | Live SDK paired requests; native default and focused summary checks | `/compact`, `compact_boundary`, and successful result; output replacement support depends on the installed CLI. |
| Junie 26.8.24 | Installed CLI ACP availability and documented command contract | Session-advertised `compress`; absent advertisement raises a capability error. |

[Live validation](validation.md) covers Codex and Claude Code compaction and
small paired input-compression workloads. Junie checks cover its stated
adapter contract; the cross-version deployment matrix remains open.

## Use programmatic result hooks

`codex_tool_hook(event, policy)` maps a documented Codex `PostToolUse` event to
`continue=false` and `stopReason` when a successful completed result changes.
Unknown result shapes pass through. The event must carry a structured
`tool_response` with an integer `exit_code`. This conservative mapping preserves
failures and avoids changing code-mode promise failure behavior. See
[Codex hooks](https://learn.chatgpt.com/docs/hooks).

`claude_code_hooks(policy)` supplies a Python Claude Agent SDK hook mapping for
`ClaudeAgentOptions(hooks=...)`. It uses `updatedToolOutput`, preserving original
result fields. Bash results without a known integer `exit_code` or `exitCode`
pass through; an SDK/CLI combination that omits this evidence requires a
host-owned completed-result adapter. No exit status is inferred from stdout.
See [Claude hooks](https://code.claude.com/docs/en/hooks#posttooluse).

## Retain proxy views with host identity

Embed `create_proxy` with `LlmContextServices(resolve_scope=...,
authorize_scope=..., retention=...)`. The resolver receives the HTTP request and
returns an explicit owner/conversation/branch/revision/operation scope after
host authentication. The authorizer separately grants access to that scope.
Incoming private A11 headers are removed before upstream forwarding. Caller
policy does not choose the host's storage path or service callbacks.

`project_view` provides the same storage orchestration to programmatic callers
with an already authorized scope. Its result contains messages, updated scope,
and an owner-backed descriptor. The proxy publishes descriptors through its
recorder; provider response envelopes remain unchanged. Owner utilities expose
retained originals and accepted views through ordinary A11 sessions.
