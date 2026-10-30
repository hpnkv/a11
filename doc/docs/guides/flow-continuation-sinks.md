# Continuation sinks

Append streams in reverse arrow order without creating intermediate nodes:

```a11flow
interact.new_interactions -> asked -> turn
```

This reads all of `asked`, then all of `interact.new_interactions`, into `turn`.
Pipelines bind more tightly than arrows. Arrows associate to the right;
parentheses preserve the destination/source distinction. A terminal comma list
broadcasts the ordered sequence. A comma before another ungrouped arrow is an
error. Assignment binds the resulting handle; trailing `after` gates the whole
transfer. A grouped inner transfer can have its own gate or `try` boundary.

A handle registers a transfer without waiting for completion. Aliases share its
identity. Handles are destinations, never payloads or source streams. Explicit
status reads, waits and cancellation use the existing completion mechanisms.

```a11flow
tail = a -> out
b -> tail
c -> tail
```

Both attachments reserve the destination before execution. They wait for `a`'s
segment to finish, then interleave. A handle's completion waits for its own
operation and every attached descendant. An attachment waits for its parent's
operation, not its complete descendant group. Independently declared producing
actions retain concurrent activation and required output draining.

## Lifecycle and correctness obligations

The compiled writer graph distinguishes operation completion, group completion
and physical destination closure. Each physical writer and observed group has
an initial reservation. Nested scopes hold one reservation while they can
create writers; skipped branches and completed scopes release it. Loop state
is per active iteration and retains no completed-iteration history.

A scope barrier waits for full handle completion. An attachment after that
barrier would make a completion cycle and is rejected with related locations.
Explicit gates delay activation without removing writer reservations.

A failed unhandled feeding operation aborts every destination it shares,
retains the initiating status and cancels other feeding operations. Source
producing actions retain their existing ownership. Cleanup releases reservations
before completion is published and before the owning monitor can be destroyed.

A completed callee releases its exclusive direct Flow-input argument feeders.
Other transfers retain owned producer cleanup; broadcasts continue feeding
their remaining destinations. Abandoning every reader of a Flow input stops
its transport wait, without cancelling an independently owned producer or
closing the caller's public node. Required action-output draining remains in
place.

`try` covers the complete transfer written after it, including its source and
writes. A handled source failure ends that transfer, retaining partial output;
subsequent attachments may continue. A failure that has already aborted a
physical destination remains a destination failure, including when a containing
branch handles its own error. Completed external effects remain completed.

## Storage and execution

Unobserved serial chains lower to source lists in one native transfer step.
Observed handles add virtual destination and group metadata, without an A11
node, queue or producer pump. Shared tails preserve their identity. Group
reservations use counters under the run monitor. Stable destination references
are resolved once per transfer activation. Forwarding preserves existing chunks
and uses bounded batches and existing store backpressure.

The normal execution path enables these reductions:

- Flatten an unshared serial chain into one transfer task. Preserve explicit
  inner `try` and `after` boundaries and named shared tails.
- Keep a virtual group counter only when attachments or cancellation need it.
  A status-only name observes its existing transfer task.
- Keep the first active physical feeder inline; allocate additional arbitration
  storage only for concurrent feeds. Discard-only observed groups allocate
  cancellation tracking only while feeds are active.
- Forward existing chunks without decoding them. Encode computed batches through
  the host bridge, preserving validation and store errors.
- Publish exclusive literal segments directly from their transfer task when the
  native single-item proof permits it. Batch at most eight literals and 64 KiB
  of source text; ordinary source batches retain existing backpressure.

Literal match templates and format strings reuse the existing native parsers.
Preparation is bounded to 64 KiB per program and 4 KiB per template. Small pure
literal predicates may reuse boolean results from the existing evaluator.
Arithmetic, conversions, clocks and effectful calls are excluded from folding.

## State correspondence

Compare a chain to its explicit `then` composition, and independent attachments
to an `interleave` composition after their common prefix:

| Observable state | Native writer graph |
|---|---|
| Registered but gated writer | Scope reservation exists; its task has not passed activation gates. |
| Current serial segment | One source cursor advances; later cursors remain unread. |
| Shared attachments after a prefix | The prefix operation is complete; each attachment advances independently. |
| Transferred item | The same item/chunk reaches each terminal destination through its normal writer. |
| Operation complete | Source consumption and owned source cleanup have finished. |
| Handle complete | Its operation and all registered descendants have completed; owned destination calls have joined. |
| Destination EOF | Every physical writer and scope reservation has released successfully. |
| Destination failure | The first initiating status is retained; outstanding feeding tasks are cancelled and joined. |

Flattening changes cursor storage and scheduling, while preserving this order
and the observable completion boundaries. A known literal proves its payload,
not permission to publish: branch activation, demand, gates and preceding
segments still control publication. Source aliases and explicit-take cursors
retain their existing identities. Repeated scopes create fresh writer state.

