// Copyright 2026 The Action Engine Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef THREAD_INTROSPECT_H_
#define THREAD_INTROSPECT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/container/inlined_vector.h>
#include <absl/time/time.h>

#include "thread/fiber_diagnostics.h"

/** @file
 *  @brief Reading fiber wait state out of a running or hung process.
 *
 * A11 blocks in fibers. In a fiber deadlock every OS thread is parked in the
 * scheduler, and the frames that explain the hang belong to fibers whose stacks
 * are mmap'd regions no thread points at, so `bt` and a core dump both miss
 * them.
 *
 * Each fiber records its wait state in a `FiberDiagnostics`.
 * `SnapshotFibers()` collects those and unwinds the parked stacks,
 * `FindWaitCycles()` reports deadlocks from mutex-ownership and join edges, and
 * `FormatFiberReport()` renders both.
 *
 * Four ways to obtain a report:
 *
 *   1. `FormatFiberReport()`, or `a11.debug.fiber_report()` in Python.
 *   2. `A11_FIBER_WATCHDOG=<seconds>`: an OS thread logs the report on a stall.
 *   3. `kill -USR2 <pid>`; see `RequestFiberDump()`.
 *   4. `scripts/a11_fibers.py` under LLDB or GDB, against a live process or a
 *      core file, when nothing inside the process can run.
 *
 * @see doc/docs/guides/debugging-concurrency.md
 */

namespace thread {

/// One fiber as of a `SnapshotFibers()` call.
struct FiberSnapshot {
  std::uint64_t id = 0;
  std::uint64_t parent_id = 0;
  /// Empty unless the fiber was named; see `SetCurrentFiberName`.
  std::string name;

  WaitKind kind = WaitKind::kRunning;
  /// The `Mutex`, `CondVar` or target fiber waited on. Waiters sharing one
  /// value wait for the same thing.
  const void* absl_nullable wait_object = nullptr;
  /// The fiber holding the mutex, or the fiber being joined; zero when the wait
  /// has no attributable other end.
  std::uint64_t blocking_fiber_id = 0;

  /// How long this fiber has been in its current wait. Zero when running.
  absl::Duration waited;
  /// `absl::InfiniteFuture()` for an untimed wait.
  absl::Time deadline = absl::InfiniteFuture();

  void* absl_nullable creation_pc = nullptr;
  const void* absl_nullable stack_lo = nullptr;
  const void* absl_nullable stack_hi = nullptr;

  /// Return addresses of the parked stack, innermost first. Empty when the
  /// fiber is running, when the walk was refused, or when `trace_raced` is set.
  absl::InlinedVector<void* absl_nullable, 32> stack;
  /// The fiber woke while its stack was read; the frames were discarded.
  bool trace_raced = false;

  absl::InlinedVector<const void* absl_nullable, kMaxRecordedSelectables>
      selectables;
  std::uint64_t waits_completed = 0;
};

/**
 * @brief Every live fiber, with parked stacks unwound.
 *
 * Holds the fiber registry lock across collection, so a parked stack is safe
 * to read: a registered fiber has not yet released it. Symbolizing happens
 * afterwards, outside the lock.
 *
 * Briefly serialises fiber creation and destruction, so this is not for a hot
 * loop. Callable from any thread, fiber or not.
 */
std::vector<FiberSnapshot> SnapshotFibers();

/// As above, with a frame cap. Zero skips the unwind, which is what a watchdog
/// poll wants.
std::vector<FiberSnapshot> SnapshotFibers(size_t max_frames);

/**
 * @brief Wait-for cycles in a snapshot.
 *
 * Only mutex ownership and joins give an edge whose other end is known, so a
 * cycle here is a deadlock. Condition variables and `Select` have no
 * discoverable signaller; `FormatFiberReport()` groups their waiters by wait
 * object instead.
 *
 * @return One vector of fiber ids per cycle, in wait order.
 */
std::vector<std::vector<std::uint64_t>> FindWaitCycles(
    const std::vector<FiberSnapshot>& snapshot);

/// Takes its own snapshot.
std::vector<std::vector<std::uint64_t>> FindWaitCycles();

/// Controls how much `FormatFiberReport` prints.
struct FiberReportOptions {
  /// Fibers waiting at least this long get a full entry. Zero reports all.
  absl::Duration stall_threshold = absl::ZeroDuration();
  /// Stack frames per fiber.
  size_t max_frames = 24;
  /// Include running fibers, which have no parked stack to show.
  bool include_running = false;
};

/// A symbolized report: a census by wait kind, any deadlock cycles, then the
/// stalled fibers with their stacks.
std::string FormatFiberReport(const FiberReportOptions& options = {});

/// `FormatFiberReport` to `LOG(ERROR)`, in chunks a log sink will accept.
void DumpFiberReport(const FiberReportOptions& options = {});

/**
 * @brief Names the calling fiber, for reports.
 *
 * Copied into fixed inline storage, truncating at `kFiberNameCapacity`. Naming
 * the fiber that owns a subsystem -- a wire pump, a store writer -- makes a
 * report readable without symbolizing every frame.
 *
 * No-op on a thread that is not a fiber.
 */
void SetCurrentFiberName(std::string_view name);

/// The calling fiber's id, or zero outside a fiber.
std::uint64_t CurrentFiberId();

/// Total completed waits across all live fibers. Two equal readings a second
/// apart mean nothing moved.
std::uint64_t TotalCompletedWaits();

/**
 * @brief Starts a watchdog thread that reports stalls on its own.
 *
 * An OS thread, not a fiber, so it still runs when no fiber can. Idempotent:
 * a second call adjusts the running watchdog's threshold.
 *
 * `A11_FIBER_WATCHDOG` (seconds) and `A11_FIBER_WATCHDOG_ABORT` install this on
 * first use of the fiber pool, so most processes never call it directly.
 *
 * A stall is reported once, not on every poll.
 *
 * @param stall_threshold
 *   Report once a fiber has waited this long.
 * @param abort_on_stall
 *   `LOG(FATAL)` after reporting, so a hung CI test fails with a report
 *   attached.
 */
void InstallFiberWatchdog(absl::Duration stall_threshold,
                          bool abort_on_stall = false);

/**
 * @brief Requests a report without producing one here.
 *
 * Async-signal-safe: one atomic store. The watchdog thread does the locking,
 * allocating and symbolizing.
 *
 * REQUIRES: a watchdog thread. `InstallFiberDumpSignalHandler` and
 * `InstallFiberWatchdog` both start one.
 */
void RequestFiberDump();

/**
 * @brief Installs a signal handler that requests a fiber dump.
 *
 * Reads `A11_FIBER_DUMP_SIGNAL` (a signal number, default `SIGUSR2`) when
 * `signal_number` is zero, and starts the watchdog thread that services the
 * request.
 *
 * @return Whether a handler was installed.
 */
bool InstallFiberDumpSignalHandler(int signal_number = 0);

namespace internal {

/// Applies `A11_FIBER_WATCHDOG`, `A11_FIBER_WATCHDOG_ABORT` and
/// `A11_FIBER_DUMP_SIGNAL`. Called once from the fiber pool's startup.
void InstallFiberDiagnosticsFromEnvironment();

}  // namespace internal
}  // namespace thread

#endif  // THREAD_INTROSPECT_H_
