# Debugging fiber deadlocks

A11 blocks in fibers, so a fiber deadlock does not look like a deadlock. Attach
a debugger to a hung A11 process and every thread reads the same:

```
thread #3: boost::fibers::algo::algorithm::suspend_until
thread #4: boost::fibers::algo::algorithm::suspend_until
thread #5: boost::fibers::algo::algorithm::suspend_until
```

The frames that explain the hang — the path that took the lock, the `Select`
nobody will ever satisfy — belong to fibers. When a fiber blocks,
Boost.Context swaps its stack off the physical thread and parks it in an mmap'd
region no thread points at. `thread apply all bt` cannot reach it, and neither
can a core dump on its own, because nothing in the dump says which regions are
fiber stacks or where their saved frames begin.

`thread/introspect.h` closes that gap. Every fiber records its wait kind, the
object it waits on and its frame pointer when it blocks; unwinding from that
frame pointer reconstructs the parked backtrace on demand.

## Getting a report

Four ways, in increasing order of how wedged the process is.

**From code.** In C++:

```cpp
#include "thread/introspect.h"

LOG(ERROR) << thread::FormatFiberReport();
```

In Python:

```python
import a11.debug

a11.debug.print_fiber_report()
```

**From the watchdog.** `A11_FIBER_WATCHDOG=<seconds>` starts a thread that logs
a report once any fiber has waited longer than that. It is an OS thread, not a
fiber, so it still runs when no fiber can:

```sh
A11_FIBER_WATCHDOG=10 python -m my_service
```

Add `A11_FIBER_WATCHDOG_ABORT=1` to abort after reporting, so a hung CI test
fails with a report attached instead of reaching the harness timeout with
nothing to look at. `ctest` sets `A11_FIBER_WATCHDOG=10` on the fiber tests, and
`a11/conftest.py` honours the same variable for `pytest`.

**From a signal.** `A11_FIBER_DUMP_SIGNAL` installs a handler, or call
`thread::InstallFiberDumpSignalHandler()` /
`a11.debug.install_fiber_dump_signal_handler()`. Then:

```sh
kill -USR2 <pid>
```

The handler sets one atomic and returns; the watchdog thread does the locking,
allocating and symbolizing, so the handler stays async-signal-safe.

`SIGUSR1` and `SIGUSR2` are the two signals POSIX reserves for application use,
so handling one changes nothing a supervisor relies on. The default is `SIGUSR2`
because `SIGUSR1` is claimed more often by runtimes and process managers.

A lifecycle signal is not a substitute:

- **`SIGTERM`** means "exit". A handler that reports and continues makes the
  process look like it is ignoring termination, and `systemd`, Kubernetes,
  `docker stop` and CI runners follow up with `SIGKILL` after their grace
  period. Reporting and *then* exiting would give one dump, at the moment you
  are trying to stop the process, which is not when a hang needs reading.
- **`SIGQUIT`** dumps core by default, and `scripts/a11_fibers.py` reads those
  core files. Handling it would remove the artefact the post-mortem path uses.
- **`SIGSEGV` and `SIGABRT`** are handled by Abseil's failure signal handler
  (`cpp/python/module.cc`). Adding a fiber report there would need the registry
  mutex, which the crashing thread may hold. On a crash, take the core file to
  `scripts/a11_fibers.py` instead.

**From a debugger.** For a process too wedged to run anything, or a core file:

```sh
lldb -p <pid> \
  -o 'command script import scripts/a11_fibers.py' \
  -o 'a11-fibers'

gdb -p <pid> -ex 'source scripts/a11_fibers.py' -ex 'a11-fibers'
```

The script registers two commands. `a11-fibers` is the fiber report;
`a11-hang` runs `thread backtrace all` first and then the report, which is the
pair a hang needs — the threads show every worker parked in
`PoolAlgorithm::suspend_until`, and the report says which fibers are stuck and
why.

### Loading it automatically

The repository has a `.lldbinit` and a `.gdbinit` that run the import, so a
debugger started in the repo root has both commands. Both debuggers require a
one-line opt-in first, because a directory-local init file runs arbitrary
commands.

LLDB, once, in `~/.lldbinit`:

```
settings set target.load-cwd-lldbinit true
```

Until you add it, `lldb` warns that the file exists and was skipped;
`settings set target.load-cwd-lldbinit false` silences the warning instead.

GDB declines the file and prints the line to add to `~/.gdbinit`:

```
add-auto-load-safe-path /path/to/a11/.gdbinit
```

### In CLion

