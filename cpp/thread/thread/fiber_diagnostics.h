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

#ifndef THREAD_FIBER_DIAGNOSTICS_H_
#define THREAD_FIBER_DIAGNOSTICS_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <absl/base/nullability.h>
#include <absl/time/time.h>

/** @file
 *  @brief What a fiber records about itself while it is blocked.
 *
 * A blocked fiber's stack is parked in an mmap'd region no OS thread points at,
 * so neither `thread apply all bt` nor a core dump reaches the frames that
 * explain a hang. Each fiber therefore stores its wait kind, the object it
 * waits on, and its frame pointer. Unwinding from that frame pointer
 * reconstructs the parked backtrace, and happens only when a report is
 * requested.
 *
 * @see thread/introspect.h
 * @see doc/docs/guides/debugging-concurrency.md
 */

namespace thread {

/// Why a fiber is not running. `kRunning` also covers a fiber blocked in an
/// uninstrumented call such as a native syscall.
enum class WaitKind : std::uint8_t {
  kRunning = 0,
  kCondVar = 1,
  kMutex = 2,
  kSelect = 3,
  kSleep = 4,
  kJoin = 5,
  /// The placeholder `Fiber::Current()` creates for an OS thread that reached
  /// A11 without being a fiber.
  kThreadPlaceholder = 6,
};

const char* absl_nonnull WaitKindName(WaitKind kind);

/// Encodes a deadline for `FiberDiagnostics::wait_deadline_nanos`. An untimed
/// wait becomes `INT64_MAX`.
std::int64_t DeadlineNanos(absl::Time deadline);

/// Selectables recorded for a `kSelect` wait. `CaseArray` is an
/// `InlinedVector<Case, 4>`; a longer Select is truncated here.
inline constexpr size_t kMaxRecordedSelectables = 4;

/// Names are copied into fixed inline storage, so a reader needs no allocator
/// and never dereferences a freed string.
inline constexpr size_t kFiberNameCapacity = 48;

/**
 * @brief Per-fiber wait state, written on blocking paths and read by a report.
 *
 * A member of `Fiber`. Mutable fields are atomic because the writer is the
 * fiber and the reader is a reporting thread, with no lock between them: a
 * fiber-aware lock here would deadlock a report diagnosing a jammed
 * fiber-aware lock.
 *
 * `epoch` is odd while parked in an instrumented wait and even otherwise. A
 * reader that sees the same odd value before and after walking the stack knows
 * the fiber stayed parked and the frames are valid; any other outcome is
 * reported as a raced trace.
 */
struct FiberDiagnostics {
  /// Process-wide, never reused. Zero means no fiber.
  std::uint64_t id = 0;
  std::uint64_t parent_id = 0;

  /// `boost::fibers::context*`, opaque here to keep this header Boost-free.
  /// `Mutex` records its holder as a context because reading one costs a
  /// thread-local load, where resolving a `Fiber*` costs a `dynamic_cast`.
  const void* absl_nullable context = nullptr;

  /// The fiber's stack, `[lo, hi)`, published from inside the fiber on entry.
  /// Bounds let the frame-pointer walk reject a corrupt chain.
  const void* absl_nullable stack_lo = nullptr;
  const void* absl_nullable stack_hi = nullptr;

  /// Return address of whoever started this fiber.
  void* absl_nullable creation_pc = nullptr;

  char name[kFiberNameCapacity] = {};

  std::atomic<std::uint32_t> epoch{0};

  std::atomic<WaitKind> wait_kind{WaitKind::kRunning};

  /// The `Mutex`, `CondVar` or target fiber record waited on. Waiters sharing
  /// one value wait for the same thing.
  std::atomic<const void* absl_nullable> wait_object{nullptr};

  /// For a `kMutex` wait, the context holding the mutex. The only wait-for edge
  /// with a known other end, and so the basis of cycle detection.
  std::atomic<const void* absl_nullable> wait_owner_context{nullptr};

  /// Frame pointer captured at the blocking call; the root of the later unwind.
  std::atomic<void* absl_nullable> wait_fp{nullptr};

  std::atomic<std::int64_t> wait_started_nanos{0};
  std::atomic<std::int64_t> wait_deadline_nanos{0};

  std::atomic<const void* absl_nullable> selectables[kMaxRecordedSelectables] =
      {};
  std::atomic<std::uint32_t> selectable_count{0};

  /// Completed waits. Equal totals across two readings mean nothing moved.
  std::atomic<std::uint64_t> waits_completed{0};

  /// Registry links, guarded by the registry mutex in introspect.cc.
  FiberDiagnostics* absl_nullable reg_next = nullptr;
  FiberDiagnostics* absl_nullable reg_prev = nullptr;
};

namespace internal {

/// The calling fiber's record, or null on a thread that is not a fiber. Costs a
/// `context::active()` and a `dynamic_cast`, so it belongs on blocking paths
/// only.
FiberDiagnostics* absl_nullable CurrentFiberDiagnostics();

/// Whether `Mutex` records its holder. Cached from `A11_FIBER_OWNER_TRACKING`,
/// default on.
bool OwnerTrackingEnabled();

/// Registry membership. Called from the `Fiber` constructors and from the top
/// of `~Fiber`, before the Boost context and its stack are released, so a
/// reader holding the registry lock can walk a registered fiber's stack.
void RegisterFiberDiagnostics(FiberDiagnostics* absl_nonnull record);
void UnregisterFiberDiagnostics(FiberDiagnostics* absl_nonnull record);

std::uint64_t NextFiberId();

/// Records the holder of the mutex a `kMutex` wait is blocked on.
void SetWaitOwnerContext(FiberDiagnostics* absl_nonnull record,
                         const void* absl_nullable owner_context);

/**
 * @brief Publishes a wait for the duration of a blocking call.
 *
 * Outermost wins: a scope whose fiber is already parked publishes nothing, so
 * the annotation from `SelectUntil` survives the `CondVar::WaitWithDeadline`
 * nested inside it.
 *
 * Only for paths that are about to block. On a fast path its stores are not
 * amortised against a context switch.
 */
class WaitScope {
 public:
  WaitScope(WaitKind kind, const void* absl_nullable object,
            void* absl_nullable frame_pointer, std::int64_t deadline_nanos = 0);
  ~WaitScope();

  WaitScope(const WaitScope&) = delete;
  WaitScope& operator=(const WaitScope&) = delete;

  /// The record this scope published to, or null when it did not publish.
  FiberDiagnostics* absl_nullable record() const { return record_; }

  /// Attaches the selectables of a `kSelect` wait.
  void RecordSelectables(const void* absl_nullable const* absl_nullable items,
                         size_t count);

 private:
  FiberDiagnostics* absl_nullable record_;
};

}  // namespace internal
}  // namespace thread

/// Publishes a wait from the frame that is about to block. A macro because the
/// frame pointer must be the caller's.
#define THREAD_WAIT_SCOPE(name, kind, object, ...) \
  ::thread::internal::WaitScope name(              \
      (kind), (object), __builtin_frame_address(0) __VA_OPT__(, ) __VA_ARGS__)

#endif  // THREAD_FIBER_DIAGNOSTICS_H_
