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

}  // namespace thread

#endif  // THREAD_EXECUTOR_H_
