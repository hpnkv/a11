// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Completion values used by every asynchronous A11 operation.
 *
 * Future and Promise carry an `absl::StatusOr<T>` between producers and
 * consumers without exposing the fiber scheduler. Runtime code can await a
 * Future from an A11 fiber or an ordinary OS thread, attach a non-blocking
 * callback, or request cooperative cancellation.
 */

#ifndef A11_CONCURRENCY_FUTURE_H_
#define A11_CONCURRENCY_FUTURE_H_

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/functional/any_invocable.h>
#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "thread/boost_primitives.h"
#include "thread/fiber.h"
#include "thread/select.h"
#include "thread/selectables.h"

namespace a11 {

/// Empty success value used by Future<Unit> operations that return no data.
struct Unit {
  friend bool operator==(Unit, Unit) = default;
};

template <typename T>
class Future;

template <typename T>
class Promise;

namespace internal {

template <typename T>
struct FutureState {
  mutable thread::Mutex mu;
  thread::CondVar cv;
  bool ready ABSL_GUARDED_BY(mu) = false;
  std::optional<absl::StatusOr<T>> result ABSL_GUARDED_BY(mu);
  std::function<void()> cancel ABSL_GUARDED_BY(mu);
  thread::PermanentEvent event;
  std::vector<absl::AnyInvocable<void(const absl::StatusOr<T>&)>> callbacks
      ABSL_GUARDED_BY(mu);
};

template <typename T>
void InvokeFutureCallback(
    absl::AnyInvocable<void(const absl::StatusOr<T>&)> callback,
    const absl::StatusOr<T>& result) {
  try {
    callback(result);
  } catch (const std::exception& error) {
    LOG(ERROR) << "Future completion callback raised: " << error.what();
  } catch (...) {
    LOG(ERROR) << "Future completion callback raised a non-standard exception";
  }
}

}  // namespace internal

/**
 * @brief Run work on A11's fiber pool with application-specific cancellation.
 *
 * The returned Future requests both @p cancellation_hook and cancellation of
 * the scheduled fiber when Future::Cancel() is called. Use this for operations
 * that must also interrupt an external SDK or transport; ordinary cooperative
 * A11 work can use Submit(). Cancellation remains a request, so consumers must
 * still observe the Future's eventual result.
 */
template <typename T>
Future<T> SubmitWithCancellationHook(
    absl::AnyInvocable<absl::StatusOr<T>() &&> work,
    std::function<void()> cancellation_hook,
    thread::TreeOptions tree_options = {});

template <typename T>
Future<T> Submit(absl::AnyInvocable<absl::StatusOr<T>() &&> work,
                 thread::TreeOptions tree_options = {});

/**
 * @brief Shared handle to one asynchronous result.
 *
 * Futures are cheap to copy and may have several waiters. Await() integrates
 * with A11 fibers when called inside the runtime and parks an ordinary thread
 * otherwise. OnReady() is the callback-oriented path used by high-cardinality
 * pumps and language bindings.
 *
 * Cancellation is a request to the producing operation; callers must still
 * observe the future to learn its eventual result.
 */
template <typename T>
class Future {
 public:
  Future() = default;

  /// Whether this handle refers to shared completion state.
  [[nodiscard]] bool valid() const { return state_ != nullptr; }

  /// Whether the producer has published either a value or an error.
  [[nodiscard]] bool IsReady() const {
    if (state_ == nullptr) {
      return false;
    }
    thread::MutexLock lock(&state_->mu);
    return state_->ready;
  }

  /**
   * @brief Request cancellation from the operation producing this result.
   * @return OK when the request was delivered, or Unimplemented when the
   *   producer did not install a cancellation source.
   */
  absl::Status Cancel() const {
    if (state_ == nullptr) {
      return absl::FailedPreconditionError("Future is not valid");
    }
    std::function<void()> cancel;
    {
      thread::MutexLock lock(&state_->mu);
      if (state_->ready) {
        return absl::OkStatus();
      }
      cancel = state_->cancel;
    }

    if (cancel == nullptr) {
      return absl::UnimplementedError(
          "This Future does not have a cancellation source");
    }

    try {
      cancel();
      return absl::OkStatus();
    } catch (const std::exception& error) {
      return absl::UnknownError(error.what());
    } catch (...) {
      return absl::UnknownError(
          "Future cancellation raised a non-standard exception");
    }
  }

