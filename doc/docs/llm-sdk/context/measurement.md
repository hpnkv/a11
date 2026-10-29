# Measure compression and compaction

Compare enabled and disabled policies on the same captured content or matched
isolated workloads. Record which changes actually reached the model, which
counts are estimates, and which outcomes the host measured.

## Measure captured content

```sh
cat captured-output.txt | a11 llm-context measure \
  --config '{"strategy":"auto","measurement":{"mode":"observe"}}'
```

This runs deterministic transforms on captured input. It executes no shell
command or model request. The comparison reports token/byte differences and
transform time; quality and monetary cost remain unmeasured.

For library use:

```python
from a11.sdk.llm_context import LlmContextConfig, compare_runs, compress_content

policy = LlmContextConfig(measurement={"mode": "observe"})
baseline = compress_content(text, policy.model_copy(update={"enabled": False}))
treatment = compress_content(text, policy)
comparison = compare_runs([baseline["report"]], [treatment["report"]])
```

`measurement.mode="shadow"` computes a deterministic candidate while returning
the original text. Its report separates `tokens_after` from
`projected_tokens_after`. Automatic generative shadow compaction returns an
explicit capability error. A standalone evaluation can explicitly supply a
checkpoint/summarizer and inspect `proposed_history` without installing it.

## Record the same setup in two arms

```python
from a11.sdk.llm_context import LlmContextServices, RunRecorder, bind_services

bind_services(registry, LlmContextServices(recorder=RunRecorder("treatment.jsonl")))
```

Enable measurement in each policy and set `measurement.run_id` and `arm`.
The disabled arm uses `enabled=false` with the same selected input/tool spans
and measurement enabled. SDK preparation records the observed passthrough
sizes. Existing provider `UsageUpdate` events are recorded by their ordinary
reporter when a recorder is bound. The recorder's file path is host-owned;
headers cannot choose an output file. The sink has a byte ceiling and counts
records dropped at that ceiling.

```sh
a11 llm-context compare --baseline baseline.jsonl --treatment treatment.jsonl
```

The comparator replaces usage revisions sharing an identity, ignores
reconciliation/context-only rows, and avoids adding request details to a turn
aggregate with the same known agent/turn scope. Unverified cumulative coverage
remains unknown. Missing token counters remain unknown, including cache counters.
Operation IDs deduplicate replayed transform reports.

A library caller can supply prices for disjoint counters:

```python
comparison = compare_runs(baseline_records, treatment_records, prices={
    "input_tokens": uncached_input_price_per_million,
    "cached_input_tokens": cache_read_price_per_million,
    "cache_write_tokens": cache_write_price_per_million,
    "output_tokens": output_price_per_million,
})
```

The result labels cost as `usage_times_supplied_tariff`. All required counters
and prices must be known. Output already includes reasoning, so adding a
reasoning charge again would double count. A projected prompt reduction alone
provides no measured billing result.

## Compare each integration path

| Path | Matched baseline | Treatment and coverage |
| --- | --- | --- |
| Library / CLI | Same captured input with `enabled=false` | Record returned transform reports; counter source states estimate or supplied tokenizer. |
| Direct `interact_with_*` | Same provider, model, headers, tools, and initial history; context disabled | Bind a recorder on each registry; compare provider usage and host task outcomes. |
| Ordinary actions / Flow | Same registry and composition, context disabled | Pass policy headers only; retain raw typed outputs for correctness comparisons. |
| Provider proxy | Same relay and route with transforms disabled | Compare projection/relay metadata; passively observe bounded JSON/SSE provider usage when recording is enabled. Compare direct traffic separately for relay overhead. |
| Shell hook / filter | Same captured completed result, `shell.mode=off` or `observe` | Compare `compress` presentation and reports; command execution/status remain identical. |
| Host history replacement | Same initial branch/session in independent arms | Record checkpoint proposal and host installation separately, plus auxiliary summary usage and task outcomes. |

Keep run IDs, workload identity, model/provider versions, tokenizer identity,
and outcome observations with your experiment records. Use isolated workspace
copies or read-only replay for paired agent tasks with side effects. A shadow
projection never requires re-executing the original tool.

