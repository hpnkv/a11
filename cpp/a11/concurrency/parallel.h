// Copyright 2026 The A11 Authors.
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

#ifndef A11_CONCURRENCY_PARALLEL_H_
#define A11_CONCURRENCY_PARALLEL_H_

#include <cstddef>
#include <utility>
#include <vector>

#include <absl/functional/any_invocable.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "thread/concurrency.h"

/// @file
/// Utilities for running independent work concurrently.

namespace a11 {

/// Awaits futures that are already running, and returns every result.
///
/// This function starts no work. It awaits every future, including those after
/// a failed result, and returns results in input order.
template <typename T>
std::vector<absl::StatusOr<T>> AwaitAll(
    const std::vector<Future<T>>& futures,
    absl::Time deadline = absl::InfiniteFuture()) {
  std::vector<absl::StatusOr<T>> results;
  results.reserve(futures.size());
  for (const Future<T>& future : futures) {
    results.push_back(future.Await(deadline));
  }
  return results;
}

/// Runs each callable on its own fibre and waits for all of them.
///
/// Each callable runs to completion even after another fails; the function
/// returns the first error. Use it for independent ordered chains such as a
/// sequence of writes followed by a close. `stack_size` sets each fibre's stack.
absl::Status RunAllToCompletion(
    std::vector<absl::AnyInvocable<absl::Status() &&>> work,
    size_t stack_size = 256 * 1024);

}  // namespace a11

#endif  // A11_CONCURRENCY_PARALLEL_H_