  /**
   * @brief Wait for and return the result up to an absolute deadline.
   *
   * A timeout or cancellation of the waiting fiber does not itself overwrite
   * the producer's result; another observer may continue waiting.
   */
  absl::StatusOr<T> Await(absl::Time deadline = absl::InfiniteFuture()) const {
    if (state_ == nullptr) {
      return absl::FailedPreconditionError("Future is not valid");
    }
    {
      thread::MutexLock lock(&state_->mu);
      if (state_->ready) {
        return *state_->result;
      }
    }

    // A dynamic A11 fiber must yield its worker instead of blocking it. Plain
    // external threads use a cv variable and do not need a fiber
    // scheduler installed merely to wait for an A11 operation.
    if (thread::GetPerThreadFiberPtr() != nullptr) {
      const int selected = thread::SelectUntil(
          deadline, {thread::OnCancel(), state_->event.OnEvent()});
      if (selected == 0) {
        return absl::CancelledError("Future wait cancelled");
      }
      if (selected < 0) {
        return absl::DeadlineExceededError(
            "Future was not ready before deadline");
      }
    } else {
      thread::MutexLock lock(&state_->mu);
      while (!state_->ready) {
        if (state_->cv.WaitWithDeadline(&state_->mu, deadline) &&
            !state_->ready) {
          return absl::DeadlineExceededError(
              "Future was not ready before deadline");
        }
      }
      return *state_->result;
    }

    thread::MutexLock lock(&state_->mu);
    if (!state_->ready) {
      return absl::InternalError("Future wake-up did not publish a result");
    }
    return *state_->result;
  }

  /**
   * @brief Run @p callback once when the result becomes available.
   *
   * The callback runs immediately when the future is already ready. It must
   * not retain the referenced StatusOr beyond the callback invocation.
   */
  void OnReady(
      absl::AnyInvocable<void(const absl::StatusOr<T>&)> callback) const {
    if (callback == nullptr) {
      return;
    }
    if (state_ == nullptr) {
      const absl::StatusOr<T> invalid =
          absl::FailedPreconditionError("Future is not valid");
      internal::InvokeFutureCallback<T>(std::move(callback), invalid);
      return;
    }
    const absl::StatusOr<T>* absl_nullable ready_result = nullptr;
    {
      thread::MutexLock lock(&state_->mu);
      if (!state_->ready) {
        state_->callbacks.push_back(std::move(callback));
        return;
      }
      ready_result = &*state_->result;
    }
    internal::InvokeFutureCallback<T>(std::move(callback), *ready_result);
  }

 private:
  void SetCancellationCallbackForExecutor(std::function<void()> cancel) {
    if (state_ == nullptr) {
      return;
    }
    thread::MutexLock lock(&state_->mu);
    if (!state_->ready) {
      state_->cancel = std::move(cancel);
    }
  }

  explicit Future(std::shared_ptr<internal::FutureState<T>> state)
      : state_(std::move(state)) {}

  std::shared_ptr<internal::FutureState<T>> state_;

  friend class Promise<T>;
  template <typename U>
  friend Future<U> Submit(absl::AnyInvocable<absl::StatusOr<U>() &&> work,
                          thread::TreeOptions tree_options);
  template <typename U>
  friend Future<U> SubmitWithCancellationHook(
      absl::AnyInvocable<absl::StatusOr<U>() &&> work,
      std::function<void()> cancellation_hook,
      thread::TreeOptions tree_options);
};

/**
 * @brief Move-only producer for a Future result.
 *
 * Obtain the consumer handle with future(), then complete the promise exactly
 * once with SetValue(), SetStatus(), or SetResult(). Destroying an incomplete
 * promise publishes a Cancelled status so agent pipelines cannot wait forever
 * on an abandoned operation.
 */
template <typename T>
class Promise {
 public:
  Promise() : state_(std::make_shared<internal::FutureState<T>>()) {}

  Promise(const Promise&) = delete;
  Promise& operator=(const Promise&) = delete;

  /// Transfer responsibility for completing or abandoning the shared state.
  Promise(Promise&& other) noexcept : state_(std::move(other.state_)) {}

  /// Abandon this state, then take responsibility for @p other's state.
  Promise& operator=(Promise&& other) noexcept {
    if (this != &other) {
      Abandon();
      state_ = std::move(other.state_);
    }
    return *this;
  }

  ~Promise() { Abandon(); }

  /// Return a consumer handle sharing this promise's completion state.
  [[nodiscard]] Future<T> future() const { return Future<T>(state_); }

  /// Install the operation invoked when a consumer calls Future::Cancel().
  absl::Status SetCancellationCallback(std::function<void()> cancel) {
    if (state_ == nullptr) {
      return absl::FailedPreconditionError("Promise is not valid");
    }
    thread::MutexLock lock(&state_->mu);
    if (state_->ready) {
      return absl::FailedPreconditionError("Promise is already complete");
    }
    state_->cancel = std::move(cancel);
    return absl::OkStatus();
  }

  /// Complete successfully with @p value.
  absl::Status SetValue(T value) {
    return SetResult(absl::StatusOr<T>(std::move(value)));
  }

