// Copyright 2026 The A11 Authors.

#include "a11/concurrency/executor.h"

#include <functional>
#include <memory>
#include <utility>

#include <absl/functional/any_invocable.h>
#include <absl/log/log.h>

#include "a11/exception_guard.h"
#include "thread/boost_primitives.h"
#include "thread/fiber.h"

namespace a11 {
namespace {

struct FiberControl {
  thread::Mutex mu;
  thread::Fiber* absl_nullable fiber ABSL_GUARDED_BY(mu) = nullptr;
  bool cancel_requested ABSL_GUARDED_BY(mu) = false;

  void Cancel() ABSL_LOCKS_EXCLUDED(mu) {
    thread::MutexLock lock(&mu);
    cancel_requested = true;
    if (fiber != nullptr) {
      fiber->Cancel();
    }
  }
};

}  // namespace

void Schedule(absl::AnyInvocable<void() &&> work,
              thread::TreeOptions tree_options) {
  // Wrapped rather than caught here: the fibre that runs this work belongs to
  // A11 and is compiled without exceptions, so a throw has to be stopped inside
  // the wrapper's own frame.
  thread::Detach(tree_options,
                 [work = exception_guard::WrapConsuming(
                      std::move(work), "Unobserved scheduled task")]() mutable {
                   std::move(work)();
                 });
}

std::function<void()> ScheduleCancelable(absl::AnyInvocable<void() &&> work,
                                         thread::TreeOptions tree_options) {
  auto control = std::make_shared<FiberControl>();
  std::unique_ptr<thread::Fiber> fiber = thread::NewTree(
      tree_options,
      [work = exception_guard::WrapConsuming(
           std::move(work), "Unobserved cancelable task")]() mutable {
        std::move(work)();
      });
  {
    thread::MutexLock lock(&control->mu);
    control->fiber = fiber.get();
    if (control->cancel_requested) {
      control->fiber->Cancel();
    }
  }
  // Handed to the pool to reap, rather than spending a second fiber to hold it.
  // It was ~15% of every fiber this process created.
  thread::ReapWhenFinished(std::move(fiber), [control]() mutable {
    thread::MutexLock lock(&control->mu);
    control->fiber = nullptr;
  });

  return [control]() {
    control->Cancel();
  };
}

}  // namespace a11
