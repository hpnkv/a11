# LLM context compression and compaction

Use `a11.sdk.llm_context` from a library, an ordinary action, or a host integration.
Context policy is optional. Concrete `interact_with_*` actions and the router
read `x-a11-llm-context-config`; an absent header disables their context layer.

See [portable Interaction views](interactions.md), [direct calls and Flow](actions.md),
[external harnesses](harnesses.md), and
[measurement](measurement.md) for complete configurations.

## Content and history

`compress_content` returns `{text, report}`. The original argument remains
unchanged. `json` removes insignificant JSON whitespace while preserving number
lexemes, duplicate keys, and string tokens. `lines` and `json-keys` emit
versioned JSON envelopes that `expand_content` can reverse byte for byte.
`auto` chooses the smallest candidate under the selected counter, including
encoding overhead. A candidate must save at least `min_gain` tokens.

`count_context` uses a host-supplied tokenizer or a byte-based estimate labeled
`byte_estimate`. It does not estimate billing, request framing, image costs,
provider tool-schema overhead, or hidden server history.

`compact_history` returns a checkpoint proposal, covered source IDs, a digest,
a retained boundary, and a report. System/developer messages and recent entries
are retained. The selected prefix ends at a complete tool-call/result group.
A registered callback or caller-supplied checkpoint provides summary text;
there is no default summarization model. A failed budget check raises an A11
status and produces no replacement history.

::: a11.sdk.llm_context.content
    options:
      members: [compress_content, count_context, expand_content]

::: a11.sdk.llm_context.history
    options:
      members: [compact_history, compact_interactions]

## Interaction views and installation

`project_interactions` returns a proposal with existing typed Interaction
sources and a separate normalized projection. `compact_interactions` selects
summary compaction. `accept_interaction_view` rechecks the snapshot and requires
an explicit owner acknowledgement. `use_interaction_view` materializes only
accepted projections, excluding the covered prefix. Accepted projections replay
without loading source-producer SDKs or repeating summarization.

The plain JSON contract is `a11.llm-context.interaction-view/v1`. It uses existing
Interaction/Chunk serialization and introduces no new runtime value or wire tag.
`entries[].source_id=null` identifies generated checkpoint content; retained
entries name canonical sources. `projection_digest` covers entries and
instructions. `view_id` binds that projection to its source digest, source
revision, coverage, boundary, and policy. The installation records `installed`,
`owner`, `view_id`, `source_digest`, `source_revision`, and the committed
`revision`. Digests provide integrity; owner access remains host-authorized.

Projected Interaction metadata contains the producer `a11.llm_context` and a
bounded `a11.llm-context.view` proof. A backend validates the full prefix and
its content/instruction digest before request formatting. `sources` retains
original action inputs/outputs, typed chunks, usage, and backend metadata;
model-facing normalized parts contain the portable text/image/tool subset.
Providers declare diagnostic-only `replay_metadata_keys` beside their existing
`register_interaction_normalizer` call. Unknown metadata remains opaque. This
capability declaration adds no provider-specific compaction algorithm.

::: a11.sdk.llm_context.interactions
    options:
      members: [project_interactions, encode_interactions, decode_interactions, interaction_view_sources, use_interaction_view, validate_interaction_view]

::: a11.sdk.llm_context.interaction_owner
    options:
      members: [accept_interaction_view, persist_interaction_view, load_interaction_view, compact_host_interactions]

## Policy and host dependencies

`LlmContextConfig` is immutable and rejects unknown fields. The header is bounded
to 8192 bytes and cached by its raw bytes. Equal inherited headers reuse the
same decoded policy. `LlmContextServices` binds host tokenizers, summarizers,
retention, and recording independently of caller policy. Its ceilings apply at
each receiving action boundary.

| Setting | Default | Meaning |
| --- | --- | --- |
| `enabled` | `true` in an explicit config | Master switch; an absent action header resolves to disabled. |
| `boundary` | `provider` | Select provider-message or backend-agnostic `interaction` projection. Accepted Interaction views select their own projection boundary. |
| `input_content`, `tool_results` | `off` | Select editable request text and model-facing tool results independently. |
| `strategy` | `auto` | `auto`, `json`, `lines`, or `json-keys`. |
| `max_bytes` | 8 MiB | Bound content admitted to transforms. |
| `min_gain` | 16 | Minimum estimated or counted token reduction. |
| `compaction` | `off` | `summary` selects history checkpointing; `native` coordinates supported harness-owned sessions. |
| `summarizer` | empty | Host-bound callback name or registered summarizer action name. |
| `max_tokens`, `reserve_tokens` | 24000, 2048 | History budget and space reserved for continuation. |
| `retain_recent` | 4 | Minimum recent message count to retain, extended to preserve tool groups. |
| `retention` | `false` | Persist original and projected request views through configured host storage. |
| `shell.mode` | `off` | `off`, `observe`, or `compress` at completed-result boundaries. |
| `measurement.mode` | `off` | `off`, `observe`, or `shadow`; shadow sends the original content. |
| `measurement.run_id`, `measurement.arm` | empty | Link observations to a host-selected experiment. |