  /// Complete with a non-OK status.
  absl::Status SetStatus(absl::Status status) {
    if (status.ok()) {
      return absl::InvalidArgumentError(
          "SetStatus requires a non-OK status; use SetValue for success");
    }
    absl::StatusOr<T> result;
    result.AssignStatus(std::move(status));
    return SetResult(std::move(result));
  }

  /// Complete with either a value or an error, waking every observer.
  absl::Status SetResult(absl::StatusOr<T> result) {
    if (state_ == nullptr) {
      return absl::FailedPreconditionError("Promise is not valid");
    }
    std::vector<absl::AnyInvocable<void(const absl::StatusOr<T>&)>> callbacks;
    const absl::StatusOr<T>* absl_nullable published = nullptr;
    {
      thread::MutexLock lock(&state_->mu);
      if (state_->ready) {
        return absl::AlreadyExistsError("Promise has already been completed");
      }
      state_->result.emplace(std::move(result));
      state_->ready = true;
      state_->cancel = {};
      published = &*state_->result;
      callbacks.swap(state_->callbacks);
    }
    state_->event.Notify();
    state_->cv.SignalAll();
    for (auto& callback : callbacks) {
      internal::InvokeFutureCallback<T>(std::move(callback), *published);
    }
    return absl::OkStatus();
  }

 private:
  void Abandon() {
    if (state_ == nullptr) {
      return;
    }
    bool ready = false;
    {
      thread::MutexLock lock(&state_->mu);
      ready = state_->ready;
    }
    // Never release the last state owner while its embedded mutex is locked.
    if (ready) {
      state_.reset();
      return;
    }
    SetStatus(absl::CancelledError("Promise was abandoned")).IgnoreError();
    state_.reset();
  }

  std::shared_ptr<internal::FutureState<T>> state_;
};

/// Return an already-successful future containing @p value.
template <typename T>
Future<T> ReadyFuture(T value) {
  Promise<T> promise;
  Future<T> future = promise.future();
  promise.SetResult(std::move(value)).IgnoreError();
  return future;
}

/// Return an already-completed future containing @p result.
template <typename T>
Future<T> CompletedFuture(absl::StatusOr<T> result) {
  Promise<T> promise;
  Future<T> future = promise.future();
  promise.SetResult(std::move(result)).IgnoreError();
  return future;
}

/// Return an already-failed future containing @p status.
template <typename T>
Future<T> FailedFuture(absl::Status status) {
  Promise<T> promise;
  Future<T> future = promise.future();
  promise.SetStatus(std::move(status)).IgnoreError();
  return future;
}

/// Asynchronous operation whose only successful result is completion itself.
using Task = Future<Unit>;

/// Return an already-successful Task.
inline Task ReadyTask() {
  return ReadyFuture(Unit{});
}

/// Return an already-failed Task.
inline Task FailedTask(absl::Status status) {
  return FailedFuture<Unit>(std::move(status));
}

/**
 * @brief
 *   Continue with @p transform once @p future completes, without a fibre.
 *
 * `Submit([f]{ return g(f.Await()); })` needs a worker only because Await()
 * blocks; @p transform does not. This runs it on whichever thread completes
 * @p future, or immediately on this one when @p future is already complete, so
 * the continuation costs no handoff to the worker pool -- ~8us, and the
 * dominant cost of the layers built out of it.
 *
 * Two obligations come with that. @p transform must not block: it may run on a
 * pooled fibre with a small stack or on a transport's own thread, and it runs
 * before the completing side gets on with its work. And it must not assume a
 * thread; see a11/concurrency/inline_pump.h for why.
 *
 * @param future
 *   The operation to continue from.
 * @param transform
 *   Called with @p future's result, returning the continued result.
 * @return
 *   A Future for @p transform's result, cancellable through to @p future.
 */
template <typename T, typename Fn>
auto Then(Future<T> future, Fn transform)
    -> Future<typename std::invoke_result_t<
        Fn, const absl::StatusOr<T>&>::value_type> {
  using Result = std::invoke_result_t<Fn, const absl::StatusOr<T>&>;
  using U = typename Result::value_type;

  if (future.IsReady()) {
    return CompletedFuture<U>(transform(future.Await()));
  }

  Promise<U> promise;
  Future<U> continued = promise.future();
  promise
      .SetCancellationCallback([future]() mutable { (void)future.Cancel(); })
      .IgnoreError();
  future.OnReady([promise = std::move(promise),
                  transform = std::move(transform)](
                     const absl::StatusOr<T>& result) mutable {
    promise.SetResult(transform(result)).IgnoreError();
  });
  return continued;
}

}  // namespace a11

#endif  // A11_CONCURRENCY_FUTURE_H_