Completion callbacks perform writer release before publishing task completion;
scope joins retain the monitor until callback cleanup finishes. Only active
loop iterations own group state. Required outer-stream replay retains its
existing lifetime and can still materialise an entire stream.

## Grouping limits

Terminal broadcast, chains ending in broadcast, and attachments to a handle
whose terminal destination broadcasts are supported. A broadcast list containing
continuation handles is diagnosed; attach to one continuation per transfer.
An ungrouped comma followed by another arrow is also diagnosed.

Static cycle analysis includes full handle completion, barriers, gates, owned
calls and child-scope reservations. Runtime-dependent host/tool deadlocks remain
subject to the existing cancellation and deadline mechanisms.

## Measurements

Apple M5 Max, native host bridge, release preset with the system allocator.
Three fresh processes per workload and form alternate their execution order.
Chain/fan-in rows use 100 timed runs after ten warmups; loop rows use ten after
three. Compilation is outside execution timing. Allocation counts come from a
separate invocation and count C++ `new`, including worker threads, but exclude
direct `malloc` and `mmap`. Values below are medians across the three processes.

| Workload | Form | C++ allocations | Requested KiB | Median latency, µs |
|---|---|---:|---:|---:|
| 64 literal segments | Explicit `then` | 3,820 | 972.6 | 1,332.5 |
| 64 literal segments | Arrow chain | 1,786 | 493.4 | 248.5 |
| Prefix + 64 fan-in writers | `then interleave` | 3,923 | 754.0 | 1,106.6 |
| Prefix + 64 fan-in writers | Shared tail | 4,386 | 705.4 | 1,201.2 |
| 1,024 sequential iterations | One `then` transfer/pass | 132,860 | 25,698.1 | 26,910.9 |
| 1,024 sequential iterations | Named tail + attachment/pass | 167,700 | 26,245.1 | 35,509.7 |
| 1,024 iterations, parallel 4 | One `then` transfer/pass | 132,502 | 25,667.7 | 27,121.8 |
| 1,024 iterations, parallel 4 | Named tail + attachment/pass | 167,981 | 26,260.5 | 35,686.8 |

The 64-literal arrow chain uses one transfer task, zero virtual groups, zero
single-item slots and zero reader queues. Its explicit composition uses one
transfer task, 64 slots and 63 reader queues. Shared fan-in retains one virtual
group and one existing transfer task per independently attached writer.

Loop attachments retain two transfer operations per pass. Their group count
peaks at one in these short, nonblocking workloads, with zero groups retained
after execution at both 256 and 1,024 iterations. The parallel setting does not
force these short scopes to overlap. Named attachments have more allocations
and higher latency here than the single-transfer composition.

Process peak RSS medians, explicit/arrow forms respectively: chain 13.45/11.69
MiB, fan-in 14.30/13.66 MiB, sequential loop 18.70/18.14 MiB, parallel loop
19.22/18.23 MiB. A process includes both tested sizes, native stores and worker
stacks. Post-run live heap medians span 4.64–5.27 MiB and include allocator and
worker retention; they do not measure handle storage alone. Several RSS ranges
overlap, so these measurements do not establish a general resident-memory gain.

Raw counts, heap/RSS snapshots, timing summaries, build configuration and source
fingerprint are in
`bench/runs/reference-flow-continuations-apple-m5-max.json`.
Run a selected row in a fresh process:

```sh
A11_FIBER_WATCHDOG=10 /usr/bin/time -l \
  cmake-build-release/cpp/bench/a11_bench --suite flow \
  --only continuation_chain_native --json /tmp/chain.json
```

Use `continuation_chain_explicit` for its existing-runtime comparison. The other
row families are `continuation_fanin`, `continuation_loop` and
`continuation_parallel_loop`, each with `_native` and `_explicit` forms.

## Validation

Native language tests: 351 passed. Runtime/value/schema tests: 222 passed with
single-item slots disabled, and 222 with them enabled. The three Flow CTest
targets, installed-CMake smoke test and header canary pass. Public Flow, CLI,
SDK and gateway checks: 370 passed, one skipped. IntelliJ: 114 tests passed;
the 0.7.23 archive embeds the current macOS arm64 native tool.
Null/EOF ordering and tolerated broadcast feeding each pass 500 repetitions
with single-item slots disabled and another 500 with them enabled.

Generated syntax and binding stubs pass their freshness checks. All five
short-reference example blocks pass native checking and default formatting.
Clang-tidy runs complete with warnings; the unchecked-StatusOr checker is disabled
because it crashes with the installed LLVM build. One peer test initially
reported an aborted stream; its isolated run and subsequent full reruns pass.
