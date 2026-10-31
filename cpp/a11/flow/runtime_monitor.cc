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

#include "a11/flow/internal/runtime_monitor.h"

namespace a11::flow::internal {

uint64_t Monitor::CompletionOrder() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  if (completion_order_ != std::numeric_limits<uint64_t>::max()) {
    ++completion_order_;
  }
  return completion_order_;
}

absl::Status Monitor::Wait(Condition& condition,
                           absl::FunctionRef<bool()> ready)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  if (ready()) {
    return absl::OkStatus();
  }
  condition.waiters_.fetch_add(1, std::memory_order_relaxed);
  const Counted counted(&condition.waiters_);
  while (!ready()) {
    if (!stop_.ok()) {
      return stop_;
    }
    if (thread::Cancelled()) {
      return absl::CancelledError("Flow task cancelled");
    }
    condition.cv_.Wait(&mu_);
  }
  return absl::OkStatus();
}

absl::Status Monitor::WaitUntil(Condition& condition, absl::Time deadline,
                                absl::FunctionRef<bool()> ready)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  if (ready()) {
    return absl::OkStatus();
  }
  condition.waiters_.fetch_add(1, std::memory_order_relaxed);
  const Counted counted(&condition.waiters_);
  while (!ready()) {
    if (!stop_.ok()) {
      return stop_;
    }
    if (thread::Cancelled()) {
      return absl::CancelledError("Flow task cancelled");
    }
    if (condition.cv_.WaitWithDeadline(&mu_, deadline) && !ready()) {
      return absl::DeadlineExceededError("The wait timed out");
    }
  }
  return absl::OkStatus();
}

absl::Status Monitor::Wait(absl::FunctionRef<bool()> ready)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  if (ready()) {
    return absl::OkStatus();
  }
  const Parked parked(this);
  while (!ready()) {
    if (!stop_.ok()) {
      return stop_;
    }
    if (thread::Cancelled()) {
      return absl::CancelledError("Flow task cancelled");
    }
    cv_.Wait(&mu_);
  }
  return absl::OkStatus();
}

absl::Status Monitor::WaitUntil(absl::Time deadline,
                                absl::FunctionRef<bool()> ready)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  if (ready()) {
    return absl::OkStatus();
  }
  const Parked parked(this);
  while (!ready()) {
    if (!stop_.ok()) {
      return stop_;
    }
    if (thread::Cancelled()) {
      return absl::CancelledError("Flow task cancelled");
    }
    if (cv_.WaitWithDeadline(&mu_, deadline) && !ready()) {
      return absl::DeadlineExceededError("The wait timed out");
    }
  }
  return absl::OkStatus();
}

void Monitor::Stop(absl::Status why) {
  if (why.ok()) {
    why = absl::CancelledError("The flow stopped");
  }
  thread::MutexLock lock(&mu_);
  if (!stop_.ok()) {
    return;
  }
  stop_ = std::move(why);
  cv_.SignalAll();
  for (Condition* condition : conditions_) {
    condition->cv_.SignalAll();
  }
}

void Monitor::WakeAll() {
  thread::MutexLock lock(&mu_);
  cv_.SignalAll();
  for (Condition* condition : conditions_) {
    condition->cv_.SignalAll();
  }
}

void Monitor::WaitComplete(Condition& condition,
                           absl::FunctionRef<bool()> ready)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  if (ready()) {
    return;
  }
  condition.waiters_.fetch_add(1, std::memory_order_relaxed);
  const Counted counted(&condition.waiters_);
  while (!ready()) {
    condition.cv_.Wait(&mu_);
  }
}

