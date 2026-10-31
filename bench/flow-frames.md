# Measure Flow frame storage

Build and run the native benchmark with the system allocator:

```sh
export A11_DEPS_PREFIX="$HOME/.cache/a11-deps/arm64"
cmake --preset release -DA11_BUILD_BENCH=ON -DA11_ALLOCATOR=system
cmake --build --preset release -j 8 --target a11_bench
cmake-build-release/cpp/bench/a11_bench --suite flow --only frame_ \
  --scale 1 --json /tmp/flow-frames.json
```

## Measurement setup

Apple M5 Max, arm64 macOS, Apple clang, `RelWithDebInfo` (`-O2`), system
allocator. Each latency row has three warmups and 30 measured invocations.
Allocation sampling uses a separate invocation. Compilation is excluded.
C++ new counters include worker activity in the benchmark process and exclude
direct malloc/mmap. Small allocation samples and timing can vary with worker
cleanup, scheduling, and process allocator state.

The baseline is commit `b04273e`, rebuilt in an isolated checkout with the same
workload generator and system allocator. Its probe adds benchmark rows without
changing runtime code. The [baseline probe patch](probes/flow-frames-baseline.patch)
applies to that commit's `cpp/bench/bench_main.cc`; configure its release preset
with `-DA11_BUILD_BENCH=ON -DA11_ALLOCATOR=system` and run the same command.
The new rows run with zero cached capacity and with the
default 256 KiB cap. Those two policies use one runtime and one representation.
The baseline comparison includes shared analysis, dense state, inline objects,
pooling, callable cleanup, and the source-file refactor. The zero-cache versus
cached comparison isolates storage reuse within the new runtime.

Raw records:

- [Original runtime](runs/reference-flow-frames-baseline-apple-m5-max.json)
- [Frames, both cache policies](runs/reference-flow-frames-apple-m5-max.json)

Workloads cover a scalar output, 128 static bindings, 4,096 sequential passes,
1,024 passes with loop parallelism four, 16 by 16 nested passes, 1,024 escaping
4 KiB object payloads, an early 64-pass parallel peak followed by 1,024 sequential
passes, and 16 concurrent scalar invocations. Parallel loop pipelines are
parenthesized so the parallel modifier applies to the loop.

## C++ allocation calls

Values are calls per complete invocation, or per group of 16 invocations for
`concurrent`. Changes compare default caching with the original runtime.

| Workload | Original | Frames, no cache | Frames, cached | Change |
|---|---:|---:|---:|---:|
| small | 424 | 407 | 506 | +19.3% |
| static | 6,060 | 5,164 | 5,164 | -14.8% |
| sequential | 446,603 | 417,626 | 409,502 | -8.3% |
| parallel | 141,426 | 134,278 | 132,097 | -6.6% |
| nested | 21,876 | 22,004 | 21,494 | -1.7% |
| escaping | 117,231 | 118,164 | 116,135 | -0.9% |
| peak then steady | 133,332 | 126,120 | 123,913 | -7.1% |
| concurrent | 6,770 | 6,585 | 6,570 | -3.0% |

The long sequential loop reduces total allocation calls by about 8% and
requested C++ allocation bytes from 66.6 MB to 62.6 MB. The large static body
reduces calls by about 15%, although its single activation has no reuse. That
change comes from shared analysis and fixed state storage. Small invocations
have no reuse and show additional overhead in this sample. Escaping payloads
retain their independent allocation paths.

## Frame blocks and capacity

Block counts are capacity allocations, distinct from whole-runtime C++ new
calls. A block has an allocation header and a separate aligned storage region.
The active count follows actual overlapping activations. Reserved bytes include
headers, active blocks, and cached blocks; they exclude allocator overhead.

