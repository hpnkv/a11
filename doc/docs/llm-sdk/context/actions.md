# Compress context in model calls and Flow

Set a context header to reduce editable model input or tool-result text.
The same policy works on a concrete `interact_with_*` action and on
`interact_with_llm`, including calls made by Flow. It requires no agent loop.

```python
import a11
from a11.sdk.llm_context import LlmContextConfig, LlmContextHeaders
from a11.sdk.interact_with_llm import interact_with_llm
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA

policy = LlmContextConfig(input_content="lossless", tool_results="lossless")
call = (
    a11.Action(INTERACT_WITH_LLM_SCHEMA)
    .bind_handler(interact_with_llm)
    .set_header(LlmContextHeaders.CONFIG, policy.model_dump_json())
)
```

Supply the normal provider/model headers and ports, then run the action using
[the model-call protocol](../../guides/llm.md). Tool execution, typed output fragments,
provider generation options, and action statuses retain their normal owners.
Only editable request spans and successful model-facing tool results are
projected. System instructions, assistant output, signed reasoning, images,
and unknown blocks remain outside deterministic text compression.

The native transforms remove JSON whitespace or encode repeated content.
A reversible envelope is model-facing content, so evaluate the model's ability
to read it on your workload. `strategy="json"` selects whitespace compaction
alone. Neither strategy silently truncates data.

To prepare model context before choosing a provider, use
[portable Interaction views](interactions.md). Proposal generation, owner
acceptance, and replay are separate calls. The same accepted checkpoint works
with compatible backends and through programmatic host callbacks or utility RPC.

## Return a compact view from an agnostic tool

A library caller can use the utilities without constructing an Action:

```python
from a11.sdk.llm_context import compress_content, expand_content

original = "Repeated diagnostic line\n" * 1000
projected = compress_content(original, {"strategy": "lines"})
assert expand_content(projected["text"]) == original
```

A callable tool can return `projected["text"]` through its existing result API.
Keep the original in its existing owner. When a tool exposes an explicit view
schema, return the view, report, and optional original reference together.
The tool's caller needs no A11-specific agent or model routing.

## Use the same utilities as actions and in Flow

```python
import a11
from a11.sdk.llm_context import register

registry = a11.ActionRegistry()
register(registry, names=["llm_context_compress", "llm_context_count", "llm_context_compact"])
```

A `llm_context_compress` call writes `{"text": ...}` to `request` and reads `result`.
Configure its header before running it. For Flow, register the same handlers:

```a11flow
flow shrink {
  in  request: object

  out result: object
  nodes scratch
  compact = run llm_context_compress(request: request) via scratch
  compact.result -> result
}
```

Invoke the program with `headers={"x-a11-llm-context-config":
'{"strategy":"lines"}'}`. A Flow that already calls a model action needs only
that header/configuration to enable the same request preparation. Raw typed
ports continue to carry original data for Flow arithmetic and filtering.

## Compact a history prefix

```python
from a11.sdk.llm_context import LlmContextConfig, compact_history

policy = LlmContextConfig(
    compaction="summary", max_tokens=12000, reserve_tokens=2000,
    retain_recent=4,
)
proposal = await compact_history(history, policy, summarizer=summarize_prefix)
```

`summarize_prefix(prefix)` returns nonempty text, synchronously or
asynchronously. Include goals, decisions, constraints, unresolved work, and
source references in your summarizer's instructions. The utility keeps recent
entries and complete tool groups, checks the resulting budget, and returns
`source_digest`, `covered_ids`, and `retained_boundary` for validation.
A standalone proposal becomes active only when the host installs it.

Automatic request compaction binds the summarizer at the receiving registry:

```python
from a11.sdk.llm_context import LlmContextServices, bind_services

bind_services(registry, LlmContextServices(summarizers={"checkpoint": summarize_prefix}))
```

Set `summarizer="checkpoint"` on the action policy. Alternatively, register a
summarizer action with a `history` input and `checkpoint` output. Its nested call
inherits the action deadline and explicitly disables recursive compaction.

