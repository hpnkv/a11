// Copyright 2026 The Action Engine Authors.

#ifndef THREAD_EXECUTOR_H_
#define THREAD_EXECUTOR_H_

#include <utility>

#include <absl/functional/any_invocable.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

namespace thread {

// Runs a short, non-blocking callback on Thread's shared worker pool without
// allocating a fiber stack. Callbacks may start asynchronous work and register
// continuations, but must not wait synchronously for that work to finish.
void Post(absl::AnyInvocable<void() &&> work);

// Like Post(), but makes the callback runnable at or after `deadline`.
void PostAt(absl::Time deadline, absl::AnyInvocable<void() &&> work);

inline void PostAfter(absl::Duration delay,
                      absl::AnyInvocable<void() &&> work) {
  PostAt(absl::Now() + delay, std::move(work));
}

/**
 * @brief A lock a parking scheduler drops for the duration of the park.
 *
 * A thread that has run a fiber carries a scheduler, which parks the thread
 * when no fiber is runnable. A host runtime uses this guard to release a global
 * lock around that park.
 *
 * `acquire` runs after `release` returns, on the same thread, whether the park
 * ended in a wake or a timeout. Both must tolerate being called on a thread
 * without the host lock; the CPython pair reads `PyGILState_Check()`. Install
 * the callbacks once, before fibers run. The callback interface keeps `thread`
 * independent of the host runtime.
 */
struct SchedulerParkGuard {
  /// Drops the host lock and returns what `acquire` needs to restore it.
  absl::AnyInvocable<void*() const> release;
  /// Restores the host lock from what `release` returned.
  absl::AnyInvocable<void(void*) const> acquire;
};

/// Installs the pair the fiber schedulers drop around a park. A default-built
/// guard removes it.
void SetSchedulerParkGuard(SchedulerParkGuard guard);

}  // namespace thread

#endif  // THREAD_EXECUTOR_H_
