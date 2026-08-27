// Copyright 2026 The Action Engine Authors.
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

#include "a11/concurrency/parallel.h"

#include <cstddef>
#include <utility>
#include <vector>

#include <absl/functional/any_invocable.h>
#include <absl/status/status.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "thread/concurrency.h"

namespace a11 {

absl::Status RunAllToCompletion(
    std::vector<absl::AnyInvocable<absl::Status() &&>> work,
    size_t stack_size) {
  if (work.empty()) {
    return absl::OkStatus();
  }
  // One item is the common case for a narrow action, and spawning a fibre to
  // run it while this one waits is strictly worse than running it here: same
  // work, one extra handoff. The whole header exists to remove handoffs.
  if (work.size() == 1) {
    return std::move(work.front())();
  }

  const thread::TreeOptions options{.stack_size = stack_size};
  std::vector<Task> tasks;
  tasks.reserve(work.size());
  for (absl::AnyInvocable<absl::Status() &&>& one : work) {
    tasks.push_back(SubmitTask(std::move(one), options));
  }

  // Await every one of them, including after a failure: see the header.
  absl::Status first;
  for (const Task& task : tasks) {
    if (absl::Status status = task.Await().status();
        !status.ok() && first.ok()) {
      first = std::move(status);
    }
  }
  return first;
}

}  // namespace a11
