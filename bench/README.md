# A11 benchmarks

Quantitative input for deciding what to work on next. Every benchmark here
exists to answer a question somebody would otherwise settle by argument, and
answers it in a unit that survives being written down.

```bash
python -m bench --list                       # what there is
python -m bench                              # everything except the slow ramps
python -m bench --suite stores --suite wire  # some of it
python -m bench -k websocket                 # substring match
python -m bench --slow                       # include the capacity ramps
python -m bench --scale 0.1                  # a tenth of the iterations, for a smoke run

python -m bench --json runs/today.json                            # record
python -m bench --json runs/new.json --baseline runs/today.json   # and diff
```

Client-side, in the language the browser speaks:

```bash
cd js && npm run build
node --expose-gc bench/bench.mjs --json ../bench/runs/js.json
```

The core on its own, with no Python and no binding:

```bash
cmake -S . -B build/ctests -DA11_BUILD_BENCH=ON
cmake --build build/ctests --target a11_bench -j 10
./build/ctests/cpp/bench/a11_bench --json bench/runs/native.json
./build/ctests/cpp/bench/a11_bench --suite flow --only pipe_stages   # one row
```

`--only` runs the rows whose name contains a substring, which is what
attributing a process-wide counter to one operation needs: `A11_POOL_STATS=1`
counts fibres and thread wakes for the whole process, so a per-operation figure
is two runs of *one* row at different scales and a division of the differences.

All three runners emit the same record shape and use the same benchmark names,
so the tables line up and `--baseline` diffs across them.

Every run records the commit it was taken on (with a `-dirty` marker), and the
path, size and build time of the `_native` extension it loaded. Check those
before believing a `--baseline` diff: the extension is installed separately
from the source tree, so an editable install can be measuring a build from
days ago, and a diff against a file whose build cannot be reconstructed cannot
be attributed to anything.

**Why three.** A cost that shows up in all three is in the design. One that
shows up in Python and C++ but not TypeScript is in the C++. One that shows up
only in Python is in the binding. Without the native runner it is impossible to
tell those apart, and the difference decides who fixes what -- the first pass of
`FINDINGS.md` misattributed two findings for exactly that reason, and building
`a11_bench` corrected both. A native number is the **ceiling**: if an operation
runs at 3M/s natively and 40k/s through Python, the binding is the work; if it
is slow natively, no binding work will help.

## Two hosts

Everything above runs on one machine, which is how a microsecond gets
attributed and is also the one thing a deployment never is. The `link` and
`server` suites need a second host running an agent:

```bash
# on the host that is to be the server
python -m bench.peer --bind 0.0.0.0 --port 8899

# on the other one
A11_BENCH_PEER=192.168.1.209:8899 python -m bench --suite link --suite server
A11_BENCH_PEER=192.168.1.209:8899 python -m bench --suite server --slow  # the soak
```

Without `A11_BENCH_PEER` both suites skip with a reason, so a one-machine run is
unaffected.

**Always take the loopback control.** `A11_BENCH_PEER=127.0.0.1:8899` runs the
identical code, in the identical two-process topology, with no network in it.
Rows carry `link=loopback` or `link=lan` so the two land on different keys and
comparing them is deliberate. A LAN number quoted against the in-process `wire`
suite instead changes three things at once (the network, the process boundary
and the language binding) and can be read as evidence for whichever of the three
the reader already believed.

**Both directions, if the link is not symmetric.** Swap which host runs the
agent and run it again. A wireless client and a wired server have different
uplink and downlink capacities, and a transport that is fine one way can be
bounded the other.

**Read the server's CPU before its rate.** Every two-host row carries
`server_cores_busy` and `server_cpu_us_per_op`, sampled with `getrusage` inside
the server process. The rate says what was delivered; the CPU says whether the
host had anything left, and per-operation CPU is the only figure that survives
being moved between machines with different core counts. "The Linux box does 3x
the actions" and "the Linux box has more cores" are different claims, and the
second is the more common one.

