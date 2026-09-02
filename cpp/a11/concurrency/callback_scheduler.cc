// Copyright 2026 The A11 Authors.

#include "a11/concurrency/callback_scheduler.h"

#include <exception>
#include <utility>

#include <absl/log/log.h>

#include "thread/executor.h"

namespace a11::internal {

void CallbackScheduler::Schedule(absl::AnyInvocable<void() &&> callback) {
  if (callback == nullptr) {
    return;
  }
  bool post = false;
  {
    thread::MutexLock lock(&mu_);
    callbacks_.push_back(std::move(callback));
    // A turn already being in flight is not evidence that this callback will be
    // reached: see Run().
    if (active_turns_ < max_concurrent_turns_) {
      ++active_turns_;
      post = true;
    }
  }
  if (post) {
    thread::Post([this] { Run(); });
  }
}

// Callbacks queued here are stackless state-machine continuations. Concurrent
// turns keep unrelated continuations moving while one turn remains active.
void CallbackScheduler::Run() {
  size_t completed = 0;
  while (completed < max_callbacks_per_turn_) {
    absl::AnyInvocable<void() &&> callback;
    {
      thread::MutexLock lock(&mu_);
      if (callbacks_.empty()) {
        --active_turns_;
        return;
      }
      callback = std::move(callbacks_.front());
      callbacks_.pop_front();
    }
    // Every callback queued here is A11's own state-machine continuation, and
    // the queue takes them by value from Post() -- so there is nothing to wrap
    // at adoption and nothing that can throw.
    std::move(callback)();
    ++completed;
  }

  // Hand the next turn back to the worker pool, keeping this turn's slot in
  // `active_turns_` -- the turn continues, on a fresh frame.
  thread::Post([this] { Run(); });
}

}  // namespace a11::internal