std::optional<a11::Task> Group::Spawn(
    absl::AnyInvocable<absl::Status() &&> work,
    std::function<absl::Status(absl::Status)> complete) {
  size_t id;
  {
    thread::MutexLock lock(&monitor_->mu());
    if (stopping_) {
      return std::nullopt;
    }
    id = spawned_++;
  }
  a11::Task task = a11::SubmitTask(std::move(work), StackOptions());
  // Counted through the future rather than inside the work: a fiber cancelled
  // before it started never runs its body, and a group that counted there
  // would wait for a fiber that had already finished.
  {
    thread::MutexLock lock(&monitor_->mu());
    tasks_.emplace(id, task);
  }
  task.OnReady([this, id, complete = std::move(complete)](
                   const absl::StatusOr<a11::Unit>& result) mutable {
    const auto status = complete ? complete(result.status()) : result.status();
    complete = nullptr;
    Finish(id, status);
  });
  bool stopping;
  {
    thread::MutexLock lock(&monitor_->mu());
    stopping = stopping_;
  }
  if (stopping) {
    task.Cancel().IgnoreError();
    monitor_->WakeAll();
  }
  return task;
}

absl::Status Group::CheckActive(bool check_task) {
  thread::MutexLock lock(&monitor_->mu());
  const auto cause = Cause();
  if (!cause.ok()) {
    return cause;
  }
  if (stopping_ || (check_task && thread::Cancelled())) {
    return absl::CancelledError("Flow scope stopped");
  }
  return absl::OkStatus();
}

void Group::OwnAction(const std::shared_ptr<actions::Action>& action) {
  bool stopped;
  {
    thread::MutexLock lock(&monitor_->mu());
    stopped = stopping_ || !Cause().ok();
    if (!stopped) {
      actions_.insert_or_assign(action.get(), action);
    }
  }
  if (stopped) {
    action->Cancel().IgnoreError();
  }
}

absl::Status Group::Join() {
  if (joined_) {
    return first_;
  }
  joined_ = true;
  bool failed;
  {
    thread::MutexLock lock(&monitor_->mu());
    const auto waiting = monitor_->Wait(
        cv_, [this] { return done_ == spawned_ || !first_.ok(); });
    if (!waiting.ok() && first_.ok()) {
      const auto cause = Cause();
      first_ = cause.ok() ? waiting : cause;
    }
    failed = done_ < spawned_;
  }
  if (failed) {
    Give();
    thread::MutexLock lock(&monitor_->mu());
    monitor_->WaitComplete(cv_, [this] { return done_ == spawned_; });
  }
  return first_;
}

absl::Status Group::Cause() const
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(monitor_->mu()) {
  absl::Status cancellation;
  for (const Group* group = this; group != nullptr; group = group->parent_) {
    if (!group->first_.ok() && !absl::IsCancelled(group->first_)) {
      return group->first_;
    }
    if (!group->first_.ok()) {
      cancellation = group->first_;
    }
  }
  return monitor_->status().ok() ? cancellation : monitor_->status();
}

void Group::Give() {
  std::vector<a11::Task> tasks;
  std::vector<std::shared_ptr<actions::Action>> actions;
  {
    thread::MutexLock lock(&monitor_->mu());
    stopping_ = true;
    for (const auto& [identity, weak] : actions_) {
      if (auto action = weak.lock(); action != nullptr) {
        actions.push_back(std::move(action));
      }
    }
    for (const auto& [id, task] : tasks_) {
      tasks.push_back(task);
    }
  }
  for (const auto& action : actions) {
    action->Cancel().IgnoreError();
  }
  for (const a11::Task& task : tasks) {
    task.Cancel().IgnoreError();
  }
  monitor_->WakeAll();
}

void Group::Finish(size_t id, const absl::Status& status) {
  // Signal under the lock so Join cannot destroy the condition first.
  thread::MutexLock lock(&monitor_->mu());
  ++done_;
  tasks_.erase(id);
  // A cancellation is not a reason. Everything else in the group is cancelled
  // when one fiber fails, so the real failure has to win however the two race
  // -- otherwise a flow that timed out reports that it was cancelled.
  absl::Status outcome = status;
  if (absl::IsCancelled(outcome)) {
    const auto cause = Cause();
    if (!cause.ok()) {
      outcome = cause;
    }
  }
  if (!outcome.ok() &&
      (first_.ok() || (!stopping_ && absl::IsCancelled(first_) &&
                       !absl::IsCancelled(outcome)))) {
    first_ = outcome;
  }
  cv_.Wake();
}

}  // namespace a11::flow::internal
