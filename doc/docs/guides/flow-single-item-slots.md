# Single-item transport

Flow can replace proven zero-or-one-item internal transports with one shared
item slot. Declared cardinality, value extraction and action interfaces retain
their existing meaning. This optimisation has no equivalent source edit.

## Proof obligations

The bound applies to every prefix, including executions that later fail. It is
per owning activation, rather than per compiled ref or entire program. Zero or
one is distinct from exactly one on successful completion. Neither bound proves
termination. A known literal describes a payload, not permission to publish it.

Eligibility accounts for every producer and write. Concurrent siblings add
bounds; alternatives of one branch join them. A loop writing an outer node can
repeat a write and is conservatively unknown. A node owned inside that loop has
a fresh bound per iteration. External ports, shared/public node maps, attached
node identities, escaping node IDs, failure sinks and unsupported producers
remain unknown. Unary annotations alone supply no proof. Native node storage and action boundaries
remain intact, including encoding, validation, store errors and cleanup.

## State correspondence

A baseline bus has one queue and terminal state per subscription. With a proven
bound of one, each queue contains either no item or the same shared item.
The replacement stores that item once, retains an independent consumed/stopped
position per subscription, and stores completion and error separately.

Before demand, both transports have unpublished payloads. A skipped subscription
does not demand production; abandoning an acquired subscription retains baseline
production demand. Publication changes the shared item state only at the
existing producer's emission event. Reading advances that subscription only.
An item is returned before a terminal error; an empty stream waits for terminal
state. Null remains an item. Completion, cancellation and reader wakeups use the
run's existing monitor. No slot owns an OS mutex.

Stream ref identity, independent subscription identity and the owner's shared
explicit-take cursor remain distinct. ValueCursor retains unary checks and
successive extraction semantics. Replay readers also retain the baseline
untimed `NextUntil` fallback. Replay readers retain the slot for the owning
activation and future nested activations, as the baseline replay buffer does.
Ordinary subscriptions release the retained item after all accounted readers
consume or abandon it. Slots are never shared between runtime activations.

## Removing a pump

A one-item proof alone changes storage, not execution placement. Derived stages
and native nodes retain their producer scheduling because they can emit an item
and then block, fail, or perform more work before completion.

Two restricted cases can produce directly in existing consumer execution:

- A literal expression has no lookups, host operations or blocking continuation.
  It is evaluated once on demand using the existing evaluator. Branch activation
  and phase gates still apply; no compile-time payload is published eagerly.
- An expression read exclusively by one initializer capture in its own body can
  run in that capture. Structural use and ownership checks establish that this
  is its only demander, with no replay reader. The baseline capture waits for
  the expression's first item; the replacement executes precisely that work in
  the same waiting step. Expression production has one emission followed only
  by return, so no blocking continuation moves before delivery. Its internal
  dependencies and independent sibling steps retain their existing fibers.
  Initializer execution remains eager, including unused values.

Pump removal is withheld in timing/race-sensitive flows. This restriction is
separate from slot eligibility. Representation-only slot transports keep their
producer pump and preserve the original per-read timing boundaries.

Eligibility evidence records the ref, owner, emission bound, producer premises,
known-literal status and pump policy. A structural verifier checks ownership,
uses and producer premises before execution selects a slot. Analysis limits or
unsupported facts retain baseline transport.

## Enable and inspect

The option defaults on. Compile an immutable program with it enabled:

```python
from a11 import flow

program = flow.loads(
    'flow main { out result: int\n 42 -> result }',
    single_item_slots=True,
)
evidence = program.optimizations()
```

`flow.load`, `ModuleDocument.compile`, `flow.run_program`, and native
`CompiledProgram::Compile` expose the same boolean. The executable frontends
accept `--no-single-item-slots` to select baseline execution. For example:
`a11 flow run --no-single-item-slots file.flow` and
`a11-flow-run --no-single-item-slots file.flow`. In Python pass
`single_item_slots=False` for the baseline. Execution never mutates a program's
transport choices.

The shared native service accepts:

```json
{"method":"optimizations","source":"flow main { 42 -> _ }","single_item_slots":true}
```

Its `flow.optimizations/v1` envelope reports each ref's owning body, declared
cardinality, established upper bound, exactness evidence, producer expression
or source ref, writer step IDs, eligibility, rejection reason and pump policy.
`applied` means the compiled layout is selected; inactive scopes and unused refs
do not allocate transports. With the optimisation disabled the report still
shows eligible candidates. `pump_free_eligible` describes a permitted pump removal;
`producer_pump` describes the configured transport for eligible candidates and
is null for rejected refs, which may use other existing storage mechanisms. False exactness means no
exactly-one guarantee was established, rather than proof of an empty stream.
No source edits or diagnostic fixes accompany this storage choice.

| Producer | Slot eligibility | Producer scheduling |
| --- | --- | --- |
| Expression result | At most one emission, retaining evaluator and validation | Pump-free for a literal or structurally exclusive initializer capture; otherwise retained |
| Reviewed count-preserving stage | Established upstream bound at most one | Retained, including item-before-terminal work |
| Reducing stage or literal `first 0/1` | Intrinsic output bound, independent of input declaration | Retained; upstream consumption and drains remain |
| Fresh, unescaped writable node in a private `nodes` map | All writes proven zero-or-one across mutually exclusive alternatives | Native node, serialization, stores, writer release and read pump retained |
| Bound values, statuses and winners | Existing lazy/preset storage | Existing implementation retained |
| Flow and action ports | Ineligible even with unary declarations | Existing node and action lifecycle retained |
| Shared/public node maps, attached nodes, escaped IDs, failure sinks | Ineligible | Existing implementation retained |
| Unknown sources, multiple writers that can coexist, outer nodes written by loops | Ineligible unless a separate supported bound establishes at most one | Existing implementation retained |