## How to read a number

Three conventions, and they matter more than any individual figure.

**Latency and throughput are measured separately.** Timing each call
individually is how you get percentiles, and percentiles are what a user feels;
but the `perf_counter` pair costs enough to distort anything under a
microsecond. So sub-microsecond work is timed as a *batch* and reports a rate
only, and anything larger reports p50/p90/p99.

**Sequential and pipelined are different questions.** In Python every `await`
costs an event-loop turn, and on a selector loop that turn is a syscall — 14.6
microseconds on the reference machine, none of it A11's. A caller awaiting one
thing at a time cannot beat that no matter how fast the thing is. Benchmarks
that can report both do, and the gap between them is how much a caller wins by
having work in flight. Read `runtime/await_floor` before reading anything else.

**Memory is a slope, not a delta.** One before/after RSS reading is dominated
by pages the allocator had already reserved and by fixed process costs charged
to the first object. `memory_slope` builds the population in stages and fits a
line, so the reported figure is *marginal* bytes per object — which is the
number that answers "how many more of these can this host hold".

## The metric catalogue

Units are in the metric name: `_us` microseconds, `_per_s` a rate, `_bytes` /
`bytes_each` resident bytes, `mib_per_s` a byte rate, `inflation` a ratio.

### `runtime` — the floor

| metric | what it says |
|---|---|
| `bare_coroutine` p50 | what the language costs; not a floor on anything |
| `event_loop_turn` p50 | the real floor on sequential async throughput |
| `native_await` p50 | the cheapest call that crosses into C++ and back |
| `native_await` excess over the turn | A11's per-await marshalling, charged once per `await` everywhere |
| `thread_offload` p50 | `asyncio.to_thread`, which the registry uses for a *caller's* codec (its own run inline) |
| `await_already_resolved` p50 | Python's scheduling cost with no loop turn at all |
| `call_soon_round_trip` p50 | one full turn, scheduled the way A11's callbacks are |
| `io_poll_syscall` p50 | the kernel poll a turn ends with — most of the turn on macOS, little of it on Linux |
| `native_await[in_flight=N]` ops/s | what pipelining recovers |

Run the suite twice, `--loop asyncio` and `--loop uvloop`, before concluding
anything from the first three. On the reference machine uvloop buys only 1.2x,
because the floor is the kernel poll and uvloop polls too — but that balance is
platform-specific, and it decides how much A11's own per-await cost matters
relative to the loop's.

### `data` — the codec floor

| metric | what it says |
|---|---|
| `to_chunk` / `from_chunk` ops/s, MiB/s | per representation (JSON, MessagePack, bytes, text) and payload size |
| `to_chunk_dispatch` / `from_chunk_dispatch` `dispatch_tax_us` | what the registry costs *beyond* the encoder it calls |
| `bytes_payload` `inflation` | wire bytes per payload byte, per representation asked for |
| `model_to_chunk` / `model_from_chunk` | the same for a registered pydantic model — the typed-port path |
| `wire_to_msgpack` / `wire_from_msgpack` | the envelope codec, against fragments per message |
| `chunk_resident`, `fragment_resident` | bytes per live value |

### `stores` — what a node's data costs

Per backend (`local`, `sqlite`, `redis` when one is reachable):

| metric | what it says |
|---|---|
| `put` p50/p99, ops/s | single-fragment append; the producer's rate |
| `put_in_flight[in_flight=N]` | the same with a window outstanding |
| `put_many[batch=N]` items/s | batched append; the ratio against `put` is the case for batching |
| `get_by_seq`, `get_by_arrival` | random and arrival-ordered replay |
| `next_drain` items/s | the sequential consumer |
| `next_fanout[consumers=N]` | whether the shared cursor scales |
| `blocked_get_wakeup` | park, write, wake — the time-to-first-token primitive |
| `empty_store`, `stored_fragment` `bytes_each` | how many nodes, and how long each may live |

### `nodes` — the stream above the store

