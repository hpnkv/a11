/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef A11_CONCURRENCY_CALLBACK_SCHEDULER_H_
#define A11_CONCURRENCY_CALLBACK_SCHEDULER_H_

#include <cstddef>
#include <deque>

#include <absl/functional/any_invocable.h>

#include "thread/boost_primitives.h"

namespace a11::internal {

// A fair, stackless callback pump. Instances must have process lifetime because
// posted callbacks retain a raw pointer to the scheduler, while queued work
// owns the state it operates on.
class CallbackScheduler {
 public:
  static constexpr size_t kDefaultMaxCallbacksPerTurn = 64;
  static constexpr size_t kDefaultMaxConcurrentTurns = 2;

  /**
   * @param max_callbacks_per_turn
   *   How many callbacks one turn drains before handing the next turn back to
   *   the pool, so a long queue cannot monopolise a worker.
   * @param max_concurrent_turns
   *   How many turns may be in flight at once. This must be greater than one:
   *   see Run() for why a single turn is not enough to keep the queue draining.
   */
  explicit CallbackScheduler(
      size_t max_callbacks_per_turn = kDefaultMaxCallbacksPerTurn,
      size_t max_concurrent_turns = kDefaultMaxConcurrentTurns)
      : max_callbacks_per_turn_(max_callbacks_per_turn),
        max_concurrent_turns_(max_concurrent_turns < 2 ? 2
                                                       : max_concurrent_turns) {
  }

  CallbackScheduler(const CallbackScheduler&) = delete;
  CallbackScheduler& operator=(const CallbackScheduler&) = delete;

  void Schedule(absl::AnyInvocable<void() &&> callback);

 private:
  void Run();

  const size_t max_callbacks_per_turn_;
  const size_t max_concurrent_turns_;
  thread::Mutex mu_;
  std::deque<absl::AnyInvocable<void() &&>> callbacks_ ABSL_GUARDED_BY(mu_);
  /// Turns in flight, including suspended turns that are not draining.
  size_t active_turns_ ABSL_GUARDED_BY(mu_) = 0;
};

}  // namespace a11::internal

#endif  // A11_CONCURRENCY_CALLBACK_SCHEDULER_H_
