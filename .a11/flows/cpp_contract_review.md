# Review C++ nullability and Status discards

Use `cpp_contract_review.flow` to review specified files for:

- Raw pointers missing `absl_nonnull`, `absl_nullable`, or
  `absl_nullability_unknown`, including each level of nested pointers.
  The corresponding `absl::Nonnull`, `Nullable`, and `NullabilityUnknown`
  wrappers also count as annotations.
- Discarded `absl::Status` values that do not use `.IgnoreError()`, including
  `(void)` casts, `static_cast<void>` and bare unused calls.

This is model-assisted review, not a deterministic compiler check. The model
receives one complete source file per request, without following includes.
Unresolved types and macros belong in `uncertainties`. An empty findings
array is not proof that every pointer and Status use is compliant. Use a
type-aware compiler checker for mandatory CI enforcement.

## Inputs

- `sources`: required string stream, one source or header path per value.
  Files start reviewing as paths arrive. No shell commands run.
- `options`: optional `CppContractOptions` struct:
  - `max_concurrency`: positive integer, default `4`. Flow limits concurrent
    per-file reviews with its parallel map stage.
  - `max_source_bytes`: positive integer, default `120000`. The read fails
    if the whole file exceeds this bound; source text is not truncated.
  - `model_config`: provider-specific configuration object, default `{}`.

The host must register `read_file` and `interact_with_llm`. Select a direct
model provider and model through `x-a11-llm-provider` and `x-a11-llm-model`,
and supply authentication through the host. The flow forwards `x-a11-llm-*`
headers to its model calls. Coding-agent backends are rejected. No model
tools are supplied. Source text is sent to the selected provider; use a
provider approved for your repository. Choose limits that fit its context.

## Output

`reviews` streams one `CppContractReview` per input file, in completion order.
Each review has the caller-supplied `path`, `findings`, `uncertainties`, and
`error`. Use `path` to associate out-of-order results with their sources.
Findings have a `rule` of
`raw-pointer-nullability` or `status-discard`, a 1-based `line`, a `message`
with code evidence, and a `suggestion`. Uncertainties are strings describing
unresolved source locations and the missing context.

`error` is `{}` on success. A file-specific failure produces an error object
with `code`, `message`, and `details`, plus empty findings and uncertainties.
An error result is not a clean review. Other files continue processing.

Example inputs (the sources array represents individual stream values):

```json
{"sources": ["cpp/example.cc"], "options": {"max_concurrency": 2}}
```

Invalid paths, read failures, model failures, empty or oversized responses,
and invalid response JSON become per-file error results. Empty source
streams, invalid shared settings, and unsupported providers fail the flow.
Host cancellation or an overall deadline can still terminate the run.
