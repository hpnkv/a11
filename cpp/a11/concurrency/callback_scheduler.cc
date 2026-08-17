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
    if (!scheduled_) {
      scheduled_ = true;
      post = true;
    }
  }
  if (post) {
    thread::Post([this] { Run(); });
  }
}

void CallbackScheduler::Run() {
  size_t completed = 0;
  while (completed < max_callbacks_per_turn_) {
    absl::AnyInvocable<void() &&> callback;
    {
      thread::MutexLock lock(&mu_);
      if (callbacks_.empty()) {
        scheduled_ = false;
        return;
      }
      callback = std::move(callbacks_.front());
      callbacks_.pop_front();
    }
    // Every callback queued here is A11's own state-machine continuation, and
    // the queue takes them by value from Post() -- so there is nothing to wrap
    // at adoption and nothing that can throw. A caller's callable reaches a
    // state machine through Submit or a WireStream callback, both of which are
    // guarded where they are adopted.
    std::move(callback)();
    ++completed;
  }

  // Keep scheduled_ true while handing the next turn back to the worker pool;
  // producers can append without creating redundant pump callbacks.
  thread::Post([this] { Run(); });
}

}  // namespace a11::internal
