// Copyright 2026 The A11 Authors.

#include "a11/concurrency/executor.h"

#include <exception>
#include <functional>
#include <memory>
#include <utility>

#include <absl/functional/any_invocable.h>
#include <absl/log/log.h>

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
  thread::Detach(std::move(tree_options), [work = std::move(work)]() mutable {
    try {
      std::move(work)();
    } catch (const std::exception& error) {
      LOG(ERROR) << "Unobserved scheduled task exception: " << error.what();
    } catch (...) {
      LOG(ERROR) << "Unobserved scheduled task non-standard exception";
    }
  });
}

std::function<void()> ScheduleCancelable(absl::AnyInvocable<void() &&> work,
                                         thread::TreeOptions tree_options) {
  auto control = std::make_shared<FiberControl>();
  std::unique_ptr<thread::Fiber> fiber = thread::NewTree(
      std::move(tree_options), [work = std::move(work)]() mutable {
        try {
          std::move(work)();
        } catch (const std::exception& error) {
          LOG(ERROR) << "Unobserved cancelable task exception: "
                     << error.what();
        } catch (...) {
          LOG(ERROR) << "Unobserved cancelable task non-standard exception";
        }
      });
  {
    thread::MutexLock lock(&control->mu);
    control->fiber = fiber.get();
    if (control->cancel_requested) {
      control->fiber->Cancel();
    }
  }
  thread::Detach({.stack_size = 256},
                 [control, fiber = std::move(fiber)]() mutable {
                   fiber->Join();
                   {
                     thread::MutexLock lock(&control->mu);
                     control->fiber = nullptr;
                   }
                   fiber.reset();
                 });

  return [control]() {
    control->Cancel();
  };
}

}  // namespace a11
