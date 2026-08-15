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
 * @brief
 *   Continue with @p transform once @p future completes, taking a fibre only if
 *   there is actually something to wait for.
 *
 * The shape every "wait for this, then report it" API has. Already-finished is
 * the common case and costs no worker, no scheduler hop and -- through a
 * language binding -- no event-loop turn, which together are two orders of
 * magnitude of what a fibre whose result has to be marshalled back costs. The
 * unfinished case takes a fibre because it has a deadline to honour.
 *
 * Prefer Then() when there is no deadline; it never needs a fibre at all.
 *
 * @param future
 *   The operation to continue from.
 * @param deadline
 *   How long the fibre may wait when @p future is not already complete.
 * @param transform
 *   Called with @p future's result once it has one. Runs inline on the caller's
 *   thread in the ready case, so it must not block.
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
  return Submit<U>([future = std::move(future), deadline,
                    transform = std::move(transform)]() mutable
                   -> absl::StatusOr<U> {
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
          try {
            result = std::move(work)();
          } catch (const std::exception& error) {
            result = absl::UnknownError(error.what());
          } catch (...) {
            result = absl::UnknownError("task raised a non-standard exception");
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
      std::move(tree_options));
}

}  // namespace a11

#endif  // A11_CONCURRENCY_EXECUTOR_H_
