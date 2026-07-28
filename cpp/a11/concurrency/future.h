// Copyright 2026 The A11 Authors.

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

template <typename T>
Future<T> SubmitWithCancellationHook(
    absl::AnyInvocable<absl::StatusOr<T>() &&> work,
    std::function<void()> cancellation_hook,
    thread::TreeOptions tree_options = {});

template <typename T>
Future<T> Submit(absl::AnyInvocable<absl::StatusOr<T>() &&> work,
                 thread::TreeOptions tree_options = {});

template <typename T>
class Future {
 public:
  Future() = default;

  [[nodiscard]] bool valid() const { return state_ != nullptr; }

  [[nodiscard]] bool IsReady() const {
    if (state_ == nullptr) {
      return false;
    }
    thread::MutexLock lock(&state_->mu);
    return state_->ready;
  }

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

template <typename T>
class Promise {
 public:
  Promise() : state_(std::make_shared<internal::FutureState<T>>()) {}

  Promise(const Promise&) = delete;
  Promise& operator=(const Promise&) = delete;

  Promise(Promise&& other) noexcept : state_(std::move(other.state_)) {}

  Promise& operator=(Promise&& other) noexcept {
    if (this != &other) {
      Abandon();
      state_ = std::move(other.state_);
    }
    return *this;
  }

  ~Promise() { Abandon(); }

  [[nodiscard]] Future<T> future() const { return Future<T>(state_); }

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

  absl::Status SetValue(T value) {
    return SetResult(absl::StatusOr<T>(std::move(value)));
  }

  absl::Status SetStatus(absl::Status status) {
    if (status.ok()) {
      return absl::InvalidArgumentError(
          "SetStatus requires a non-OK status; use SetValue for success");
    }
    absl::StatusOr<T> result;
    result.AssignStatus(std::move(status));
    return SetResult(std::move(result));
  }

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

template <typename T>
Future<T> ReadyFuture(T value) {
  Promise<T> promise;
  Future<T> future = promise.future();
  promise.SetResult(std::move(value)).IgnoreError();
  return future;
}

template <typename T>
Future<T> CompletedFuture(absl::StatusOr<T> result) {
  Promise<T> promise;
  Future<T> future = promise.future();
  promise.SetResult(std::move(result)).IgnoreError();
  return future;
}

template <typename T>
Future<T> FailedFuture(absl::Status status) {
  Promise<T> promise;
  Future<T> future = promise.future();
  promise.SetStatus(std::move(status)).IgnoreError();
  return future;
}

using Task = Future<Unit>;

inline Task ReadyTask() {
  return ReadyFuture(Unit{});
}

inline Task FailedTask(absl::Status status) {
  return FailedFuture<Unit>(std::move(status));
}

}  // namespace a11

#endif  // A11_CONCURRENCY_FUTURE_H_
