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

#ifndef A11_FLOW_INTERNAL_CALL_HANDLE_H_
#define A11_FLOW_INTERNAL_CALL_HANDLE_H_

#include "a11/flow/internal/runtime_monitor.h"

namespace a11::flow::internal {

/// One instance of a `call` step: its action, its ports, its completion.
class CallHandle {
 public:
  explicit CallHandle(Monitor& monitor) : monitor_(&monitor) {}

  void SetDeadline(absl::Time deadline) {
    thread::MutexLock lock(&monitor_->mu());
    deadline_ = deadline;
  }

  absl::Time deadline() {
    thread::MutexLock lock(&monitor_->mu());
    return deadline_;
  }

  void Adopt(std::shared_ptr<actions::Action> action);

  void Started(std::shared_ptr<actions::Action> action);

  /// Say why the call never started, so nothing waits on it for ever.
  void NeverStarted(absl::Status why);

  void Done();

  /// Whether the call has finished. Requires the run's lock.
  ///
  /// The caller holds one monitor lock while checking several handles, which
  /// the thread-safety analysis cannot express through their pointers.
  [[nodiscard]] bool finished() const ABSL_NO_THREAD_SAFETY_ANALYSIS {
    return done_;
  }

  uint64_t completion_order() const ABSL_NO_THREAD_SAFETY_ANALYSIS {
    return completion_order_;
  }

  void SetError(absl::Status error);

  absl::Status error() {
    thread::MutexLock lock(&monitor_->mu());
    return error_;
  }

  absl::StatusOr<std::shared_ptr<actions::Action>> Action();

  std::shared_ptr<actions::Action> action_now() {
    thread::MutexLock lock(&monitor_->mu());
    return action_;
  }

  /// The node of one of the call's ports, once the call has started.
  absl::StatusOr<NodePtr> Node(const std::string& name,
                               syntax::PortDirection direction);

  /// How the call went, once it has gone.
  ///
  /// Two statuses, which is why this is not a `StatusOr`: the returned one
  /// says whether the *waiting* worked, and `outcome` is what the call
  /// finished with.
  absl::Status Outcome(absl::Status* absl_nonnull outcome);

  /// Ports nobody writes and outputs nobody reads: closed and drained for the
  /// callee once it has started.
  std::vector<NodePtr> unclosed;
  std::vector<NodePtr> undrained;

 private:
  Monitor* absl_nonnull monitor_;
  std::shared_ptr<actions::Action> action_;
  absl::Time deadline_ ABSL_GUARDED_BY(monitor_->mu()) = absl::InfiniteFuture();
  bool started_ = false;
  bool done_ = false;
  uint64_t completion_order_ = 0;
  absl::Status start_error_;
  absl::Status error_;
};

}  // namespace a11::flow::internal
#endif  // A11_FLOW_INTERNAL_CALL_HANDLE_H_