A call or attached-node alias sharing a candidate node's map blocks its writer
proof. Default action/session node maps remain ineligible because host code can
access them. Private named node maps remain eligible when those boundaries are
absent. This private node has one proven item while retaining non-unary
extraction semantics:

```a11flow
nodes scratch {
  n = node()
  42 -> n
  n -> result
}
```
Inference does not inspect an external implementation or trust its schema as an
emission bound. Large primitive literals are recognised directly; constant
containers use the existing native literal recogniser with a 4 KiB source cap.

Clocks, timed calls/waits, timeout/pace stages, races, merges and explicit
parallel/unordered work disable pump removal for the whole flow. Single-item
storage can still apply with producer scheduling retained. Replacing a replay
buffer also removes its copy-only fill pump: the shared slot itself retains the
one item and terminal state for future nested readers. No extra collection or
producer execution is added to achieve replay.

Analysis has a 200,000-operation optional quota and a depth bound of 128. Unknown
or incomplete evidence selects baseline transport. Optional work is charged to
the host's shared compilation budget; reaching its remaining work or time limit
falls back without invalidating a baseline that already passed compilation.
The structural verifier runs independently of candidate discovery; node writer verification additionally checks every
writer's owner path and pairwise branch exclusivity.

`RunOptions.transport_stats` optionally records actual bus, slot and replay-fill
pump entries, inline producers, allocated reader queues and slot states. Counters
are disabled when absent. They describe transport work, not all fibers,
allocations or retained bytes in the action and storage layers.

## Measurements

Measurements on 2026-10-30 use an Apple M5 Max (arm64, 18 logical CPUs), the
release preset (`RelWithDebInfo`), native host bridge, default executor settings
and the system allocator. Each mode runs in three fresh processes, with mode
order alternated, 100 warmups and 1,000 timed complete invocations per row.
Counters are disabled during timing and measured in a separate invocation.
No other builds or tests run concurrently; the desktop machine is not isolated.
The table reports medians of the three process-level percentiles.

| Workload | Baseline p50 / p99, µs | Slots p50 / p99, µs | Flow transport pump entries | Reader queues |
| --- | --- | --- | --- | --- |
| Literal, 64 bytes | 63.29 / 124.54 | 53.75 / 134.08 | 1 → 0 | 1 → 0 |
| Literal, 64 KiB | 238.08 / 309.04 | 229.79 / 276.67 | 1 → 0 | 1 → 0 |
| Scalar parameter, 64 bytes | 117.42 / 178.62 | 113.88 / 330.33 | 4 → 3 | 4 → 1 |
| Scalar parameter, 64 KiB | 294.29 / 520.71 | 268.17 / 433.21 | 4 → 3 | 4 → 1 |

The literal uses one shared slot and no transport producer fiber. The scalar
parameter uses three slots: one initializer produces inline, two derived
transports keep their pumps, and the external input retains one bus pump and
queue. Native action, port and storage work remains outside these counters.
These counts demonstrate removed queues and fiber starts; they do not count all
heap allocations in a complete invocation.

Proof construction, including structural checking, measures 0.21 µs median
for the literal and 0.63 µs for the scalar parameter. Median compilation for
the 64-byte fixture measures 5.58 → 5.96 µs and 7.62 → 8.92 µs respectively.
Larger programs can reach the optional quota and retain baseline transport.

OS peak RSS across all four workloads is 15.77–18.30 MiB for the baseline and
16.23–17.98 MiB with slots, with medians 17.64 and 17.05 MiB. The ranges overlap;
process RSS includes allocator retention, stacks, compilation and native stores.
It is not a per-slot allocation measurement or a general memory guarantee.
Small-payload p99 latency is worse in the enabled samples despite lower p50.
These figures describe the fixtures and runs, without establishing a speedup for
other programs, hosts or allocators.

The build's mimalloc backend faults during process shutdown after writing its
results, including in the existing direct-action benchmark with no Flow
execution. LLDB locates the fault in `mi_free_generic_mt` during static
finalization. Measurements above use cleanly exiting system-allocator runs;
this optimisation changes no allocator code.

Raw records, compiler identification and source fingerprint are in
`bench/runs/reference-flow-single-item-apple-m5-max.json`. Reproduce with:

```sh
export A11_DEPS_PREFIX=/path/to/static/dependencies
cmake --preset release -DA11_BUILD_BENCH=ON -DA11_ALLOCATOR=system
cmake --build --preset release --target a11_bench -j 8
A11_FIBER_WATCHDOG=10 cmake-build-release/cpp/bench/a11_bench \
  --suite flow --only single_item_off --scale 1 --json /tmp/slots-off.json
A11_FIBER_WATCHDOG=10 cmake-build-release/cpp/bench/a11_bench \
  --suite flow --only single_item_on --scale 1 --json /tmp/slots-on.json
```
