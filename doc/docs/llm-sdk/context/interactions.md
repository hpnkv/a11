# Prepare Interaction views before choosing a backend

Build a model-facing view from A11 interactions, inspect its coverage, and accept
it through the history owner. The accepted checkpoint replays through compatible
`interact_with_*` backends without another summarizer call. These utilities run
as ordinary Python functions and require no A11 agent.

| History | Contents | Owner |
| --- | --- | --- |
| Canonical interactions | Original records, typed chunks, action fragments, metadata, and IDs. | The application's existing history owner. |
| Interaction view | A source archive, projected normalized parts, instructions, covered IDs, and an explicit installation acknowledgement. | The host that accepts the view. |
| Harness-native history | Codex thread, Claude Code session, or ACP host history, including server-held records. | The actual harness/session service. |

A local Interaction view does not install a checkpoint in a server-held session.
Use [native harness coordination](harnesses.md) for those histories.

## Build and inspect a proposal

```python
import a11
from a11.sdk.llm import Interaction
from a11.sdk.llm_context import LlmContextConfig, project_interactions

canonical = [
    Interaction(
        system_instructions=[a11.to_chunk("Preserve exact identifiers")],
        content=[a11.to_chunk("Earlier measurements " * 2000)],
    ),
    Interaction(content=[a11.to_chunk("Continue the investigation")]),
]
policy = LlmContextConfig(
    boundary="interaction",
    input_content="lossless",
    compaction="summary",
    max_tokens=500,
    reserve_tokens=100,
    retain_recent=1,
)
proposal = await project_interactions(
    canonical,
    policy,
    source_revision=7,
    checkpoint="The accepted result is ALPHA-17; verify its measurement.",
)
assert proposal["state"] == "proposal"
assert proposal["covered_ids"] == [canonical[0].id]
```

Supply `summarizer=callback` instead of `checkpoint` for a host-selected
synchronous or asynchronous summarizer. The callback receives complete prefix
messages from the shared compaction engine and returns nonempty text. There is
no default summarization model. Supplied checkpoint text covers the selected
prefix; use an accepted view for reuse of an already committed checkpoint. `compact_interactions` selects summary
compaction explicitly; `project_interactions` applies the supplied policy.

The proposal's `entries` exclude the covered prefix and contain generated
checkpoint text separately from retained source entries. `sources` contains
complete typed Interaction chunk documents. `interaction_view_sources(proposal)`
recovers independent canonical records. `source_ids`, `source_digest`,
`source_revision`, `covered_ids`, `retained_boundary`, and `view_id` describe the
snapshot and projection. These identity fields remain explicit on the wire.

Instructions retain their order and precede model history. Recent history and
complete tool-call/result groups stay together. Images, signed reasoning, and
opaque blocks form protected boundaries during prefix selection. The shared
counter applies the byte/token budget; provider framing, tool schemas, image
charges, and hidden server history remain outside that count.

## Accept through the history owner

```python
from a11.sdk.llm_context import accept_interaction_view, use_interaction_view

accepted = await accept_interaction_view(
    proposal,
    read_history=host.read_snapshot,
    install_view=host.install_projection,
)
model_history = use_interaction_view(accepted)
```

`read_snapshot()` returns `{"interactions": [...], "revision": 7}`.
`install_projection(proposal, *, expected_revision, source_digest)` atomically
checks that revision and digest, retains canonical history, and installs a
separate model-facing view. Both callbacks can be synchronous or asynchronous.
The installer acknowledges the exact committed view:

```python
acknowledgement = {
    "installed": True,
    "owner": "application-history",
    "view_id": proposal["view_id"],
    "source_digest": proposal["source_digest"],
    "source_revision": 7,
    "revision": 8,
}
```

A11 rechecks sources before calling the installer. The owner's atomic check
covers concurrent changes during installation. A changed revision or source
returns `ABORTED`. Missing, mismatched, or uncertain installation receives an
explicit error and no automatic retry. Reconcile the operation with the owner
before retrying. Digests validate integrity; authorization comes from the host's
ordinary authenticated storage and transport boundaries.

Persist the accepted JSON document through the existing history owner. A host
using `RetainedContentStore` can call `persist_interaction_view(proposal, store,
scope)` and `load_interaction_view(store, scope)`. This owner adopts the declared
canonical snapshot beside its projected view, using existing scoped manifests,
revision checks, operation IDs, and retained nodes. It does not validate an
external application's canonical revision: use that application's atomic
installer for its history. Ordinary utility actions additionally require the
receiving host's `authorize_scope` callback.

## Continue with compatible backends

Pass `model_history` to the usual `interactions` port on a concrete backend or
on `interact_with_llm`. Choose the provider with the ordinary model/provider
headers. For example, select `gpt` for one fresh replay and `claude` for another,
using the same accepted JSON document. The receiving backend validates the
complete projected prefix and formats its existing `NormalizedMessage` parts.
Source producers are unnecessary after acceptance; the archive stays typed and
the portable projection contains the committed model content.