Replay-based providers can send the projected history. Resumed Codex/Claude
Code sessions and Gemini ID chains own history on another service; a client-side
history edit cannot replace that state. The integration returns
`FAILED_PRECONDITION` when automatic summary compaction requires that missing
capability. Use a standalone proposal and a supported host replacement API.

## Retain originals and resume a view

```python
from a11.sdk.llm_context import LlmContextServices, RetainedContentStore, bind_services

store = RetainedContentStore("/host/context", owner="analysis-host")
bind_services(registry, LlmContextServices(
    retention=store,
    authorize_scope=authorize_context_scope,
))
```

The authorization callback receives the actual Action and validated scope.
Check it against the host's authenticated identity and access policy. Set
`retention=true` in context policy and send an explicit scope header:

```json
{
  "owner": "analysis-host",
  "conversation": "conversation-42",
  "branch": "main",
  "expected_revision": 0,
  "operation_id": "request-1"
}
```

The SDK retains original messages and saves the projected view with a revision
check. Its internal `context` log records the resulting descriptor. Pass the
returned revision in the next explicit operation; use a new operation ID when
input changes. Repeating an operation with the same source reuses its stored
view. A source mismatch or stale revision returns `ABORTED`. Automatic request
preparation derives a request operation from the caller operation ID, source
digest, policy, and host service revision. Each completed model round advances
the action's serialized scope revision; callers use the emitted revision across
separate actions. A durable pending claim prevents a retry from duplicating a
summary call after an uncertain outcome. Reconcile pending work at its owner
before retrying it.

`llm_context_load_view` and `llm_context_retrieve` recover authorized views/data through
ordinary actions. Library hosts can call `load_view` and `retrieve` directly
under their own authorization. Retrieval is paged on UTF-8 byte boundaries.
Expiry and quotas bound retention; `purge_expired()` releases expired payloads.
Applications select the owner and root path, never caller headers.

Each process receiving actions binds its own services. Policy/scope headers and
references survive transport; Python objects and caches do not establish remote
identity or correctness. SDK-registered Python handlers retain their receiving
host binding through native registry copies for sessions. A cold remote process
registers its own services. The gateway also binds its configured services on
each receiving session registry.

## Publish configured defaults

`configure_registry(registry, policy, names=[...], services=...)` publishes policy
through existing header schema defaults. Explicit call headers, including
`{"enabled":false}`, override them. The gateway exposes the same default with
`a11 gateway --llm-context-config '{"input_content":"lossless"}'` and with
`GatewayConfig.llm_context_config` for embedded operation.

See the [reference](reference.md) for settings, return contracts, and
[measurement](measurement.md) before comparing workloads.

## Recover retained operations

Read the stored operation ID from `load_view` or the operation report when
recovering an automatically projected view. Derived operation IDs hash the
owner/conversation/branch, caller operation, source digest, and policy revision;
they fit the same bounded wire scope as caller IDs.

The receiving host owns durable retention state. `inspect_operation` reads its
claim token and pending/committed/released/abandoned/expired state.
`reconcile_operation` requires the observed token and an audit note. An explicit
owner decision can abandon work or release it for a retry after reconciling
external effects. A replacement token fences the previous worker. Lost replies
receive no automatic summarizer retry.

The ordinary actions `llm_context_inspect_operation` and
`llm_context_reconcile_operation` carry those requests over existing transports.
Their scope, including operation ID, is authorized on the receiving host.
`llm_context_fork_view` authorizes both source and destination scopes, copies
retained source into the destination, and starts that branch at revision one.
`llm_context_save_view` accepts an optional claim token and requires it when
completing previously claimed work.

`collect_garbage` is a host-owned maintenance operation. It clears expired,
failed, and abandoned payloads and preserves revision heads as tombstones.
Its pending grace accounts for live writers. A process restart retains claims,
accepted manifests, and revision checks in SQLite; no task-local cache supplies
recovery evidence. Quotas bound payload bytes and metadata record counts.
