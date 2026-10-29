# Validate token savings and retained facts

Use a paired workload and a separate native-compaction continuation check to
measure input-token savings and preservation of required facts. The results
below are a small live check on 2026-10-29, with one workload and one sample per
arm and harness. They establish these observed outcomes; broader task quality
and production failure coverage require separate evaluations.

## Compare lossless compression

The workload contains 64 JSON records. The task reads the marker of record 7,
adds the units of records 19 and 5, and reads the marker of record 63. Its exact
expected answer is `LANTERN-427|44|KITE-742`. Each arm starts in an independent
session and temporary directory. Tools are disabled for Claude Code; Codex
uses a read-only workspace and receives an instruction to use no tools. No tool
calls occur. Both arms have the same model, instructions, and input values.

The baseline has context transforms disabled. Treatment selects
`input_content="lossless"`, `strategy="json"`, and `min_gain=0`. No generative
history compaction runs during these paired requests.

| A11 SDK surface | CLI / model | Reported input tokens, off | Reported input tokens, on | Reduction | Exact answer |
| --- | --- | ---: | ---: | ---: | --- |
| `interact_with_codex` | Codex 0.164.0 / `gpt-5.4-mini` | 13,825 | 12,803 | 1,022 (7.39%) | Pass in both arms |
| `interact_with_claude_code` | Claude Code 2.1.266 / `claude-haiku-4-5-20251001` | 2,669 | 1,318 | 1,351 (50.62%) | Pass in both arms |

These are provider/harness-reported counters, including harness overhead.
Cache-read and cache-write counters are zero in both arms. Codex output counts
are 66 and 77 tokens; Claude output counts are 16 and 16. The percentages above
refer to input tokens. All three requested values survive, and both answers
match the full expected string.

Claude Code reports per-arm costs of $0.002749 and $0.001398 for these requests.
The Codex comparison has no monetary estimate. One sample supplies no latency
comparison or statistical quality guarantee.

A second paired check through the standalone library uses
Claude Code 2.1.266, the same model and fact task, and Python SDK 0.2.148. It
reports 2,605 input tokens with compression off and 1,318 with compression on,
16 output tokens in each arm, matched setup metadata, and correct answers.
Its canonical JSON field ordering differs from the direct SDK check.

## Check native compaction and continuation

Codex's live `thread/compact/start` run produces a completed
`contextCompaction` item and matching completed turn. The continuation returns
the correct three-part answer. The direct SDK action also resumes a persisted
thread with `compaction="native"`, awaits compaction, and returns the correct
answer. These checks exercise both the programmatic coordinator and automatic
SDK integration with a fresh app-server process.

Claude Code's native command produces a `compact_boundary` and successful
result after three prior turns. The model has no transcript-reading tools.
The two observed cases are:

| Native Claude summary policy | Reported context before / after | Continuation |
| --- | ---: | --- |
| Default `/compact` | 2,883 / 218 | Fails: required facts are absent from the retained context. |
| `/compact` with explicit preservation instructions | 2,883 / 452 | Passes: exact expected answer. |

The preservation instruction identifies records and required fields without
supplying their values:

```python
await compact_claude_session(
    client,
    instructions=(
        "Preserve the exact markers and units for record ids 7, 19, 5 and 63, "
        "including the arithmetic answer already computed. These facts are "
        "required for the next task. Include the facts in the summary itself; "
        "no transcript file or tool access will be available."
    ),
)
```

The focused summary reduces reported context by 84.32%. Its summarization
request has additional token/cache costs; this context reduction is not a
measured net billing saving. A successful installation event provides evidence
of native completion, independently of task quality. The default-summary
failure remains part of this result.

A one-turn Claude session reports `Not enough messages to compact.` through
structured status and a successful command result. The coordinator returns
`installed=False` with `skip_reason="insufficient_history"` for this explicit
no-op. A success message without either a boundary or the structured no-op
evidence does not acknowledge compaction.

## Apply SDK boundary checks

An explicit Codex API-key header is adopted through app-server
`account/login/start` after initialization. Its credential store is ephemeral,
so the per-action override remains in the child process. An environment-only
key override does not authenticate the tested app-server version.

The Claude native coordinator accepts focused summary instructions and
structured insufficient-history no-ops. SDK generation after compaction uses
the preceding result from the same process as its usage baseline. Unverified
restored-session usage retains partial coverage. Session identity is read from
the initialization payload as well as message attributes.

These behaviors have regression tests independently of the live probes.

## Run a minimal pair

Use `project_messages` and `evaluate_pair` with isolated host callbacks as
described in the [measurement guide](measurement.md#run-isolated-host-evaluations).
Select the same model and harness version in both arms, record provider usage,
and grade the exact answer. Source credentials remain outside the report.
Transient native-protocol probes stay outside repository tests; regression tests
cover request conversion, key adoption, no-op evidence, and usage separation.