| Workload | Blocks, no cache | Blocks, cached | Reuses | Peak active frames | Peak reserved bytes |
|---|---:|---:|---:|---:|---:|
| small | 1 | 1 | 0 | 1 | 352 |
| static | 1 | 1 | 0 | 1 | 54,928 |
| sequential | 4,097 | 2 | 4,095 | 2 | 1,704 |
| parallel | 1,025 | 5 | 1,020 | 5 | 5,088 |
| nested | 273 | 3 | 270 | 3 | 1,600 |
| escaping | 1,025 | 2 | 1,023 | 2 | 976 |
| peak then steady | 1,089 | 18 | 1,071 | 17 | 8,384 |
| concurrent | 16 | 16 | 0 | 14 | 4,928 |

The sequential loop reuses one child block 4,095 times. The parallel loop holds
four child blocks alongside its root. The early peak uses 18 blocks across
1,089 activations. Cache retention remains capped; blocks larger than the cap
are discarded. Every measured invocation ends with zero reserved frame bytes.

## Latency

Median microseconds per complete invocation. These measurements do not establish
a universal latency improvement: the static and parallel workloads improve in
this run, while the small and sequential workloads regress. Allocation savings
are separate from scheduling and end-to-end latency.

| Workload | Original | Frames, no cache | Frames, cached |
|---|---:|---:|---:|
| small | 55.5 | 81.0 | 104.4 |
| static | 2,617.0 | 2,392.3 | 1,939.6 |
| sequential | 93,025.8 | 93,873.6 | 99,197.0 |
| parallel | 64,415.0 | 60,970.1 | 58,960.8 |
| nested | 4,082.9 | 4,312.7 | 4,309.9 |
| escaping | 32,851.5 | 33,434.4 | 32,201.9 |
| peak then steady | 33,991.6 | 35,634.4 | 34,745.6 |
| concurrent | 271.1 | 272.2 | 272.4 |

## Live population

The population row keeps 128 invocations parked on open inputs, samples at
populations 16, 32, 64, and 128, and fits the later three RSS points. Requested
active frame capacity is 576 bytes per invocation,
73,728 bytes in total. The process RSS slope is
about 79.7 KiB per invocation in this run. The heap
reading grows by 180.0 MiB; that reading
includes node storage, queues, fibers, and allocator state. These are different
quantities. Frame capacity is not total invocation memory. All frame capacity
is released after cancellation and completion.

## Coverage and validation

Frames cover fixed step state, captures, folds, call handles, cursors, buses,
lazy values, buffer coordination, destinations, and writer groups. Shared
single-item transports, payloads, queue contents, node storage, task groups,
host objects and fiber stacks retain their existing allocation paths. There
is no payload arena or new representation for Flow-defined structs.

- All 15 debug CTest targets pass, including 235 Flow runtime/value/frame tests.
- Full Python: 1,944 passed, 22 failed, 16 skipped, one expected failure. All 22
  failures reproduce against a separately rebuilt `b04273e`: SDK status logging,
  one Flow generation fixture, and one typing fixture.
- The final Flow and native-binding Python selection passes: 280 passed, one
  skipped.
- The extension is rebuilt; regenerated stubs pass `--check`.
- The installed CMake package passes the out-of-tree smoke test.
- Six storage tests pass with AddressSanitizer and UndefinedBehaviorSanitizer;
  all seven private frame tests pass with UndefinedBehaviorSanitizer. The
  address-instrumented probe excludes the object-table test because its
  uninstrumented Abseil dependency has a different sanitizer table layout.
  Full-runtime ASan and TSan are not run.
- Thread-safety compilation is clean. Configured clang-tidy checks run on the
  layout, scope and transport sources with existing warnings. The
  `abseil-unchecked-statusor-access` check is disabled for this run because the
  installed clang-tidy crashes in its dataflow analysis of `Submit`.

Tests cover alignment, arithmetic overflow, lazy/partial construction,
fallible block allocation, exact destruction, cache caps, retained-capacity
charging, budget eviction, local-call budgets, escaped values, independent
concurrent invocations, and four loop activations parked simultaneously in
host calls. Existing suites cover cancellation, status details, replay,
backpressure, continuation reservations, deadlines, and optional bindings.