Append new canonical interactions after the materialized view for continuation.
Keep the complete projected prefix and its provenance together. Partial,
reordered, edited, or spliced projected prefixes fail validation. Persist new
canonical outputs through their ordinary owner. `new_interactions` continues to
carry original outputs and typed action fragments. Projected tool-result clones
feed only the backend conversation.

The view retains instructions, text, inline images, and identified tool calls
and results that existing normalization represents faithfully. Remote image
URLs with detail settings, signed reasoning, unknown blocks/metadata, failed
records, and unavailable producer normalizers remain opaque in proposals.
Portable replay of retained opaque entries returns `FAILED_PRECONDITION`.
Original chunks remain available for native replay through their existing owner.
Same-backend provider-message projection preserves native blocks directly.

Replay requires a fresh provider history. Codex/Claude session resumes, GPT
`previous_response_id`, and Gemini server-history continuations reject local
view substitution. A new fresh session can consume a compatible portable view;
its subsequent server-held history belongs to that new session.

## Configure automatic request-local projection

```python
call.set_header(LlmContextHeaders.CONFIG, policy.model_dump_json())
```

With `boundary="interaction"`, participating SDK backends build and accept a
request-local view before formatting provider messages. This selection changes
the request's model context; it installs no canonical application or native
harness history. `retention=True` additionally commits through a configured,
authorized retained-view owner. Absent headers disable automatic transforms.
The default `boundary="provider"` retains provider-message projection.
Without a selected Interaction policy or an explicitly supplied accepted view,
SDK inputs stream incrementally through their existing backend preparation.
Absent or disabled policies with measurement off apply no context-specific host
limits; active processing and explicit utility calls retain those limits.

| Boundary | Projection responsibility |
| --- | --- |
| Router | Forward Interaction fragments and provenance unchanged. |
| Concrete backend | Validate an accepted view, or apply the opted-in Interaction policy once. |
| Provider formatter | Format projected parts and enforce backend replay restrictions. |
| Tool bridge | Preserve raw canonical fragments; the active Interaction projection owns model-facing result compression. |
| Provider-message HTTP proxy | Apply provider policy to editable native message trees. |

An accepted view selects the Interaction boundary even without a policy header.
Provider-message transforms skip a validated Interaction stage in that action.
The stage carries the current action identity; inherited stages do not suppress
unrelated nested actions. Equal decoded policies and immutable proof metadata
use bounded caches. Serialized identities, revisions, and owner acknowledgements
establish correctness across processes; those caches establish no ownership.

Select one projection owner for each request path. Configure an HTTP relay as
passthrough when an upstream SDK already projects its Interaction view. A proxy
cannot reconstruct A11 provenance from provider text and rejects
`boundary="interaction"`. Completed-shell compression and Interaction projection
also require separate result ownership; selecting both returns a capability
error. Automatic generative shadow compaction requires a standalone proposal.
If an accepted prefix plus new history exceeds the configured summary budget,
request a new checkpoint through its owner.

## Call utilities through actions and Flow

Register the ordinary handlers with `register(registry, services=...)`. Send
`encode_interactions(canonical)` as `request.interactions`, an optional
`source_revision`, and an optional `checkpoint`. Policy travels in
`x-a11-llm-context-config`. The returned proposal is the same JSON contract as
the direct function.

```a11flow
flow project {
  in  request: object

  out result: object
  nodes scratch
  view = run llm_context_compact_interactions(request: request) via scratch
  view.result -> result
}
```

Use `llm_context_accept_interaction_view` with `view` and an authorized `scope`
to install through the receiving retained-view owner. Use
`llm_context_load_interaction_view` to recover it and
`llm_context_use_interaction_view` to receive typed projected Interaction chunk
documents. `llm_context_project_interactions` also supports compression without
summary compaction. Existing Flows that call model actions receive automatic
Interaction projection through their headers; the Flow language and runtime
require no changes.

## Install from an external harness

```python
from a11.sdk.llm_context import compact_host_interactions

accepted = await compact_host_interactions(
    read_history=host.read_interaction_snapshot,
    install_view=host.install_interaction_projection,
    config=policy,
    summarizer=host.summarize_prefix,
)
```

The host maps its editable records into A11 interactions and installs the
accepted projection through its supported checkpoint API. Its inference client,
agent loop, tool executor, and session persistence remain with the host.
Programmatic callbacks require no shell or HTTP wrapper. Other languages can
request standalone proposals over ordinary utility RPC and validate/install
them through their native host APIs. The receiving utility's retained-view
acknowledgement attests only that store; installation in the harness requires
that harness owner's acknowledgement. Codex, Claude Code, and ACP native
compaction remain [separate coordination protocols](harnesses.md).

Measure baseline and treatment through the same host path, including summary
usage, installation outcome, and task quality. See
[Interaction measurement](measurement.md#measure-interaction-views).
