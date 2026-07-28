// Copyright 2026 The A11 Authors.

#ifndef A11_CONCURRENCY_CALLBACK_SCHEDULER_H_
#define A11_CONCURRENCY_CALLBACK_SCHEDULER_H_

#include <cstddef>
#include <deque>

#include <absl/functional/any_invocable.h>

#include "thread/boost_primitives.h"

namespace a11::internal {

// A fair, stackless callback pump. Instances must have process lifetime because
// posted callbacks retain a raw pointer to the scheduler, while queued work
// owns the state it operates on.
class CallbackScheduler {
 public:
  explicit CallbackScheduler(size_t max_callbacks_per_turn = 64)
      : max_callbacks_per_turn_(max_callbacks_per_turn) {}

  CallbackScheduler(const CallbackScheduler&) = delete;
  CallbackScheduler& operator=(const CallbackScheduler&) = delete;

  void Schedule(absl::AnyInvocable<void() &&> callback);

 private:
  void Run();

  const size_t max_callbacks_per_turn_;
  thread::Mutex mu_;
  std::deque<absl::AnyInvocable<void() &&>> callbacks_ ABSL_GUARDED_BY(mu_);
  bool scheduled_ ABSL_GUARDED_BY(mu_) = false;
};

}  // namespace a11::internal

#endif  // A11_CONCURRENCY_CALLBACK_SCHEDULER_H_
