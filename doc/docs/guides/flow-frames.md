# Configure Flow frame storage

Use `frame_cache_bytes` to set how much completed execution-frame storage a
runner retains. Zero disables caching. The default is 256 KiB. Both settings
use the same layouts, execution paths, and cleanup rules.

```python
from a11 import flow

program = flow.loads("flow main { out result: int\n 42 -> result }")
result = await program["main"].invoke(
    frame_cache_bytes=256 * 1024,
    frame_budget_bytes=1024 * 1024,
)
```

`flow.start`, `flow.invoke`, `flow.make_handler`, the native plan's
`make_handler`, and `flow.run_program` accept these settings. Native hosts use
`a11::flow::RunOptions::frames` or the interpreter's `RunOptions::frames`.

## Frame capacity and budgets

`frame_budget_bytes` limits requested frame-block capacity across one invocation
and its transitive local Flow calls within the compiled program. Cached blocks
remain charged until freed. A runner evicts its cached blocks when a different
layout cannot fit the remaining budget. Exceeding the budget or failing a
fallible block allocation returns `RESOURCE_EXHAUSTED`; Python receives
`a11.status.StatusException`.

The budget includes fixed frame storage and each block's header. It excludes
allocator overhead, fiber stacks, compiled metadata, external actions, host
objects, node stores, payloads, and runtime allocations outside the frame.
It does not impose a total process-memory ceiling or guarantee recovery from
every allocation failure.

Cache limits apply per runner. Each runner's cache belongs to its invocation;
concurrent invocations of a compiled program share immutable layouts and
analysis. They have separate frames, caches, and budget accounting. Local Flow
calls share their caller's frame budget and use their own runner caches.

## Frame contents and lifetime

Compilation finalizes a native layout for each body. Global step identities map
to dense local slots; capture names become compiled numeric identities. Child
bodies allocate on activation, so a branch or loop reserves storage for its
actual active bodies. Concurrent activations use distinct blocks.

Frames contain step state, completion statuses and order, captured values, fold
accumulators, and optional storage for call handles, cursors, buses, lazy
values, replay-buffer coordination, destinations, and writer groups. Reserving
an optional object's storage does not construct it or start its work.

Strings, lists, chunks, and host objects keep independent payload ownership.
Shared single-item transports, replay-buffer contents, reader queues, node
storage, task groups, and ordinary containers retain their existing allocation
paths. Flow-defined structs keep the existing object-value representation.

A completed activation remains live while its captures are read by its parent.
Before returning its storage, the activation joins owned work, releases readers,
callbacks and registrations, and destroys its data-bearing objects. Task
callable captures are destroyed before task completion is published. Flow group
completion callbacks release their captures before group completion is published.

A cached block retains its allocation header and raw storage. Reuse constructs
fresh typed slots; optional object slots begin empty. The runtime does not
blanket-zero the block. Previous values, statuses, handles and registrations
are released through their destructors. Values emitted from an activation keep
independent ownership after its frame is reclaimed.

Frame blocks have fixed addresses and checked alignment and size arithmetic.
Layouts are sealed before execution. No public value or handle borrows frame
storage. These frames are separate from Thread's fiber stacks.

## Inspect frame capacity

Native hosts can attach `FrameOptions::stats`, a shared `FrameStats` collector.
It records block allocations, reuses, activation count, active frame count, and
current and peak reserved, active, and cached capacity. Active bytes count an
active block's entire capacity, including unused optional slots. A collector
shared across invocations aggregates their counters and gauges.

The benchmark's `frame_*` rows measure timing and C++ allocation calls in
separate runs, exercise cached and uncached frames, and report frame counters.
`frame_live_population` fits an RSS slope over parked invocations. RSS includes
resources outside the frame budget. See the repository's
`bench/flow-frames.md` for measurements and reproduction commands.

## Extend runtime state

Add fixed state to the owning body's layout in `runtime_layout.cc`. Use a typed
`FrameLayout::Slots<T>` region for dense state, or a `FrameObjectLayout<T>` for
objects constructed lazily by logical identity. Finalize regions before sealing
the layout. Access slots through their typed descriptors; descriptors belong to
one immutable layout.

A new asynchronous construct must establish when its work, callbacks, readers,
and reservations have quiesced. Destruction and frame return follow that
boundary. Dynamic payload storage retains independent ownership unless a
separate scoped-lifetime argument supports moving it into a frame.

The runtime sources separate public compilation and handler APIs (`runtime.cc`),
layouts, monitoring, transports, destinations, scope lifetime, steps and loops,
streams, expressions, call dispatch, and schemas. Their private declarations
live under `cpp/a11/flow/internal/`. The standalone language library does not
link these runtime components.