An enabled config alone selects no request, tool, or history transforms.
Standalone `compress_content(text)` explicitly requests compression and uses
its default policy. Utility actions use their effective action header.

::: a11.sdk.llm_context.config
    options:
      members: [LlmContextConfig, LlmContextScope, LlmContextHeaders, LlmContextServices, decode_config, bind_services, configure_registry]

## Programmatic host integration

`project_messages` projects provider-shaped message dictionaries in a host's
request callback. `compact_host_history` reads an explicit revisioned snapshot,
prepares a checkpoint, rechecks its source, and calls the host's atomic
installer. These APIs use Python callbacks and require no command or proxy.
Other languages can invoke the same utilities as ordinary A11 actions.

::: a11.sdk.llm_context.runtime
    options:
      members: [project_messages, project_view]

::: a11.sdk.llm_context.harness
    options:
      members: [compact_host_history]

## Ordinary actions

`register(registry, services=..., names=...)` installs selectable ordinary
handlers. Each has a unary JSON `request` input and `result` output. Policy and
scope use the same headers as model actions. The usual action transports,
describer, status chunks, and Flow dispatch apply.

| Action | Request data |
| --- | --- |
| `llm_context_project_interactions`, `llm_context_compact_interactions` | `interactions`: typed Chunk JSON documents, optional `source_revision`, `checkpoint` |
| `llm_context_accept_interaction_view` | `view`, authorized `scope` with expected revision and stable operation ID |
| `llm_context_load_interaction_view` | Authorized `scope` |
| `llm_context_use_interaction_view` | Accepted `view`; result contains typed projected `interactions` |
| `llm_context_compress`, `llm_context_count`, `llm_context_expand` | `text` |
| `llm_context_compact` | `history`, optional `checkpoint` |
| `llm_context_shell` | Completed command `result`, optional `source` and `bypass` |
| `llm_context_retain` | `text`, authorized `scope` |
| `llm_context_retrieve` | `reference`, authorized `scope`, optional `cursor` and `max_bytes` |
| `llm_context_save_view` | `view`, `source_digest`, authorized `scope` with operation/revision |
| `llm_context_load_view` | Authorized `scope` |

`llm_context_retrieve` authorizes against the receiving host's `authorize_scope`
callback. A caller-supplied owner or conversation string grants no access.

::: a11.sdk.llm_context.actions
    options:
      members: [register, schema]

## Retention, shell results, and HTTP interception

`RetainedContentStore` uses native SQLite chunk stores for bytes and a separate
SDK SQLite index for scopes, expiry, and view revisions. References name an
owner-local object. A remote host accesses that owner's utility action; a node
ID alone has no routing or authorization meaning.

::: a11.sdk.llm_context.retention
    options:
      members: [RetainedContentStore]

::: a11.sdk.llm_context.shell
    options:
      members: [intercept_shell_result, shell_hook]

::: a11.sdk.llm_context.proxy
    options:
      members: [create_proxy, project_request]

The generic `a11.net.http.relay_http` forwards to an explicit URL through the
native connection pool. It streams response bytes and trailers, preserves HTTP
error responses, strips hop-by-hop/private A11 headers, and performs no retry.
Its deadline and cancellation affect the exchange's stream.

## Measurement and CLI

::: a11.sdk.llm_context.measurement
    options:
      members: [RunRecorder, load_runs, summarize_run, compare_runs, evaluate_pair]

`a11 llm-context --help` lists standalone operations. `compress`, `count`, and
`expand` read bounded UTF-8 stdin; `compact`, `retrieve`, and `shell-hook` read
JSON. `shell-filter` reads/writes raw bytes and preserves passthrough bytes.
Other operations emit JSON. Diagnostics use stderr.

`--config` uses the same strict policy validator. `--record` appends reports.
`compact --summarizer module:callable` loads an explicitly selected host callable;
policy headers never select import paths. `compare --baseline FILE --treatment
FILE` reads recorded JSON/JSONL. `measure` compares captured input with the same
utility disabled/enabled without executing a model or a command.

## Harness adapters

The shared transforms accept provider messages, completed results, and explicit
checkpoint proposals. `adapters` maps those contracts onto public harness APIs.
The optional Claude Agent SDK import occurs when its hook factory is called.

::: a11.sdk.llm_context.adapters
    options:
      members: [compact_codex_thread, compact_claude_session, compact_acp_session, project_acp_prompt, codex_tool_hook, claude_code_hooks]

## Passive response observations

`ResponseUsageObserver` consumes bounded JSON or SSE response events through the
native relay observer. Usage revisions share a request identity and use the
same provider normalization as SDK clients. Incomplete frames, limits, errors,
and non-success HTTP statuses produce partial coverage. Transport completion
alone supplies no missing usage counters. The relay disables a failing observer
and continues forwarding the original response.

::: a11.sdk.llm_context.observation
    options:
      members: [ResponseUsageObserver]