| metric | what it says |
|---|---|
| `node_create` ops/s, `node_resident` bytes | actions make two nodes per port, so this is per-call |
| `put[path=object\|chunk][stage=admitted\|confirmed]` | the object path against the chunk path, and admission against store confirmation |
| `drain[path=...]` items/s | what an output-port reader gets |
| `read_one_at_a_time[via=AsyncNode.next_fragment]` vs `read_batched[via=ChunkStore.next(limit=64)]` | the node read against the store's batched read on identical data; the gap is headroom the node layer has not claimed |
| `put_then_read`, `producer_consumer` | the streaming unit, both pacings |
| `replicated_read[readers=N]` | one node, several readers |

### `wire` — the transport

Per transport (`in-process`, `websocket`, `sse`):

| metric | what it says |
|---|---|
| `message_round_trip` p50/p99 by size | the floor under every remote dispatch |
| `one_way` ops/s, MiB/s by size | sustained rate; where the two curves cross is where to batch |
| `live_stream` `bytes_each` | **how many streams a process can hold** |
| `live_stream` ops/s | the connection-open rate |
| `unread_messages_before_abort` | how far a sender may outrun its reader before the stream dies — and the transports disagree |
| `concurrent_round_trips[streams=N]` (slow) | aggregate rate and per-stream percentiles under load |

### `actions` — the unit A11 is counted in

| metric | what it says |
|---|---|
| `local_action[ports=...]` | built, run and awaited here; the ceiling |
| `dispatched_action[ports=...]` | over a session; the gap is the session's own cost |
| `actions_in_flight[in_flight=N]` | **actions per second**, and whether concurrency buys anything |
| `local_in_flight`, `sessions_in_flight` | attribution: is the ceiling the loop, the session, or the transport |
| `live_action[ports=...]` `bytes_each` | a wide schema is a memory decision |

### `service` — capacity

| metric | what it says |
|---|---|
| `live_session` `bytes_each`, ops/s | resident cost per served connection, and the accept rate |
| `loop_turn_under_load[idle_sessions=N]` | whether idle sessions are genuinely idle |
| `actions_across_connections[connections=N]` | the gateway-sizing number |

### `flow` — the language and the runtime

| metric | what it says |
|---|---|
| `tokens`, `parse`, `check`, `format` p50/p99 by document size | editor latency on a keystroke; the curve's *shape* against size is the point |
| `complete` p99 | the slowest thing an editor asks for |
| `compile_source`, `program_resident` | what a gateway pays at startup and holds |
| `action_direct` vs `flow_run[steps=1]` | what expressing a call as a flow costs |
| `flow_run[steps=N]` | per-additional-step cost |
| `pipe_values` items/s | the streaming path through `\|` |
| `flows_in_flight` | concurrent whole flows |

The native runner has the same names plus four rows the Python one cannot have,
because they are about the runtime rather than the language: `pipe_stages` (the
cost of stage *n*, which is what says whether a pipeline paces itself or runs
value by value), `pipe_prefilled` (the same pipeline over a stream that is
already there, which is where a stage sees several values at once),
`for_each_call[parallel=N]`, and a native `action_direct` baseline. **Read the two
tables together**: the difference between them is the host bridge, and per value
that is most of the cost — see "the cost of a Python-hosted flow *is* the
crossing" in `FINDINGS.md`.

### `workload` — the calls people actually make

| metric | what it says |
|---|---|
| `interact_with_llm[tokens=N]` | a turn with a fake provider; the slope is A11's per-token cost |
| `turns_in_flight` | concurrent conversations |
| `shell_execute[lines=N]` | the bash tool against a real shell; the slope is A11's per-line cost |
| `subprocess_floor` | the same command with no A11 at all, for scale |

### `link` — the wire, over a wire

Needs a `bench.peer` agent. Every row carries `link=loopback|lan` and the
server's own CPU cost.

