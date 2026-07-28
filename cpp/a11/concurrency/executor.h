// Copyright 2026 The A11 Authors.

#ifndef A11_CONCURRENCY_EXECUTOR_H_
#define A11_CONCURRENCY_EXECUTOR_H_

#include <exception>
#include <functional>
#include <utility>

#include <absl/functional/any_invocable.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/concurrency/future.h"

namespace a11 {

// Schedule on the included fiber pool. Work may block on another A11 Future
// without consuming a worker thread, while callers from Python/libuv receive a
// callback-driven Future and never need to enter the fiber scheduler.
void Schedule(absl::AnyInvocable<void() &&> work);

// Like Schedule, but retains ownership of the root fiber until it is joined.
// The returned function can safely race completion and is idempotent.
std::function<void()> ScheduleCancelable(absl::AnyInvocable<void() &&> work);

template <typename T>
Future<T> SubmitWithCancellationHook(
    absl::AnyInvocable<absl::StatusOr<T>() &&> work,
    std::function<void()> cancellation_hook) {
  Promise<T> promise;
  Future<T> future = promise.future();
  std::function<void()> cancel = ScheduleCancelable(
      [promise = std::move(promise), work = std::move(work)]() mutable {
        absl::StatusOr<T> result;
        if (thread::Cancelled()) {
          result = absl::CancelledError("Task cancelled before it started");
        } else
          try {
            result = std::move(work)();
          } catch (const std::exception& error) {
            result = absl::UnknownError(error.what());
          } catch (...) {
            result = absl::UnknownError("task raised a non-standard exception");
          }
        const absl::Status completion = promise.SetResult(std::move(result));
        (void)completion;
      });
  // The promise has moved into the task, but both handles share its state.
  // Install cancellation through a temporary handle recovered from Future.
  future.SetCancellationCallbackForExecutor(
      [cancel = std::move(cancel),
       cancellation_hook = std::move(cancellation_hook)]() {
        if (cancellation_hook != nullptr)
          cancellation_hook();
        cancel();
      });
  return future;
}

template <typename T>
Future<T> Submit(absl::AnyInvocable<absl::StatusOr<T>() &&> work) {
  return SubmitWithCancellationHook<T>(std::move(work), {});
}

inline Task SubmitTask(absl::AnyInvocable<absl::Status() &&> work) {
  return Submit<Unit>(
      [work = std::move(work)]() mutable -> absl::StatusOr<Unit> {
        absl::Status status = std::move(work)();
        if (!status.ok())
          return status;
        return Unit{};
      });
}

}  // namespace a11

#endif  // A11_CONCURRENCY_EXECUTOR_H_