Measure first-token latency at the host if that is the rollout criterion;
transform time alone omits transport, model queueing, and checkpoint calls.
Record failures, cancellations, partial coverage, and successful installations.
The utilities do not infer task quality or authorize automatic rollout.

## Run isolated host evaluations

`evaluate_pair(workload, invoke, setup=..., grade=...)` runs independent workload
copies through baseline and treatment callbacks. The callback owns session and
workspace isolation and records usage from every participating model, summary,
retrieval, and subagent call with distinct IDs. Setup metadata identifies the
provider/model, harness version, generation settings, tokenizer, and seed.
The comparison checks workload/setup/pair identities and reports mismatches.
Absent metadata leaves matching unverified. Whole-host wall time and response
first-byte samples stay separate from summed transform time. Proxy first-byte
measurement includes projection and relay setup; it is not first-token latency.

```python
from a11.sdk.llm_context import evaluate_pair

result = await evaluate_pair(
    workload,
    run_isolated_host,
    setup={"provider": provider, "model": model, "harness_version": version},
    grade=lambda outcome: outcome["answer"] == expected,
)
```

Each callback receives `(arm, workload_copy, record)`. The baseline disables the
selected transforms, and treatment enables them. Record task outcomes with the
grade callback. Provider-reported monetary totals can accompany host outcome
records; tariff-derived costs require complete disjoint usage figures.

Cumulative observations can carry `baseline={"scope": ..., "usage": ...}`.
The comparator subtracts a baseline only when session, agent, and epoch match.
SDK-normalized verified differences use `reporting="delta"`; raw cumulative
snapshots remain available as source evidence. Native compaction consumption
has a separate usage identity from the following generation.
A reset or missing baseline leaves coverage unknown. Partial response usage
remains partial and supplies no complete billing total. Known turn aggregates
cover only request details with matching agent/turn identity. Cross-agent
inclusive aggregates require host reconciliation records; the comparator does
not infer their coverage.

[Live validation results](validation.md) record input-token savings, exact
answers, and a default native-summary fact-retention failure. Use the library
callbacks above to run paired evaluations through a host's inference client.

## Inspect recorded deterministic sizes

Measurements on fixed synthetic JSON, repeated-log, search-record, and
repeated-prose inputs record these sizes on 2026-10-29:

| Synthetic workload | Original bytes | Model-view bytes | Estimated tokens before / after |
| --- | ---: | ---: | ---: |
| JSON records | 18,292 | 10,291 | 6,098 / 3,431 |
| Repeated log with a final error | 30,024 | 108 | 10,008 / 36 |
| Search records | 20,692 | 12,691 | 6,898 / 4,231 |
| Repeated prose | 6,800 | 113 | 2,267 / 38 |

Counts use `byte_estimate`; encoded envelopes include their representation
overhead. JSON whitespace removal preserves values and number/string lexemes.
Reversible envelopes preserve original bytes. Repetition drives the log/prose
results. These synthetic measurements establish representation size changes;
they provide no model comprehension, cache, billing, or task-success result.


## Measure Interaction views

Use `project_interactions(..., reporter=record)` for shared transform/compaction
reports, or pass a `RunRecorder` through action services. Compare identical
canonical snapshots and backend configurations with disabled and enabled policy.
Keep `view_id`, `source_digest`, covered IDs, installation outcome, experiment
arm, and quality grade with each run. Count the checkpoint's creation once and
record its reuse separately when switching backends.

Summary actions receive the ordinary usage attribution header
`x-a11-llm-context-operation=summarization`. Their `UsageReporter` events retain
provider counters and separate summary usage from generation. A host callback
using its own inference client supplies that client's usage records explicitly;
returning checkpoint text supplies no usage evidence. Paired evaluation and
`compare_runs` preserve unknown counters and unmeasured quality.

Grade the completed task on facts and constraints covered by both the summary
and retained history. Include tool dependencies, exact identifiers, opaque
content capability errors, and any retrieval in that coverage. Local token
estimates exclude provider framing, images, tool schemas, and server-held state.
Use authoritative provider usage for savings claims. Regression fixtures with
mocked boundaries establish projection/replay correctness; they do not establish
model quality across workloads.
