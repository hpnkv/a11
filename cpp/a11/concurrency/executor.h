// Copyright 2026 The A11 Authors.

#ifndef A11_CONCURRENCY_EXECUTOR_H_
#define A11_CONCURRENCY_EXECUTOR_H_

#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

#include <absl/functional/any_invocable.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"

namespace a11 {

/// Schedule work on A11's fiber pool without returning a completion handle.
/// Work may await another A11 Future without consuming an OS worker thread.
void Schedule(absl::AnyInvocable<void() &&> work,
              thread::TreeOptions tree_options = {});

/// Schedule work and return an idempotent cooperative-cancellation function.
/// The scheduler retains the root fiber until it has been joined.
std::function<void()> ScheduleCancelable(absl::AnyInvocable<void() &&> work,
                                         thread::TreeOptions tree_options = {});

/**
 * @brief Continue with @p transform when @p future completes or the deadline
 * expires.
 *
 * A ready future is transformed inline; otherwise a fibre waits until
 * @p deadline. Use Then() when the operation has no deadline.
 *
 * @param future
 *   The operation to continue from.
 * @param deadline
 *   How long the fibre may wait when @p future is not already complete.
 * @param transform
 *   Called with @p future's result. Runs inline in the ready case and must not
 *   block.
 */
template <typename T, typename Fn>
auto ThenAfterWaiting(Future<T> future, absl::Time deadline, Fn transform)
    -> Future<typename std::invoke_result_t<
        Fn, const absl::StatusOr<T>&>::value_type> {
  using Result = std::invoke_result_t<Fn, const absl::StatusOr<T>&>;
  using U = typename Result::value_type;

  if (future.IsReady()) {
    return CompletedFuture<U>(transform(future.Await()));
  }
  return Submit<U>(
      [future = std::move(future), deadline,
       transform = std::move(transform)]() mutable -> absl::StatusOr<U> {
        const absl::StatusOr<T> result = future.Await(deadline);
        return transform(result);
      });
}

template <typename T>
Future<T> SubmitWithCancellationHook(
    absl::AnyInvocable<absl::StatusOr<T>() &&> work,
    std::function<void()> cancellation_hook, thread::TreeOptions tree_options) {
  Promise<T> promise;
  Future<T> future = promise.future();
  std::function<void()> cancel = ScheduleCancelable(
      [promise = std::move(promise), work = std::move(work)]() mutable {
        absl::StatusOr<T> result;
        if (thread::Cancelled()) {
          result = absl::CancelledError("Task cancelled before it started");
        } else {
          // The work is the caller's, and so is this instantiation; see
          // a11/exception_guard.h for why the guard belongs here rather than at
          // the call.
          const absl::Status raised = exception_guard::Attempt(
              [&] { result = std::move(work)(); }, "task");
          if (!raised.ok()) {
            result = raised;
          }
        }
        const absl::Status completion = promise.SetResult(std::move(result));
        (void)completion;
      },
      std::move(tree_options));
  // The promise has moved into the task, but both handles share its state.
  // Install cancellation through a temporary handle recovered from Future.
  future.SetCancellationCallbackForExecutor(
      [cancel = std::move(cancel),
       cancellation_hook = std::move(cancellation_hook)]() {
        if (cancellation_hook != nullptr) {
          cancellation_hook();
        }
        cancel();
      });
  return future;
}

/// Run status-returning work on the fiber pool and expose its Future.
template <typename T>
Future<T> Submit(absl::AnyInvocable<absl::StatusOr<T>() &&> work,
                 thread::TreeOptions tree_options) {
  return SubmitWithCancellationHook<T>(std::move(work), {},
                                       std::move(tree_options));
}

/// Run an operation that returns only a completion status.
inline Task SubmitTask(absl::AnyInvocable<absl::Status() &&> work,
                       thread::TreeOptions tree_options = {}) {
  return Submit<Unit>(
      [work = std::move(work)]() mutable -> absl::StatusOr<Unit> {
        ABSL_RETURN_IF_ERROR(std::move(work)());
        return Unit{};
      },
      tree_options);
}

}  // namespace a11

#endif  // A11_CONCURRENCY_EXECUTOR_H_
