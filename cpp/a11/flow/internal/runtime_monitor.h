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

#ifndef A11_FLOW_INTERNAL_RUNTIME_MONITOR_H_
#define A11_FLOW_INTERNAL_RUNTIME_MONITOR_H_

#include "a11/flow/internal/runtime_common.h"

namespace a11::flow::internal {

/// Coordinates every wait in one run and wakes them together on cancellation.
///
/// Blocking node, action, and host operations run with the monitor lock
/// released.
class Monitor {
 public:
  thread::Mutex& mu() ABSL_LOCK_RETURNED(mu_) { return mu_; }

  absl::Status status() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    return stop_;
  }

  uint64_t CompletionOrder() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Wake every parked waiter after shared state changes. Waiters increment the
  // count under the monitor lock before testing their predicate.
  /// Wake every parked waiter after shared state changes.
  ///
  /// Waiters increment the count under the monitor lock before testing their
  /// predicate. The mutex provides ordering; the count only avoids empty
  /// broadcasts and therefore uses relaxed atomic access.
  void Wake() {
    if (waiters_.load(std::memory_order_relaxed) > 0) {
      cv_.SignalAll();
    }
  }

  /// A per-object condition registered for cancellation by the monitor.
  ///
  /// Use one after every mutation of its predicate has been wired to wake it.
  /// Stop still wakes all registered conditions.
  class Condition {
   public:
    explicit Condition(Monitor& monitor) : monitor_(&monitor) {
      thread::MutexLock lock(&monitor_->mu_);
      monitor_->conditions_.push_back(this);
    }

    ~Condition() {
      thread::MutexLock lock(&monitor_->mu_);
      std::erase(monitor_->conditions_, this);
    }

    Condition(const Condition&) = delete;
    Condition& operator=(const Condition&) = delete;

    /// Wake the fibers parked on this, and nothing else.
    void Wake() {
      if (waiters_.load(std::memory_order_relaxed) > 0) {
        cv_.SignalAll();
      }
    }

   private:
    friend class Monitor;

    Monitor* absl_nonnull monitor_;
    thread::CondVar cv_;
    std::atomic<int> waiters_ = 0;
  };

  // Wait on one object's condition until `ready` holds, or the run is given up.
  // REQUIRES: the lock is held.
  /// Wait on one object's condition until `ready` holds, or the run is given
  /// up.
  ///
  /// REQUIRES: the lock is held.
  absl::Status Wait(Condition& condition, absl::FunctionRef<bool()> ready)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  /// The same, up to a deadline: `DeadlineExceeded` when it passes first.
  absl::Status WaitUntil(Condition& condition, absl::Time deadline,
                         absl::FunctionRef<bool()> ready)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  /// Wait until `ready` holds, or the run is given up on.
  ///
  /// REQUIRES: the lock is held.
  absl::Status Wait(absl::FunctionRef<bool()> ready)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  /// The same, up to a deadline: `DeadlineExceeded` when it passes first.
  absl::Status WaitUntil(absl::Time deadline, absl::FunctionRef<bool()> ready)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  /// Give up on the run, waking everything waiting on anything.
  ///
  /// The shared monitor wakes pumps, barriers, and steps together so a failed
  /// flow cannot leave one waiting on state that will never change.
  void Stop(absl::Status why);

  void WakeAll();

  void WaitComplete(Condition& condition, absl::FunctionRef<bool()> ready)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

 private:
  /// Decrements a waiter count on the way out, however the wait ends.
  class Counted {
   public:
    explicit Counted(std::atomic<int>* absl_nonnull waiters)
        : waiters_(waiters) {}

    ~Counted() { waiters_->fetch_sub(1, std::memory_order_relaxed); }

    Counted(const Counted&) = delete;
    Counted& operator=(const Counted&) = delete;

   private:
    std::atomic<int>* absl_nonnull waiters_;
  };

  /// Counts one fiber in [Wait] for as long as it might park.
  class Parked {
   public:
    explicit Parked(Monitor* absl_nonnull monitor) : monitor_(monitor) {
      monitor_->waiters_.fetch_add(1, std::memory_order_relaxed);
    }

    ~Parked() { monitor_->waiters_.fetch_sub(1, std::memory_order_relaxed); }

    Parked(const Parked&) = delete;
    Parked& operator=(const Parked&) = delete;

   private:
    Monitor* absl_nonnull monitor_;
  };

  thread::Mutex mu_;
  uint64_t completion_order_ ABSL_GUARDED_BY(mu_) = 0;
  thread::CondVar cv_;
  absl::Status stop_ ABSL_GUARDED_BY(mu_);
  /// Read without the lock by [Wake]; see the note there.
  std::atomic<int> waiters_ = 0;
  /// Every per-object condition, so that [Stop] wakes them too.
  std::vector<Condition*> conditions_ ABSL_GUARDED_BY(mu_);
};

/// A set of concurrent fibers with one outcome.
///
/// The first failure is the group's, and everything else is cancelled rather
/// than waited out: a step that failed leaves pumps and barriers waiting for
/// things that are not coming.
class Group {
 public:
  explicit Group(Monitor& monitor, Group* absl_nullable parent = nullptr)
      : monitor_(&monitor), cv_(monitor), parent_(parent) {}

  Group(const Group&) = delete;
  Group& operator=(const Group&) = delete;

  /// Nothing is left running: a group that was abandoned rather than joined --
  /// because the code around it failed -- cancels its fibers and waits for
  /// them.
  ~Group() {
    if (!joined_) {
      Give();
    }
    Join().IgnoreError();
  }

  std::optional<a11::Task> Spawn(
      absl::AnyInvocable<absl::Status() &&> work,
      std::function<absl::Status(absl::Status)> complete = {});

  absl::Status CheckActive(bool check_task = true);

  void OwnAction(const std::shared_ptr<actions::Action>& action);

  /// Wait for every fiber, giving up on the run as soon as one fails.
  absl::Status Join();

 private:
  absl::Status Cause() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(monitor_->mu());

  /// Stop waiting for anything: the run is over, one way or another.
  ///
  /// Cancel owned actions before interrupting their Flow tasks. The shared
  /// monitor wakes conditions while the cause remains local to this subtree.
  void Give();

  void Finish(size_t id, const absl::Status& status);

  Monitor* absl_nonnull monitor_;
  Monitor::Condition cv_;
  Group* absl_nullable parent_;
  absl::flat_hash_map<size_t, a11::Task> tasks_ ABSL_GUARDED_BY(monitor_->mu());
  size_t spawned_ = 0;
  size_t done_ = 0;
  absl::Status first_;
  bool joined_ = false;
  bool stopping_ ABSL_GUARDED_BY(monitor_->mu()) = false;
  absl::flat_hash_map<actions::Action*, std::weak_ptr<actions::Action>> actions_
      ABSL_GUARDED_BY(monitor_->mu());
};
}  // namespace a11::flow::internal
#endif  // A11_FLOW_INTERNAL_RUNTIME_MONITOR_H_
