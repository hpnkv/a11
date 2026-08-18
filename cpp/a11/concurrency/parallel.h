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
/// Running independent work at the same time instead of one item at a time.
///
/// The shape this exists to remove is a loop that starts one future and awaits it
/// before starting the next. Every `a11::Submit` is a handoff to a worker thread
/// -- the dominant cost in the action path, see `bench/FINDINGS.md`, "The action
/// ceiling is thread handoffs, not messages" -- so a loop over N independent
/// nodes pays N of them in series where it could pay one round.
///
/// **The work has to be genuinely independent.** `FINDINGS.md`'s "Batch
/// concurrent work, not a serial chain" records two batching attempts that
/// measured at zero because each step was caused by the previous one. Awaiting
/// several *already-running* operations one at a time is also fine as it stands:
/// the total is the slowest, not the sum, and `Session::AwaitAllActions` is
/// deliberately left that way.

namespace a11 {

/// Awaits futures that are already running, and returns every result.
///
/// The caller starts all of them first, which is the whole point: by the time
/// this is called the work is in flight, so the cost is the slowest item rather
/// than the sum. Nothing here starts anything.
///
/// Every future is awaited even if an earlier one failed, because the callers are
/// teardown paths where abandoning the rest leaks whatever they would have
/// closed.
template <typename T>
std::vector<absl::StatusOr<T>> AwaitAll(
    std::vector<Future<T>> futures,
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
/// For work that is more than one future per item -- a chain of puts and then a
/// close, say -- where the chain has to stay ordered but the chains do not have
/// to be ordered against each other.
///
/// **Everything runs to completion and the first error is returned**, which is
/// the opposite of the `Group` in `flow/runtime.cc`: that one cancels its
/// siblings as soon as one fails, because a flow whose step failed has pumps
/// waiting for data that is not coming. A teardown that gave up on the first
/// failure would leave the remaining nodes open, which is the bug class this
/// header is for. Callers wanting cancel-on-failure should use Flow's `Group` or
/// spawn directly.
///
/// `stack_size` is explicit because A11's pooled fibre stacks are small (see
/// `thread/thread_pool.cc`); anything reaching the node or store layers needs a
/// real one, and the default here is what Flow uses for the same reason.
absl::Status RunAllToCompletion(
    std::vector<absl::AnyInvocable<absl::Status() &&>> work,
    size_t stack_size = 256 * 1024);

}  // namespace a11

#endif  // A11_CONCURRENCY_PARALLEL_H_