The **Frames** pane cannot show a parked fiber, in CLion or any other debugger
UI. It lists what the debugger backend reports per thread, a parked fiber
belongs to no thread, and neither LLDB nor GDB exposes an API for adding a
synthetic thread to the list.

The script's output is reachable, though. CLion's Debug tool window has an
LLDB (or GDB) console tab that takes raw debugger commands:

```
command script import /absolute/path/to/scripts/a11_fibers.py
a11-fibers
```

For every session, put that import line in CLion's debugger startup commands
(**Settings → Build, Execution, Deployment → Debugger**). That path needs no
opt-in, because CLion runs the commands itself rather than having the debugger
read a directory-local file.

Frames print as `symbol at file:line`, the same shape LLDB prints, so the
console resolves them against the project index. IntelliJ IDEA without the C/C++
plugin cannot debug native code and has no path to this at all.

The script needs no cooperation from the target. It walks the fiber registry
and unwinds the parked stacks with the debugger's own memory reads, resolving
symbols to `file:line`. Field offsets come from the `a11_fiber_layout` symbol
rather than a compiled-in copy of the struct, so it keeps working when a field
is added.

## Reading a report

```
=== A11 fiber dump: 2 live fibers at 2026-08-29T16:50:09Z
census: mutex=2

--- deadlock: wait cycle of 2 fibers ---
  F#2 "cycle-right" waits mutex(0x10032c500) held-by=F#1 waited=205ms
  F#1 "cycle-left" waits mutex(0x10032c550) held-by=F#2 waited=205ms

--- 2 fibers waiting at least 0 ---
F#2 "cycle-right"  parent=F#0  created-at thread::internal::CreateTree()
     mutex(0x10032c500) held-by=F#1 waited=205ms
     #0  thread::MutexLock::MutexLock() at boost_primitives.h:72
     #1  (anonymous namespace)::MutexCycle()::$_1::operator()() at demo.cc:53
     ...
```

- **`census`** counts fibers by wait kind: `running`, `condvar`, `mutex`,
  `select`, `sleep`, `join`, `os-thread`. `os-thread` is the placeholder A11
  keeps for a thread that reached it without being a fiber.
- **A wait cycle is a deadlock.** Only mutex ownership and joins produce an edge
  whose other end is known, so a cycle is proof rather than a suspicion.
- **Condition variables have no discoverable signaller**, so they produce no
  cycle. The report instead prefixes waiters that share a wait object with
  `[N fibers wait on 0x...]`. Three readers on one channel with no writer left
  is the shape a `Select` deadlock takes.
- **`waited`** is time in the current wait. A `deadline=in ...` suffix appears
  for a timed wait.
- **Frame 0 is the caller of the blocking primitive**, because that primitive is
  the frame that recorded the frame pointer. How many intermediate frames appear
  depends on inlining.
- **`(woke while its stack was read; frames discarded)`** means the fiber
  resumed mid-walk, so the frames were dropped rather than reported. Take
  another report.

Name the fibers that own a subsystem, and reports become readable without
symbolizing anything:

```cpp
auto pump = thread::NewTree({.name = "wire-pump"}, [&] { ... });
thread::SetCurrentFiberName("store-writer");  // or from inside
```

## Prevent GIL deadlocks in the fiber scheduler

A11's fiber primitives block the *thread* that reaches them when it is not a
fiber: a contended `thread::Mutex`, a `Submit` whose fiber is dispatched inline,
a `Future::OnReady` racing the fiber that completes it. A thread that has run a
fiber carries a fiber scheduler, and a scheduler with nothing to run parks the
thread.

**Release the GIL before a Python thread enters a blocking native call.** A
scheduler park holding the GIL prevents the fiber that supplies the wake from
running. The report shows the parked thread at `PyEval_AcquireThread` with no
other thread running Python:

```
thread #2  take_gil <- PyEval_AcquireThread <- _native
thread #1  __psynch_cvwait  (main thread, in the fiber scheduler)
```

Neither end names the binding: the parked thread's Python frames sit on the
stack its registers no longer point at, so no unwinder bridges the switch.
`py-spy dump --pid` reads them from outside the process, or a `sys.setprofile`
hook logging `c_call` events for `a11._native` names the last binding entered.

Bindings release the GIL for the native call with `WithoutGil`,
`CallWithoutGil` or `ValueWithoutGil` from `cpp/python/interop.h`, and
`scripts/check_binding_gil.py` reports calls that retain it. The checker
resolves receiver types and one call hop. Shared helpers and C++ destructors
invoked by Python deallocation remain outside its reachability model.