| metric | what it says |
|---|---|
| `tcp_round_trip[size=N]` | a bare socket ping-pong with no A11 in it: the floor this link imposes |
| `tcp_one_way` MiB/s | bytes at the socket into a peer that only reads: the ceiling every transport row below is a fraction of |
| `message_round_trip[transport,size]` | the same row `wire` measures, with a real RTT under it |
| `stream_throughput[transport,size]` | windowed one-way rate; `gbit_per_s` against the link's rating |
| `wire_inflation[transport,size]` | **bytes on the link per byte of payload** — the measurement loopback cannot make, and the one that prices a base64 body |
| `stream_connect[transport]` | opening a stream to another machine, per transport |
| `peer` | not a measurement: what the other host is, recorded in the run |

### `server` — the shape a deployment has

Needs a `bench.peer` agent. The unit here is a *window* of wall clock in which
several client populations work at once, not a repeated single call.

| metric | what it says |
|---|---|
| `join_then_call[idle_population=N]` | one client's join-to-first-result, with a population already connected |
| `join_burst[idle_population=N]` | clients dialling at once: the reconnect-storm ceiling |
| `steady_actions[churn=off\|on]` | **the headline** — established clients' rate and tail, with and without joins and leaves underneath, measured back to back on the same connections. `degradation_*` is the ratio |
| `churn_cycle` | sustained connect + call + disconnect rate while that load runs |
| `population_alone` / `population_mixed[population=...]` | four workload kinds solo, then together; `interference` is solo over mixed |
| `mixed_total` | the aggregate, to be read after `server_cores_busy` and not before |
| `survivor_latency[dropped=N]` | what a mass disconnect does to the clients that stayed; `spike_p99` is during over before |
| `soak_bucket[population=...]` (slow) | per-second buckets over minutes: `cov`, `drift`, `worst_second`, and RSS growth |
| `actions_by_clients[clients=N]` (slow) | the capacity curve; the *knee* is the result, and how many cores were busy at it |

`p999_us` is a column in the table rather than a note, because it is the figure
a server is judged on and a p50 is the figure a reader defaults to.

### `a11_bench` (native)

`cpp/bench/` mirrors a subset of the Python suites -- `data`, `stores`,
`nodes`, `actions`, `wire` -- using the same names for the same operations, so
a native row and a Python row can be put beside each other without a mapping
table. It is off by default (`A11_BUILD_BENCH=OFF`): a benchmark binary links
the whole runtime and runs for minutes, and neither belongs in the target CI
builds on every commit.

`cpp/bench/harness.{h,cc}` is a deliberate reimplementation of
`bench/harness.py`'s method -- separate latency and throughput loops, the same
percentile definition, the same RSS-slope fit. If one of them changes, change
both, or the comparison stops meaning anything.

Each suite prints its table and rewrites the JSON as soon as it finishes, so a
crash costs only the suite it happened in. **Check the exit status** --
`a11_bench | tail` reports `tail`'s success, and a suite that dies on a signal
looks exactly like one that printed nothing:

```bash
./build/ctests/cpp/bench/a11_bench --json bench/runs/native.json > /tmp/out; echo $?
```

## Adding one

A suite module is a list of decorated coroutines and nothing else:

```python
@benchmark("mysuite", "what_it_measures")
async def what_it_measures(scale: float) -> list[Result]:
    """One line an engineer can read in `--list`."""
    return [Result("mysuite", "the_thing", await latency(op, iterations=_scaled(1000, scale)))]
```

Rules worth keeping:

- Multiply every iteration count by `scale`.
- Use `throughput`/`throughput_sync` under a microsecond, `latency` above it.
- Use `pipelined` wherever a real caller would have work in flight, and report
  it *beside* the sequential number rather than instead of it.
- Use `memory_slope`, never a single `MemoryProbe` delta, for anything reported
  per object.
- `slow=True` for anything over about a minute; `raise Skip(...)` when a
  prerequisite (a Redis, a binary, an API key) is not present. A missing
  dependency is a skip with a reason, never a failure.
- Put the interpretation in the `note`. A number without one is a number
  somebody will misread in three months.
