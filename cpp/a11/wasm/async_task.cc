// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "async_task.h"

#include <memory>
#include <utility>

#include <absl/status/status.h>

#include "a11/concurrency/executor.h"
#include "bridge_core.h"

namespace a11::wasm {

std::shared_ptr<AsyncTask> AsyncTask::Start(Work work) {
  auto task = std::make_shared<AsyncTask>();
  task->work_ = std::move(work);
  return task;
}

AsyncTask::~AsyncTask() {
  if (!consumed_.load(std::memory_order_relaxed)) {
    delete result_.load(std::memory_order_relaxed);
  }
}

void AsyncTask::Publish(std::unique_ptr<Result> result) {
  Result* expected = nullptr;
  if (result_.compare_exchange_strong(expected, result.get(),
                                      std::memory_order_release,
                                      std::memory_order_relaxed)) {
    result.release();
  }
}

void AsyncTask::MaybeCancel() {
  auto* cancel = cancel_.load(std::memory_order_acquire);
  if (cancel == nullptr || !cancel_requested_.load(std::memory_order_acquire)) {
    return;
  }
  bool expected = false;
  if (cancel_called_.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel)) {
    (*cancel)();
  }
}

void AsyncTask::Launch() {
  if (result_.load(std::memory_order_acquire) != nullptr) {
    return;
  }
  auto task = shared_from_this();
  auto work = std::move(work_);
  auto cancel = a11::ScheduleCancelable(
      [task, work = std::move(work)]() mutable {
        auto outcome = work();
        std::unique_ptr<Result> result(outcome.ok()
                                           ? new Result{0, std::move(*outcome)}
                                           : Failure(outcome.status()));
        task->Publish(std::move(result));
      },
      {});
  cancel_owner_ = std::make_unique<std::function<void()>>(std::move(cancel));
  cancel_.store(cancel_owner_.get(), std::memory_order_release);
  MaybeCancel();
}

Result* AsyncTask::Poll() {
  Result* found = result_.load(std::memory_order_acquire);
  if (found == nullptr) {
    return new Result{0, {}, false};
  }
  if (consumed_.exchange(true, std::memory_order_acq_rel)) {
    return Failure(absl::FailedPreconditionError("Task result was consumed"));
  }
  return found;
}

void AsyncTask::Cancel() {
  Publish(std::unique_ptr<Result>(
      Failure(absl::CancelledError("Task was cancelled"))));
  cancel_requested_.store(true, std::memory_order_release);

  MaybeCancel();
}

}  // namespace a11::wasm
