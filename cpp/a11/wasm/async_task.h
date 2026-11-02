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

#ifndef A11_WASM_ASYNC_TASK_H_
#define A11_WASM_ASYNC_TASK_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <absl/status/statusor.h>

#include "bridge_core.h"

namespace a11::wasm {

class AsyncTask : public std::enable_shared_from_this<AsyncTask> {
 public:
  using Work = std::function<absl::StatusOr<std::string>()>;

  static std::shared_ptr<AsyncTask> Start(Work work);
  ~AsyncTask();
  void Launch();
  Result* Poll();
  void Cancel();

 private:
  void Publish(std::unique_ptr<Result> result);
  void MaybeCancel();

  std::atomic<Result*> result_{nullptr};
  std::atomic<bool> consumed_{false};
  std::atomic<bool> cancel_requested_{false};
  std::atomic<bool> cancel_called_{false};
  std::unique_ptr<std::function<void()>> cancel_owner_;
  std::atomic<std::function<void()>*> cancel_{nullptr};
  Work work_;
};

}  // namespace a11::wasm

#endif  // A11_WASM_ASYNC_TASK_H_