The park itself drops the lock. `thread::SetSchedulerParkGuard`
(`cpp/thread/thread/executor.h`) takes a release/acquire pair, and both fiber
schedulers call it around the wait; `cpp/python/module.cc` installs
`PyEval_SaveThread` / `PyEval_RestoreThread` at import, guarded by
`PyGILState_Check()` and the interpreter's finalising flag. That covers the
paths outside the binding audit. The callback interface keeps `thread`
independent of the host runtime.

## Programmatic access

`thread::SnapshotFibers()` returns a `FiberSnapshot` per live fiber — id,
parent, name, wait kind, wait object, blocking fiber, wait duration, and the
unwound stack. `thread::FindWaitCycles()` returns the cycles. In Python,
`a11.debug.fiber_snapshot()`, `a11.debug.waiting_fibers()` and
`a11.debug.find_fiber_deadlock()` are the same three.

`thread::TotalCompletedWaits()` counts completed waits across all fibers. Two
equal readings a second apart mean nothing moved, which separates a hang from
slow progress.

Most of A11's data path runs on stackless callback pumps rather than fibers
(`ChunkStoreReader`, `ChunkStoreWriter`), so a snapshot of an idle process is
often empty. Fibers appear where A11 offers a synchronous-looking API and in the
flow runtime, which is where a report has something to say.

## Environment dials

| Variable | Effect |
| --- | --- |
| `A11_FIBER_WATCHDOG=<seconds>` | Report once a fiber waits this long |
| `A11_FIBER_WATCHDOG_ABORT=1` | Abort after the watchdog reports |
| `A11_FIBER_DUMP_SIGNAL=<signal>` | Install a dump handler; default `SIGUSR2` |
| `A11_FIBER_OWNER_TRACKING=0` | Stop recording mutex holders; no cycles |
| `A11_FIBER_CENSUS=1` | Count fiber creation sites, reported at exit |
| `A11_POOL_STATS=1` | Worker-pool counters, reported at exit |
| `A11_POOL_PIN=<spec>` | Pin pool workers to CPUs |

## What it costs

Recording a frame pointer is one register read, and everything except mutex
holder tracking sits on a path that already pays a context switch. Measured with
`fiber_introspect_bench` (RelWithDebInfo, Apple arm64, three runs), with
`A11_FIBER_OWNER_TRACKING` on and off:

| Operation | Tracking on | Tracking off |
| --- | --- | --- |
| Uncontended `Mutex` lock + unlock | 11.8–12.5 ns | 10.3–10.6 ns |
| Channel round trip | 6.1–6.5 µs | 5.9–6.2 µs |
| Fiber create + join | 1.56–1.80 µs | 1.49–1.76 µs |
| Snapshot, 65 fibers, no unwind | 2.6 µs | 2.6 µs |
| Snapshot, 65 fibers, 24 frames each | 3.0–3.2 µs | 3.0 µs |

Holder tracking adds about 1.5 ns to an uncontended lock and unlock pair: a
relaxed load, a relaxed store, and a `context::active()` call. The channel and
fiber-creation figures overlap between the two columns, so the instrumentation
on the blocking paths is below the noise of those operations.

Turning tracking off keeps wait state and parked stacks; it only drops the
mutex-holder edge, and with it cycle detection.

## Frame pointers

The unwind follows the frame record that `-fomit-frame-pointer` removes, so
`A11_FRAME_POINTERS` (default `ON`) adds `-fno-omit-frame-pointer` for A11 and
for the dependencies built from source. Configuring with
`-DA11_FRAME_POINTERS=OFF` leaves wait state, cycle detection and the report
intact, and truncates the stacks.

On an architecture with no frame-record layout in
`thread/internal/stack_walk.cc`, the report says
`(frame-pointer walk unsupported on this architecture)` rather than presenting
an empty stack as a fact. AArch64 and x86-64 are supported.

## Trying it

`fiber_deadlock_demo` hangs, in three shapes:

```sh
cmake --build --preset debug --target fiber_deadlock_demo
./cmake-build-debug/cpp/thread/fiber_deadlock_demo mutex-cycle
./cmake-build-debug/cpp/thread/fiber_deadlock_demo orphan-select
./cmake-build-debug/cpp/thread/fiber_deadlock_demo join-cycle
```

Each prints a report, then waits so `kill -USR2`, `A11_FIBER_WATCHDOG` and
`scripts/a11_fibers.py` can be tried against a live hang.
