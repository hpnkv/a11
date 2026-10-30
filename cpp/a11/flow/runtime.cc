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

#include "a11/flow/runtime.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/base/thread_annotations.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/functional/any_invocable.h>
#include <absl/functional/function_ref.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/strings/str_split.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <absl/types/span.h>
#include <nlohmann/json.hpp>

#include "a11/actions/registry.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/concurrency/parallel.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/flow/contract.h"
#include "a11/flow/schema.h"
#include "a11/flow/vocabulary.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/stores/chunk_store_writer.h"
#include "a11/utf8.h"
#include "thread/boost_primitives.h"

namespace a11::flow {
struct ExecutionSnapshot {
  absl::flat_hash_map<std::string,
                      std::pair<actions::ActionSchema, actions::ActionHandler>>
      external;
  bool sealed = false;
};

namespace {

using graph::BodyId;
using graph::ExprId;
using graph::FlowGraph;
using graph::kNone;
using graph::RefId;
using graph::RefKind;
using graph::StepId;
using graph::StepKind;

/// Stack size for Flow fibers that may enter a host interpreter through
/// [HostBridge].
constexpr size_t kStackSize = 256 * 1024;

thread::TreeOptions StackOptions() {
  return thread::TreeOptions{.stack_size = kStackSize};
}

absl::Status Fail(std::string_view message,
                  absl::StatusCode code = absl::StatusCode::kInvalidArgument) {
  return {code, message};
}

/// What a barrier is a barrier on, out of its label: `wait x` -> `x`.
std::string SubjectOf(const graph::Step& step) {
  const size_t space = step.label.find(' ');
  if (space == std::string::npos) {
    return step.label;
  }
  return step.label.substr(space + 1);
}

// --- Items -------------------------------------------------------------------

/// One value travelling through a pipe.
///
/// An item read from a node keeps its native [data::Chunk], so a pipe that only
/// moves values re-writes exactly the producer's bytes and mimetype and never
/// pays for a round trip through the host. A stage that looks at the value
/// decodes it once, and every reader of the same item shares that.
class Item {
 public:
  static std::shared_ptr<Item> OfChunk(data::Chunk chunk) {
    auto item = std::make_shared<Item>();
    item->chunk_ = std::move(chunk);
    return item;
  }

  static std::shared_ptr<Item> Of(Value value) {
    auto item = std::make_shared<Item>();
    item->result_ = std::move(value);
    item->decoded_ = true;
    return item;
  }

  const std::optional<data::Chunk>& chunk() const { return chunk_; }

  std::string Mimetype() const {
    if (!chunk_.has_value()) {
      return std::string(data::kJsonMimetype);
    }
    return chunk_->GetMimetype();
  }

  /// The decoded value, read out of the chunk the first time it is asked for.
  absl::StatusOr<Value> Read(HostBridge* absl_nonnull bridge) const {
    thread::MutexLock lock(&mu_);
    if (!decoded_) {
      if (!chunk_.has_value()) {
        result_ = Value::Null();
      } else {
        result_ = bridge->FromChunk(*chunk_);
      }
      decoded_ = true;
    }
    return result_;
  }

  /// Whether this item still has to be decoded to be read.
  bool NeedsDecoding() const {
    thread::MutexLock lock(&mu_);
    return !decoded_ && chunk_.has_value();
  }

  /// Give the item the outcome of a decode somebody else did.
  ///
  /// What lets a stage decode a whole batch in one crossing into the host and
  /// still fail exactly where the one-at-a-time path failed: the outcome is
  /// stored per item, including a bad one, and surfaces when [Read] reaches it.
  void Prime(absl::StatusOr<Value> result) const {
    thread::MutexLock lock(&mu_);
    if (decoded_) {
      return;
    }
    result_ = std::move(result);
    decoded_ = true;
  }

 private:
  std::optional<data::Chunk> chunk_;
  mutable thread::Mutex mu_;
  mutable absl::StatusOr<Value> result_;
  mutable bool decoded_ = false;
};

using ItemPtr = std::shared_ptr<Item>;

/// `left / right`, for the one place the language divides: `| avg`.
///
/// Not an operator, because a flow cannot write one -- the language has `+` and
/// `-` and nothing else -- so this is a mean and not the beginning of
/// arithmetic. A duration divided by a count is a duration, which is what makes
/// `| avg it.elapsed` the useful form.
absl::StatusOr<Value> Divide(const Value& total, const Value& count) {
  const double by = AsDouble(count);
  if (by == 0.0) {
    return Value::Null();
  }
  if (total.kind() == Value::Kind::kDuration) {
    return Value::Duration(total.duration() / by);
  }
  return Value::Double(AsDouble(total) / by);
}

/// Decode every item of a batch that still needs it, in one crossing.
///
/// A no-op when fewer than two values need decoding. Batching reduces host
/// crossings while preserving the direct path for pipes that only move values.
void PrimeBatch(const std::vector<ItemPtr>& items,
                HostBridge* absl_nonnull bridge) {
  std::vector<const data::Chunk*> chunks;
  std::vector<const Item*> owners;
  for (const ItemPtr& item : items) {
    if (!item->NeedsDecoding()) {
      continue;
    }
    chunks.push_back(&*item->chunk());
    owners.push_back(item.get());
  }
  if (chunks.size() < 2) {
    return;
  }
  std::vector<absl::StatusOr<Value>> values = bridge->FromChunks(chunks);
  if (values.size() != owners.size()) {
    return;
  }
  for (size_t index = 0; index < owners.size(); ++index) {
    owners[index]->Prime(std::move(values[index]));
  }
}

/// A mimetype without its parameters: `application/x-msgpack;type=X`.
std::string BaseMimetype(std::string_view mimetype) {
  const size_t at = mimetype.find(';');
  std::string base(at == std::string_view::npos ? mimetype
                                                : mimetype.substr(0, at));
  absl::AsciiStrToLower(&base);
  return std::string(absl::StripAsciiWhitespace(base));
}

/// Whether `name` matches a pattern whose `*` spans any characters.
bool Matches(std::string_view name, std::string_view pattern) {
  const std::vector<std::string_view> parts = absl::StrSplit(pattern, '*');
  if (parts.size() == 1) {
    return name == pattern;
  }
  if (!absl::StartsWith(name, parts.front())) {
    return false;
  }
  if (!absl::EndsWith(name, parts.back())) {
    return false;
  }
  size_t at = parts.front().size();
  const size_t limit = name.size() - parts.back().size();
  if (at > limit) {
    return false;
  }
  for (size_t index = 1; index + 1 < parts.size(); ++index) {
    if (parts[index].empty()) {
      continue;
    }
    const size_t found = name.find(parts[index], at);
    if (found == std::string_view::npos ||
        found + parts[index].size() > limit) {
      return false;
    }
    at = found + parts[index].size();
  }
  return true;
}

bool MatchesFolded(std::string_view name, std::string_view pattern) {
  return Matches(absl::AsciiStrToLower(name), absl::AsciiStrToLower(pattern));
}

// --- The monitor -------------------------------------------------------------

// Coordinates every wait in one run and wakes them together on cancellation.
// Blocking node, action, and host operations run with the monitor lock
// released.
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

  uint64_t CompletionOrder() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    if (completion_order_ != std::numeric_limits<uint64_t>::max()) {
      ++completion_order_;
    }
    return completion_order_;
  }

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
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
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

  /// The same, up to a deadline: `DeadlineExceeded` when it passes first.
  absl::Status WaitUntil(Condition& condition, absl::Time deadline,
                         absl::FunctionRef<bool()> ready)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
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

  /// Wait until `ready` holds, or the run is given up on.
  ///
  /// REQUIRES: the lock is held.
  absl::Status Wait(absl::FunctionRef<bool()> ready)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
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

  /// The same, up to a deadline: `DeadlineExceeded` when it passes first.
  absl::Status WaitUntil(absl::Time deadline, absl::FunctionRef<bool()> ready)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
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

  /// Give up on the run, waking everything waiting on anything.
  ///
  /// The shared monitor wakes pumps, barriers, and steps together so a failed
  /// flow cannot leave one waiting on state that will never change.
  void Stop(absl::Status why) {
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

  void WakeAll() {
    thread::MutexLock lock(&mu_);
    cv_.SignalAll();
    for (Condition* condition : conditions_) {
      condition->cv_.SignalAll();
    }
  }

  void WaitComplete(Condition& condition, absl::FunctionRef<bool()> ready)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    condition.waiters_.fetch_add(1, std::memory_order_relaxed);
    const Counted counted(&condition.waiters_);
    while (!ready()) {
      condition.cv_.Wait(&mu_);
    }
  }

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

  void Spawn(absl::AnyInvocable<absl::Status() &&> work) {
    size_t id;
    {
      thread::MutexLock lock(&monitor_->mu());
      if (stopping_) {
        return;
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
    task.OnReady([this, id](const absl::StatusOr<a11::Unit>& result) {
      Finish(id, result.status());
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
  }

  absl::Status CheckActive() {
    thread::MutexLock lock(&monitor_->mu());
    const auto cause = Cause();
    if (!cause.ok()) {
      return cause;
    }
    if (stopping_ || thread::Cancelled()) {
      return absl::CancelledError("Flow scope stopped");
    }
    return absl::OkStatus();
  }

  void OwnAction(const std::shared_ptr<actions::Action>& action) {
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

  /// Wait for every fiber, giving up on the run as soon as one fails.
  absl::Status Join() {
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

 private:
  absl::Status Cause() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(monitor_->mu()) {
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

  /// Stop waiting for anything: the run is over, one way or another.
  ///
  /// Cancel owned actions before interrupting their Flow tasks. The shared
  /// monitor wakes conditions while the cause remains local to this subtree.
  void Give() {
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

  void Finish(size_t id, const absl::Status& status) {
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

// --- Readers -----------------------------------------------------------------

/// One reader's view of a stream.
class Reader {
 public:
  virtual ~Reader() = default;

  /// The next item, or a null pointer at the end of the stream.
  ///
  /// A non-ok status is the producer's failure, relayed to every reader.
  virtual absl::StatusOr<ItemPtr> Next() = 0;

  // Append up to `limit` currently available items, waiting only when none is
  // ready.
  /// Append up to `limit` currently available items, waiting only when none is
  /// ready.
  ///
  /// Appending nothing means the stream has ended, as a null item does for
  /// [Next].
  virtual absl::Status NextMany(std::vector<ItemPtr>& out, size_t limit) {
    if (limit == 0) {
      return absl::OkStatus();
    }
    ABSL_ASSIGN_OR_RETURN(ItemPtr item, Next());
    if (item != nullptr) {
      out.push_back(std::move(item));
    }
    return absl::OkStatus();
  }

  /// The next item, or `DeadlineExceeded` when none arrives by `deadline`.
  ///
  /// Only `| timeout` asks for this, and only a reader that *waits* can answer
  /// it meaningfully: a list or a buffer has its values already and cannot be
  /// late with one, so the default answers with the value rather than
  /// pretending to have waited.
  virtual absl::StatusOr<ItemPtr> NextUntil(absl::Time deadline) {
    (void)deadline;
    return Next();
  }

  /// Stop reading.
  ///
  /// The producer is not cancelled -- it keeps going into a discarded buffer. A
  /// node on the other end of it may be feeding an action that would stall if
  /// nobody drained it, and a `first 3` must not be able to wedge the step it
  /// reads from.
  virtual void Stop() {}
};

using ReaderPtr = std::unique_ptr<Reader>;

/// A reader over a fixed list of items: a preset, a buffer, a lazy value.
class ListReader : public Reader {
 public:
  explicit ListReader(std::vector<ItemPtr> items) : items_(std::move(items)) {}

  absl::StatusOr<ItemPtr> Next() override {
    if (at_ >= items_.size()) {
      return ItemPtr{};
    }
    return items_[at_++];
  }

  absl::Status NextMany(std::vector<ItemPtr>& out, size_t limit) override {
    while (at_ < items_.size() && limit-- > 0) {
      out.push_back(items_[at_++]);
    }
    return absl::OkStatus();
  }

  void Stop() override { at_ = items_.size(); }

 private:
  std::vector<ItemPtr> items_;
  size_t at_ = 0;
};

/// A reader that fails, for a stream whose producer failed before anyone read.
class FailedReader : public Reader {
 public:
  explicit FailedReader(absl::Status status) : status_(std::move(status)) {}

  absl::StatusOr<ItemPtr> Next() override { return status_; }

 private:
  absl::Status status_;
};

/// What a bus hands each of its readers.
struct Slot {
  std::deque<ItemPtr> items;
  bool ended = false;
  absl::Status error;
  bool dropped = false;
};

/// Publishes one item or an existing batch to a stream consumer.
///
/// Sinks without a batch path apply the single-item callback to each value.
class Sink {
 public:
  using OneFn = absl::FunctionRef<absl::Status(ItemPtr)>;
  using ManyFn = absl::FunctionRef<absl::Status(std::vector<ItemPtr>&)>;

  explicit Sink(OneFn one) : one_(one) {}

  Sink(OneFn one, ManyFn many) : one_(one), many_(many) {}

  absl::Status One(ItemPtr item) const { return one_(std::move(item)); }

  /// Publish everything in `items`, which is left empty.
  absl::Status Many(std::vector<ItemPtr>& items) const {
    if (many_.has_value()) {
      return (*many_)(items);
    }
    for (ItemPtr& item : items) {
      ABSL_RETURN_IF_ERROR(one_(std::move(item)));
    }
    items.clear();
    return absl::OkStatus();
  }

 private:
  OneFn one_;
  std::optional<ManyFn> many_;
};

/// A stream with a fixed number of readers, fed by one producer.
class Bus {
 public:
  using Produce = std::function<absl::Status(const Sink&)>;

  Bus(Monitor& monitor, std::string label, Produce produce, int readers)
      : monitor_(&monitor),
        moved_(monitor),
        label_(std::move(label)),
        produce_(std::move(produce)) {
    slots_.reserve(static_cast<size_t>(std::max(readers, 0)));
    for (int index = 0; index < readers; ++index) {
      slots_.push_back(std::make_unique<Slot>());
    }
  }

  const std::string& label() const { return label_; }

  /// One reader's view. Fails when the plan accounted for fewer readers.
  absl::StatusOr<ReaderPtr> Take();

  /// Read the stream once and fan it out until it ends.
  absl::Status Pump();

  bool Skip() {
    thread::MutexLock lock(&monitor_->mu());
    if (handed_out_ < slots_.size()) {
      slots_[handed_out_++]->dropped = true;
    }
    bool all = true;
    for (const auto& slot : slots_) {
      all = all && slot->dropped;
    }
    moved_.Wake();
    return all && !wanted_;
  }

  void Wanted() {
    bool woken = false;
    {
      thread::MutexLock lock(&monitor_->mu());
      woken = !wanted_;
      wanted_ = true;
    }
    // Once per stream rather than once per read: after the first, the pump is
    // producing and there is nothing waiting to be told again.
    if (woken) {
      moved_.Wake();
    }
  }

  Monitor& monitor() { return *monitor_; }

  /// Everything parked on this stream: its readers, and its producer waiting
  /// for room. One condition for both, because both wait on the same slots.
  Monitor::Condition& moved() { return moved_; }

 private:
  friend class BusReader;

  Monitor* absl_nonnull monitor_;
  Monitor::Condition moved_;
  std::string label_;
  Produce produce_;
  std::vector<std::unique_ptr<Slot>> slots_;
  size_t handed_out_ = 0;
  bool wanted_ = false;
};

class BusReader : public Reader {
 public:
  BusReader(Bus& bus, Slot& slot) : bus_(&bus), slot_(&slot) {}

  ~BusReader() override { Stop(); }

  absl::StatusOr<ItemPtr> Next() override {
    bus_->Wanted();
    Monitor& monitor = bus_->monitor();
    thread::MutexLock lock(&monitor.mu());
    ABSL_RETURN_IF_ERROR(monitor.Wait(bus_->moved(), [this] {
      return !slot_->items.empty() || slot_->ended;
    }));
    if (!slot_->items.empty()) {
      ItemPtr item = std::move(slot_->items.front());
      slot_->items.pop_front();
      bus_->moved().Wake();
      return item;
    }
    ABSL_RETURN_IF_ERROR(slot_->error);
    return ItemPtr{};
  }

  absl::StatusOr<ItemPtr> NextUntil(absl::Time deadline) override {
    bus_->Wanted();
    Monitor& monitor = bus_->monitor();
    thread::MutexLock lock(&monitor.mu());
    ABSL_RETURN_IF_ERROR(monitor.WaitUntil(bus_->moved(), deadline, [this] {
      return !slot_->items.empty() || slot_->ended;
    }));
    if (!slot_->items.empty()) {
      ItemPtr item = std::move(slot_->items.front());
      slot_->items.pop_front();
      bus_->moved().Wake();
      return item;
    }
    ABSL_RETURN_IF_ERROR(slot_->error);
    return ItemPtr{};
  }

  absl::Status NextMany(std::vector<ItemPtr>& out, size_t limit) override {
    if (limit == 0) {
      return absl::OkStatus();
    }
    bus_->Wanted();
    Monitor& monitor = bus_->monitor();
    bool room = false;
    {
      thread::MutexLock lock(&monitor.mu());
      ABSL_RETURN_IF_ERROR(monitor.Wait(bus_->moved(), [this] {
        return !slot_->items.empty() || slot_->ended;
      }));
      while (!slot_->items.empty() && limit-- > 0) {
        out.push_back(std::move(slot_->items.front()));
        slot_->items.pop_front();
        room = true;
      }
      if (!room) {
        ABSL_RETURN_IF_ERROR(slot_->error);
      }
    }
    // One wake is sufficient because the producer only tests whether room
    // exists.
    if (room) {
      bus_->moved().Wake();
    }
    return absl::OkStatus();
  }

  void Stop() override {
    Monitor& monitor = bus_->monitor();
    {
      thread::MutexLock lock(&monitor.mu());
      if (slot_->dropped) {
        return;
      }
      slot_->dropped = true;
      slot_->items.clear();
    }
    // The producer may be waiting for room this reader will never make.
    bus_->Wanted();
    bus_->moved().Wake();
  }

 private:
  Bus* absl_nonnull bus_;
  Slot* absl_nonnull slot_;
};

absl::StatusOr<ReaderPtr> Bus::Take() {
  thread::MutexLock lock(&monitor_->mu());
  if (handed_out_ >= slots_.size()) {
    return Fail(absl::StrCat("Internal flow error: ", label_,
                             " has more readers than the plan accounted for."));
  }
  return ReaderPtr(new BusReader(*this, *slots_[handed_out_++]));
}

absl::Status Bus::Pump() {
  // Start production on the first read so effects respect `after` ordering.
  {
    thread::MutexLock lock(&monitor_->mu());
    ABSL_RETURN_IF_ERROR(monitor_->Wait(moved_, [this] {
      if (wanted_) {
        return true;
      }
      for (const auto& slot : slots_) {
        if (!slot->dropped) {
          return false;
        }
      }
      return true;
    }));
    if (!wanted_) {
      return absl::OkStatus();
    }
  }
  absl::Status error;
  // Batches may exceed the queue depth once admitted; the next batch waits
  // until every live reader has room.
  const auto room = [this] {
    for (const std::unique_ptr<Slot>& slot : slots_) {
      if (!slot->dropped && slot->items.size() >= kQueueDepth) {
        return false;
      }
    }
    return true;
  };
  const auto one = [this, &room](const ItemPtr& item) -> absl::Status {
    {
      thread::MutexLock lock(&monitor_->mu());
      ABSL_RETURN_IF_ERROR(monitor_->Wait(moved_, room));
      for (const std::unique_ptr<Slot>& slot : slots_) {
        if (!slot->dropped) {
          slot->items.push_back(item);
        }
      }
    }
    moved_.Wake();
    return absl::OkStatus();
  };
  const auto many = [this, &room](std::vector<ItemPtr>& items) -> absl::Status {
    if (items.empty()) {
      return absl::OkStatus();
    }
    {
      thread::MutexLock lock(&monitor_->mu());
      ABSL_RETURN_IF_ERROR(monitor_->Wait(moved_, room));
      for (const std::unique_ptr<Slot>& slot : slots_) {
        if (slot->dropped) {
          continue;
        }
        slot->items.insert(slot->items.end(), items.begin(), items.end());
      }
    }
    items.clear();
    moved_.Wake();
    return absl::OkStatus();
  };
  error = produce_(Sink(one, many));
  {
    thread::MutexLock lock(&monitor_->mu());
    for (const std::unique_ptr<Slot>& slot : slots_) {
      slot->ended = true;
      slot->error = error;
    }
  }
  moved_.Wake();
  return absl::OkStatus();
}

/// A single value computed the first time it is asked for, then shared.
///
/// What a status is: reading one waits for its subject and may end a node, so
/// it happens when a step asks, and once however many steps ask.
class Lazy {
 public:
  using Produce = std::function<absl::StatusOr<ItemPtr>()>;

  Lazy(Monitor& monitor, Produce produce)
      : monitor_(&monitor), produce_(std::move(produce)) {}

  absl::StatusOr<ReaderPtr> Replay() {
    ABSL_ASSIGN_OR_RETURN(const ItemPtr item, Get());
    std::vector<ItemPtr> items;
    if (item != nullptr) {
      items.push_back(item);
    }
    return ReaderPtr(new ListReader(std::move(items)));
  }

 private:
  absl::StatusOr<ItemPtr> Get() {
    {
      thread::MutexLock lock(&monitor_->mu());
      if (running_) {
        ABSL_RETURN_IF_ERROR(monitor_->Wait([this] { return ready_; }));
        return result_;
      }
      running_ = true;
    }
    absl::StatusOr<ItemPtr> result = produce_();
    {
      thread::MutexLock lock(&monitor_->mu());
      result_ = result;
      ready_ = true;
    }
    monitor_->Wake();
    return result;
  }

  Monitor* absl_nonnull monitor_;
  Produce produce_;
  bool running_ = false;
  bool ready_ = false;
  absl::StatusOr<ItemPtr> result_;
};

// Incrementally buffers a stream and replays it to each nested reader. The
// buffer retains the stream for the body's lifetime.
/// Incrementally buffers a stream and replays it to each nested reader.
///
/// Loops and branches use this for outer refs so every pass sees the same
/// values as they arrive. The buffer retains the stream for the body's
/// lifetime.
class Buffer {
 public:
  /// Shared storage retained by readers that outlive the creating scope.
  class State {
   public:
    explicit State(Monitor& monitor) : monitor_(&monitor), grew_(monitor) {}

    /// The item at `index`, once it is there; a null item at the end.
    ///
    /// Return buffered items before reporting the source's terminal failure.
    absl::StatusOr<ItemPtr> At(size_t index) {
      thread::MutexLock lock(&monitor_->mu());
      ABSL_RETURN_IF_ERROR(monitor_->Wait(
          grew_, [this, index] { return index < items_.size() || ended_; }));
      if (index < items_.size()) {
        return items_[index];
      }
      ABSL_RETURN_IF_ERROR(error_);
      return ItemPtr{};
    }

    void Add(ItemPtr item) {
      {
        thread::MutexLock lock(&monitor_->mu());
        items_.push_back(std::move(item));
      }
      // Per item, because a reader waiting for this one is waiting now.
      grew_.Wake();
    }

    /// The same for a batch the source already had: one lock, one wake.
    void AddMany(std::vector<ItemPtr>& items) {
      if (items.empty()) {
        return;
      }
      {
        thread::MutexLock lock(&monitor_->mu());
        items_.insert(items_.end(), std::make_move_iterator(items.begin()),
                      std::make_move_iterator(items.end()));
      }
      items.clear();
      grew_.Wake();
    }

    void End(absl::Status error) {
      {
        thread::MutexLock lock(&monitor_->mu());
        error_ = std::move(error);
        ended_ = true;
      }
      grew_.Wake();
    }

   private:
    Monitor* absl_nonnull monitor_;
    /// Everything reading this buffer: a pass of a loop waiting for the value
    /// it is up to. Its own condition, so filling it does not wake the run.
    Monitor::Condition grew_;
    /// Whether the source has finished, one way or the other.
    bool ended_ = false;
    absl::Status error_;
    std::vector<ItemPtr> items_;
  };

  Buffer(Monitor& monitor, ReaderPtr source)
      : state_(std::make_shared<State>(monitor)), source_(std::move(source)) {}

  /// Read the source, publishing each item as it arrives.
  absl::Status Fill() {
    absl::Status status;
    std::vector<ItemPtr> batch;
    batch.reserve(kQueueDepth);
    while (true) {
      batch.clear();
      status = source_->NextMany(batch, kQueueDepth);
      if (!status.ok()) {
        break;
      }
      if (batch.empty()) {
        break;
      }
      state_->AddMany(batch);
    }
    state_->End(std::move(status));
    // Relayed to the readers of the buffer, not raised here: see [Bus::Pump].
    return absl::OkStatus();
  }

  /// One reader's own cursor over the buffer.
  absl::StatusOr<ReaderPtr> Replay();

 private:
  std::shared_ptr<State> state_;
  ReaderPtr source_;
};

/// Shared cursor for expressions that consume values from a stream.
///
/// General streams advance once per value read. Unary streams cache their one
/// value for every reader and report an error if another value arrives.
class ValueCursor {
 public:
  ValueCursor(HostBridge& bridge, ReaderPtr reader, bool unary,
              std::string label)
      : bridge_(&bridge),
        reader_(std::move(reader)),
        unary_(unary),
        label_(std::move(label)) {}

  void Stop() { reader_->Stop(); }

  // Return the next value, or the cached value for a unary stream. Readers
  // reserve a turn under the monitor lock and perform the blocking read after
  // releasing it.
  /// Return the next value, or the cached value for a unary stream.
  ///
  /// Readers reserve a turn under the monitor lock and perform the blocking
  /// read after releasing it.
  absl::StatusOr<Value> Next(Monitor& monitor) {
    {
      thread::MutexLock lock(&monitor.mu());
      ABSL_RETURN_IF_ERROR(monitor.Wait([this] { return !busy_; }));
      if (unary_ && read_) {
        return held_;
      }
      busy_ = true;
    }
    absl::StatusOr<Value> value = Read();
    {
      thread::MutexLock lock(&monitor.mu());
      busy_ = false;
      if (unary_ && value.ok()) {
        held_ = *value;
        read_ = true;
      }
    }
    monitor.Wake();
    return value;
  }

 private:
  /// One value off the stream, and for a unary one the promise checked.
  absl::StatusOr<Value> Read() {
    ABSL_ASSIGN_OR_RETURN(const ItemPtr item, reader_->Next());
    Value value;
    if (item != nullptr) {
      ABSL_ASSIGN_OR_RETURN(value, item->Read(bridge_));
    }
    if (!unary_ || item == nullptr) {
      return value;
    }
    // Confirm that a stream declared unary ends after its first value.
    ABSL_ASSIGN_OR_RETURN(const ItemPtr extra, reader_->Next());
    if (extra != nullptr) {
      return absl::InvalidArgumentError(absl::StrCat(
          label_,
          " carries one value, and a second arrived. Say 'stream' where it may "
          "carry several, or reduce it with '| collect', '| count' or "
          "'| first 1'."));
    }
    return value;
  }

  HostBridge* absl_nonnull bridge_;
  ReaderPtr reader_;
  bool unary_ = false;
  std::string label_;
  /// Whether a read is in progress, so the next caller waits its turn.
  bool busy_ = false;
  bool read_ = false;
  Value held_;
};

/// A cursor over a [Buffer], waiting per item rather than for the whole stream.
class ReplayReader : public Reader {
 public:
  explicit ReplayReader(std::shared_ptr<Buffer::State> state)
      : state_(std::move(state)) {}

  absl::StatusOr<ItemPtr> Next() override {
    if (stopped_) {
      return ItemPtr{};
    }
    ABSL_ASSIGN_OR_RETURN(ItemPtr item, state_->At(at_));
    if (item != nullptr) {
      ++at_;
    }
    return item;
  }

  void Stop() override { stopped_ = true; }

 private:
  std::shared_ptr<Buffer::State> state_;
  size_t at_ = 0;
  bool stopped_ = false;
};

absl::StatusOr<ReaderPtr> Buffer::Replay() {
  return ReaderPtr(new ReplayReader(state_));
}

// --- Destinations ------------------------------------------------------------

using NodePtr = std::shared_ptr<nodes::AsyncNode>;

/// End a node: mark the stream over, then close the write half.
absl::Status CloseNode(const NodePtr& node) {
  return node->Finalize({.wait = true}).Await().status();
}

/// A node several steps may write, closed when the last of them is done.
class Destination {
 public:
  using Open = std::function<absl::StatusOr<NodePtr>()>;

  Destination(Monitor& monitor, std::string label, Open open, int writers,
              bool tolerant)
      : monitor_(&monitor),
        turn_(monitor),
        label_(std::move(label)),
        open_(std::move(open)),
        writers_(writers),
        tolerant_(tolerant) {
    // A destination with no writers starts complete.
    if (writers_ <= 0) {
      finished_ = true;
    }
  }

  const std::string& label() const { return label_; }

  int writers() const { return writers_; }

  absl::StatusOr<NodePtr> Node() {
    {
      thread::MutexLock lock(&monitor_->mu());
      if (opening_) {
        ABSL_RETURN_IF_ERROR(monitor_->Wait(turn_, [this] { return opened_; }));
        return node_;
      }
      opening_ = true;
    }
    absl::StatusOr<NodePtr> node = open_();
    {
      thread::MutexLock lock(&monitor_->mu());
      node_ = node;
      opened_ = true;
    }
    turn_.Wake();
    return node;
  }

  /// Append one value, as the producer wrote it wherever possible.
  absl::Status Write(const ItemPtr& item, HostBridge* absl_nonnull bridge) {
    return WriteRange(absl::MakeConstSpan(&item, 1), bridge);
  }

  /// Append a batch in order after the writer queue admits it.
  ///
  /// Admission provides backpressure. Closing the destination drains pending
  /// confirmations and reports any store failure.
  absl::Status WriteMany(std::vector<ItemPtr>& items,
                         HostBridge* absl_nonnull bridge) {
    const absl::Status written = WriteRange(items, bridge);
    items.clear();
    return written;
  }

 private:
  absl::Status WriteRange(absl::Span<const ItemPtr> items,
                          HostBridge* absl_nonnull bridge) {
    if (items.empty()) {
      return absl::OkStatus();
    }
    ABSL_ASSIGN_OR_RETURN(const NodePtr node, Node());
    absl::StatusOr<std::shared_ptr<stores::ChunkStoreWriter>> writer =
        node->writer();
    ABSL_RETURN_IF_ERROR(writer.status());
    // Serialize writers so shared destinations append complete values. Encode
    // computed batches in one host crossing where supported.
    std::vector<absl::StatusOr<data::Chunk>> encoded;
    if (items.size() > 1) {
      std::vector<const Value*> values;
      std::vector<absl::StatusOr<Value>> read;
      read.reserve(items.size());
      for (const ItemPtr& item : items) {
        if (item->chunk().has_value()) {
          continue;
        }
        read.push_back(item->Read(bridge));
      }
      bool readable = true;
      for (const absl::StatusOr<Value>& value : read) {
        if (!value.ok()) {
          readable = false;
          break;
        }
      }
      if (readable && read.size() > 1) {
        values.reserve(read.size());
        for (const absl::StatusOr<Value>& value : read) {
          values.push_back(&*value);
        }
        encoded = bridge->ToChunks(values, {});
        if (encoded.size() != values.size()) {
          encoded.clear();
        }
      }
    }
    ABSL_RETURN_IF_ERROR(Enter());
    absl::Status status;
    size_t computed = 0;
    for (const ItemPtr& item : items) {
      data::Chunk chunk;
      if (item->chunk().has_value()) {
        chunk = *item->chunk();
      } else if (computed < encoded.size()) {
        absl::StatusOr<data::Chunk>& ready = encoded[computed++];
        if (!ready.ok()) {
          status = ready.status();
          break;
        }
        chunk = *std::move(ready);
      } else {
        absl::StatusOr<Value> value = item->Read(bridge);
        if (!value.ok()) {
          status = value.status();
          break;
        }
        absl::StatusOr<data::Chunk> one = bridge->ToChunk(*value, {});
        if (!one.ok()) {
          status = one.status();
          break;
        }
        chunk = *std::move(one);
      }
      stores::ChunkStoreWrite write = (*writer)->EnqueueChunk(std::move(chunk));
      status = write.admitted.Await().status();
      if (!status.ok()) {
        break;
      }
    }
    Leave();
    // A `try call` that has already failed or been cancelled has aborted its
    // ports; feeding one is then not the flow's problem.
    if (!status.ok() && !tolerant_) {
      return status;
    }
    return absl::OkStatus();
  }

 public:
  /// Close the node now, whoever was writing it.
  ///
  /// What `drain` does to a node the flow does not write itself: a callee given
  /// a node to write does not close it -- it does not own it -- so the flow
  /// that lent it the node is the one that says when it is over.
  absl::Status End() { return Finish(/*forced=*/true); }

  /// One writer is done; close the node when it was the last.
  ///
  /// The close writes the null final chunk that says the stream is over, so a
  /// reader waiting on a whole value is told the value has ended rather than
  /// left waiting.
  absl::Status Release() { return Finish(/*forced=*/false); }

  /// End the node with a failure, so its readers see the error and not an end.
  ///
  /// `drain` reports an ordinary end; this reports a failure so readers can
  /// distinguish completion from interruption.
  ///
  /// Forced, like End(): whoever was writing it, this is the last word. It also
  /// marks the node finished, so the ordinary Release() every step does on its
  /// way out finds nothing left to close.
  absl::Status Abort(absl::Status reason) {
    return Finish(/*forced=*/true, std::move(reason));
  }

  absl::Status Finished() {
    thread::MutexLock lock(&monitor_->mu());
    return monitor_->Wait(turn_, [this] { return finished_; });
  }

 private:
  absl::Status Enter() {
    thread::MutexLock lock(&monitor_->mu());
    ABSL_RETURN_IF_ERROR(monitor_->Wait(turn_, [this] { return !writing_; }));
    writing_ = true;
    return absl::OkStatus();
  }

  void Leave() {
    {
      thread::MutexLock lock(&monitor_->mu());
      writing_ = false;
    }
    turn_.Wake();
  }

  absl::Status Finish(bool forced, absl::Status reason = absl::OkStatus()) {
    ABSL_RETURN_IF_ERROR(Enter());
    bool close = false;
    {
      thread::MutexLock lock(&monitor_->mu());
      if (!closed_) {
        if (forced) {
          closed_ = true;
          writers_ = 0;
          close = true;
        } else {
          --writers_;
          if (writers_ <= 0) {
            closed_ = true;
            close = true;
          }
        }
      }
    }
    absl::Status status;
    if (close) {
      absl::StatusOr<NodePtr> node = Node();
      if (!node.ok()) {
        status = node.status();
      } else if (reason.ok()) {
        status = CloseNode(*node);
      } else {
        status = (*node)->AbortWithStatus(std::move(reason)).Await().status();
      }
      {
        thread::MutexLock lock(&monitor_->mu());
        finished_ = true;
      }
      // Whoever is waiting for this node to be over, and only them.
      turn_.Wake();
    }
    Leave();
    if (!status.ok() && !tolerant_) {
      return status;
    }
    return absl::OkStatus();
  }

  Monitor* absl_nonnull monitor_;
  /// Writers taking turns on the node, and whoever waits for it to be over.
  Monitor::Condition turn_;
  std::string label_;
  Open open_;
  int writers_ = 0;
  bool tolerant_ = false;
  bool writing_ = false;
  bool closed_ = false;
  bool finished_ = false;
  bool opening_ = false;
  bool opened_ = false;
  absl::StatusOr<NodePtr> node_;
};

// --- Calls -------------------------------------------------------------------

/// One instance of a `call` step: its action, its ports, its completion.
class CallHandle {
 public:
  explicit CallHandle(Monitor& monitor) : monitor_(&monitor) {}

  void Adopt(std::shared_ptr<actions::Action> action) {
    thread::MutexLock lock(&monitor_->mu());
    action_ = std::move(action);
  }

  void Started(std::shared_ptr<actions::Action> action) {
    {
      thread::MutexLock lock(&monitor_->mu());
      action_ = std::move(action);
      started_ = true;
    }
    monitor_->Wake();
  }

  /// Say why the call never started, so nothing waits on it for ever.
  void NeverStarted(absl::Status why) {
    {
      thread::MutexLock lock(&monitor_->mu());
      if (!started_) {
        started_ = true;
        start_error_ = why;
      }
      if (error_.ok()) {
        error_ = std::move(why);
      }
      if (!done_) {
        completion_order_ = monitor_->CompletionOrder();
      }
      done_ = true;
    }
    monitor_->Wake();
  }

  void Done() {
    {
      thread::MutexLock lock(&monitor_->mu());
      if (!done_) {
        completion_order_ = monitor_->CompletionOrder();
      }
      done_ = true;
    }
    monitor_->Wake();
  }

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

  void SetError(absl::Status error) {
    thread::MutexLock lock(&monitor_->mu());
    if (error_.ok()) {
      error_ = std::move(error);
    }
  }

  absl::Status error() {
    thread::MutexLock lock(&monitor_->mu());
    return error_;
  }

  absl::StatusOr<std::shared_ptr<actions::Action>> Action() {
    thread::MutexLock lock(&monitor_->mu());
    ABSL_RETURN_IF_ERROR(monitor_->Wait([this] { return started_; }));
    ABSL_RETURN_IF_ERROR(start_error_);
    return action_;
  }

  std::shared_ptr<actions::Action> action_now() {
    thread::MutexLock lock(&monitor_->mu());
    return action_;
  }

  /// The node of one of the call's ports, once the call has started.
  absl::StatusOr<NodePtr> Node(const std::string& name,
                               syntax::PortDirection direction) {
    ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<actions::Action> action,
                          Action());
    if (direction == syntax::PortDirection::kInput) {
      return action->GetInput(name, std::nullopt);
    }
    return action->GetOutput(name, std::nullopt);
  }

  /// How the call went, once it has gone.
  ///
  /// Two statuses, which is why this is not a `StatusOr`: the returned one
  /// says whether the *waiting* worked, and `outcome` is what the call
  /// finished with.
  absl::Status Outcome(absl::Status* absl_nonnull outcome) {
    std::shared_ptr<actions::Action> action;
    {
      thread::MutexLock lock(&monitor_->mu());
      ABSL_RETURN_IF_ERROR(monitor_->Wait([this] { return done_; }));
      if (!error_.ok()) {
        *outcome = error_;
        return absl::OkStatus();
      }
      action = action_;
    }
    *outcome = action == nullptr ? absl::OkStatus() : action->GetStatus();
    return absl::OkStatus();
  }

  /// Ports nobody writes and outputs nobody reads: closed and drained for the
  /// callee once it has started.
  std::vector<NodePtr> unclosed;
  std::vector<NodePtr> undrained;

 private:
  Monitor* absl_nonnull monitor_;
  std::shared_ptr<actions::Action> action_;
  bool started_ = false;
  bool done_ = false;
  uint64_t completion_order_ = 0;
  absl::Status start_error_;
  absl::Status error_;
};

// --- The runner and its scopes ----------------------------------------------

class Runner;

/// One running instance of a body: the flow's top level, or a loop pass.
class Scope {
 public:
  Scope(Runner& runner, BodyId body, Scope* absl_nullable parent,
        absl::flat_hash_map<RefId, std::vector<ItemPtr>> presets)
      : runner_(&runner),
        body_(body),
        parent_(parent),
        presets_(std::move(presets)) {}

  ~Scope();
  absl::Status Run();

  /// What the pass captured, by slot: a `repeat`'s carry and its condition.
  [[nodiscard]] const absl::flat_hash_map<std::string, Value>& captures()
      const {
    return captures_;
  }

 private:
  friend class Runner;

  absl::Status Prepare();
  absl::Status EnsureStep(StepId step);
  void SkipRef(RefId ref);
  absl::Status RunStep(StepId step);
  absl::Status Execute(StepId step);
  absl::Status StepDone(StepId step);
  void MarkDone(StepId step);

  absl::Status RunForEach(StepId step);
  absl::Status RunRepeat(StepId step);
  absl::StatusOr<bool> PassCondition(const graph::Step& one, const Scope& pass);
  void Record(StepId step, const absl::Status& outcome);
  absl::StatusOr<absl::Status> ChosenStatus(StepId step);
  absl::Status AbortNode(StepId step);
  absl::Status RunWait(StepId step);
  absl::Status RunWaitMany(StepId step);
  absl::Status Failure(StepId step);
  absl::Status WriteLog(const graph::LogTail& tail, const ItemPtr& subject);
  absl::Status LogValue(const Value& value, const Item* absl_nullable carried,
                        const actions::LogOptions& options);
  absl::Status Logged(const absl::Status& logged);

  /// An independent view of a ref's values, for one reader.
  absl::StatusOr<ReaderPtr> Subscribe(RefId ref);
  Scope* absl_nonnull Owner(RefId ref);
  Scope* absl_nullable FindOwner(RefId ref);
  Destination* absl_nullable FindDestination(RefId ref);
  absl::StatusOr<Destination*> DestinationOf(RefId ref);
  absl::StatusOr<CallHandle*> Call(StepId step);

  absl::Status Produce(RefId ref, const Sink& sink);
  absl::Status ProduceStage(RefId ref, const Sink& sink);
  absl::Status ProduceZip(RefId ref, const Sink& sink);
  absl::Status ProduceMerge(RefId ref, const Sink& sink);
  absl::StatusOr<ItemPtr> StatusItem(RefId ref);
  absl::StatusOr<ItemPtr> WinnerItem(RefId ref);
  absl::Status NodeOutcome(RefId ref, absl::Status* absl_nonnull outcome);
  absl::StatusOr<NodePtr> ReadableNode(RefId ref);
  absl::StatusOr<NodePtr> DestinationNode(RefId ref);
  absl::StatusOr<NodePtr> LocalNode(RefId ref);
  absl::StatusOr<NodePtr> MakeLocalNode(RefId ref);

  /// Read a node as items, each keeping the producer's own chunk.
  absl::Status ReadNode(const NodePtr& node, bool tolerant, const Sink& sink);

  /// Run a per-value stage with the requested fixed concurrency.
  ///
  /// Workers serialize reads from the shared cursor and process values
  /// concurrently. Results retain input order unless the stage is `unordered`.
  absl::Status InParallel(
      const graph::Stage& stage, Reader& source, const Sink& sink,
      absl::FunctionRef<absl::Status(const ItemPtr&, std::vector<ItemPtr>&)>
          body,
      bool reads_values);

  absl::StatusOr<Value> ValueOf(RefId ref);
  absl::StatusOr<Value> Evaluate(ExprId expr);
  absl::StatusOr<Value> EvaluateWith(ExprId expr, const Value& it,
                                     bool join = true);
  /// What an aggregating stage compares or adds: the value itself, or what its
  /// expression makes of it. `| sum` and `| sum it.price` differ by this and
  /// nothing else, and so do `| min` and `| sort by`.
  absl::StatusOr<Value> StageValue(const graph::Stage& stage,
                                   const ItemPtr& item, HostBridge& host);
  absl::Status Tolerated(const graph::Stage& stage, const ItemPtr& item,
                         const absl::Status& why, HostBridge& host);
  /// One pass of a `fold`: the accumulator bound to its name, `it` to the
  /// value.
  absl::StatusOr<Value> EvaluateFold(const graph::Stage& stage,
                                     const Value& carried, const Value& it);

  [[nodiscard]] const FlowGraph& graph() const;
  [[nodiscard]] Monitor& monitor() const;
  [[nodiscard]] HostBridge& bridge() const;
  /// The shapes the program declared, for a cast or a coercion in this body.
  [[nodiscard]] const Program& shapes() const;

  Runner* absl_nonnull runner_;
  BodyId body_ = kNone;
  Scope* absl_nullable parent_ = nullptr;
  absl::flat_hash_map<RefId, std::vector<ItemPtr>> presets_;
  /// Who reads and writes what in this body -- the run's copy, not one of its
  /// own: a `for` runs one Scope per pass, and the answer is a property of the
  /// body rather than of the pass. See [Runner::AnalysisOf].
  const graph::Analysis* absl_nonnull analysis_ = nullptr;
  absl::flat_hash_map<RefId, std::unique_ptr<Bus>> buses_;
  absl::flat_hash_map<RefId, std::unique_ptr<Lazy>> lazies_;
  absl::flat_hash_map<RefId, std::unique_ptr<Buffer>> buffers_;
  absl::flat_hash_map<RefId, std::unique_ptr<Destination>> destinations_;
  absl::flat_hash_map<StepId, std::unique_ptr<CallHandle>> calls_;
  absl::flat_hash_map<StepId, bool> done_;
  absl::flat_hash_map<StepId, uint64_t> completion_order_;
  /// How a `[try] { ... }` step went, for the name bound to it to read.
  absl::flat_hash_map<StepId, absl::Status> outcomes_;
  absl::flat_hash_map<std::string, Value> captures_;
  /// Which subject each `wait first of` in this body saw finish first, once it
  /// has. Read through the barrier's winner ref, which waits for the step.
  absl::flat_hash_map<StepId, std::int64_t> winners_;
  // What a `fold` is carrying right now, per fold in this body. One entry per
  // fold, and a fold runs on one fiber -- it reads its stream to the end -- so
  // the only concurrency here is a second fold elsewhere in the same body.
  /// What a `fold` is carrying right now, per fold in this body.
  ///
  /// A fold's accumulator is a name in an expression, and a name in an
  /// expression is a ref: this is where the ref's value comes from, filled in
  /// per value by the stage rather than read off a stream. One entry per fold,
  /// and a fold runs on one fiber -- it reads its stream to the end -- so the
  /// only concurrency here is a second fold elsewhere in the same body.
  absl::flat_hash_map<RefId, Value> folds_;
  /// One cursor per ref, shared by every value read of it: see [ValueCursor].
  /// On the scope that *owns* the ref, so passes of a loop take turns on the
  /// one view of an outer stream rather than each starting it again.
  absl::flat_hash_map<RefId, std::unique_ptr<ValueCursor>> cursors_;
  /// Refs whose cursor is being made right now, so only one step subscribes.
  absl::flat_hash_set<RefId> opening_cursors_;
  absl::flat_hash_map<RefId, absl::StatusOr<NodePtr>> nodes_;
  absl::flat_hash_set<RefId> opening_;
  std::vector<RefId> unwritten_;

  struct OwnedNode {
    std::shared_ptr<nodes::NodeMap> map;
    NodePtr node;
    std::string id;
  };

  std::vector<OwnedNode> owned_nodes_;
  Scope* absl_nullable resource_owner_ = nullptr;
  Group* absl_nullable work_ = nullptr;
  absl::flat_hash_set<StepId> started_;
  std::shared_ptr<a11::Promise<Value>> expression_result_;
  Value expression_it_;
  absl::flat_hash_map<ExprId, std::shared_ptr<a11::Promise<a11::Unit>>>
      expression_cleanups_;
};

/// One execution of one flow, against the action it is running as.
class Runner {
 public:
  Runner(std::shared_ptr<const CompiledProgram> program,
         const ResolvedFlow& flow, std::shared_ptr<actions::Action> action,
         std::shared_ptr<HostBridge> bridge,
         std::shared_ptr<net::WireStream> dispatch_stream,
         std::shared_ptr<ExecutionSnapshot> snapshot)
      : program_(std::move(program)),
        flow_(&flow),
        action_(std::move(action)),
        bridge_(std::move(bridge)),
        dispatch_stream_(std::move(dispatch_stream)),
        snapshot_(snapshot == nullptr ? std::make_shared<ExecutionSnapshot>()
                                      : std::move(snapshot)) {}

  absl::Status Run() {
    const auto shared = monitor_;
    ABSL_RETURN_IF_ERROR(action_->SetOnCancelled(
        [shared](const std::shared_ptr<actions::Action>&) {
          shared->Stop(absl::CancelledError("Flow action cancelled"));
          return absl::OkStatus();
        }));
    ABSL_RETURN_IF_ERROR(Preflight());
    if (!snapshot_->sealed) {
      snapshot_->sealed = true;
    }
    Scope root(*this, flow_->graph.root, nullptr, {});
    const absl::Status status = root.Run();
    if (!status.ok()) {
      monitor_->Stop(status);
    }
    return status;
  }

  Monitor& monitor() { return *monitor_; }

  std::shared_ptr<Monitor> shared_monitor() { return monitor_; }

  HostBridge& bridge() { return *bridge_; }

  const Program& shapes() const { return program_->program(); }

  const FlowGraph& graph() const { return flow_->graph; }

  const FlowPlan& plan() const { return flow_->plan; }

  const std::shared_ptr<actions::Action>& action() const { return action_; }

  const std::shared_ptr<net::WireStream>& dispatch_stream() const {
    return dispatch_stream_;
  }

  /// The temporary node map a `nodes` block names.
  ///
  /// One map per name per execution. Nodes created in it are not in the
  /// session's node map, so a peer neither sees them nor receives their
  /// fragments.
  absl::StatusOr<std::shared_ptr<nodes::NodeMap>> NodeMapNamed(
      const std::string& name) {
    thread::MutexLock lock(&monitor_->mu());
    const auto found = node_maps_.find(name);
    if (found != node_maps_.end()) {
      return found->second;
    }
    ABSL_ASSIGN_OR_RETURN(std::shared_ptr<nodes::NodeMap> made,
                          nodes::NodeMap::Create());
    node_maps_.emplace(name, made);
    return made;
  }

  // Return the cached read/write analysis for one body. Analysis runs outside
  // the monitor lock.
  /// Return the cached read/write analysis for one body.
  ///
  /// Analysis runs outside the monitor lock. Concurrent first callers may
  /// compute the same immutable result before one cache entry wins.
  const graph::Analysis& AnalysisOf(BodyId body) {
    {
      thread::MutexLock lock(&monitor_->mu());
      const auto found = analyses_.find(body);
      if (found != analyses_.end()) {
        return *found->second;
      }
    }
    auto made =
        std::make_unique<graph::Analysis>(graph::Analyse(graph(), body));
    thread::MutexLock lock(&monitor_->mu());
    const auto [at, added] = analyses_.emplace(body, std::move(made));
    return *at->second;
  }

  /// An id for a node the flow declared, unique within this run.
  ///
  /// Named after the flow's own action and the name the flow gave it, so a node
  /// a peer does see is recognisable rather than a bare identifier.
  absl::StatusOr<std::string> FreshNodeId(const std::string& name) {
    uint64_t count = 0;
    {
      thread::MutexLock lock(&monitor_->mu());
      auto& next = node_counts_[name];
      if (next == std::numeric_limits<uint64_t>::max()) {
        return absl::ResourceExhaustedError(
            "Flow node identity space exhausted.");
      }
      count = ++next;
    }
    const std::string suffix =
        count == 1 ? name : absl::StrCat(name, "-", count);
    return actions::Action::MakeNodeId(action_->GetId(), suffix);
  }

  /// The schema and handler for a call target.
  ///
  /// The handler may be empty: an action registered for its schema alone is one
  /// whose ports are known here and whose *work* happens on the peer, and that
  /// is what a flow composing a gateway's actions from the outside registers.
  /// Such an action can only be `call`ed; what `run` needs is a handler, and
  /// saying `run` without one is an error rather than a quiet trip to the
  /// session.
  absl::Status Resolve(const std::string& name, actions::ActionSchema* schema,
                       actions::ActionHandler* handler);
  absl::Status Preflight();

  /// The schema of an action this flow calls, resolved once per run.
  ///
  /// By pointer, because a schema is a map of ports and this is asked for once
  /// per call step *per pass of a loop*: returning it by value copied every
  /// port name of every action a loop body calls, once per value the loop read.
  /// The table holds the schemas by pointer so an answer stays valid while it
  /// grows.
  absl::StatusOr<const actions::ActionSchema*> SchemaOf(
      const std::string& action) {
    {
      thread::MutexLock lock(&monitor_->mu());
      const auto found = schemas_.find(action);
      if (found != schemas_.end()) {
        return found->second.get();
      }
    }
    auto schema = std::make_unique<actions::ActionSchema>();
    actions::ActionHandler handler;
    ABSL_RETURN_IF_ERROR(Resolve(action, schema.get(), &handler));
    thread::MutexLock lock(&monitor_->mu());
    const auto [at, added] = schemas_.emplace(action, std::move(schema));
    return at->second.get();
  }

  // Whether this body's call ports have already been checked against the
  // schemas of what they call.
  /// Whether this body's call ports have already been checked against the
  /// schemas of what they call.
  ///
  /// The check is a property of the body and the registry, neither of which
  /// changes while a flow runs, so a loop body is checked on its first pass and
  /// not on its thousandth.
  bool PortsChecked(BodyId body) {
    thread::MutexLock lock(&monitor_->mu());
    return !checked_bodies_.insert(body).second;
  }

  /// The headers `forward headers` sends on to a step, as they arrived.
  ///
  /// A pattern matches the flow's own headers -- what its caller sent -- and
  /// `*` in one matches any run of characters. Nothing is invented: a header
  /// that was not sent is simply not forwarded, because a composition should
  /// not fail over an optional one nobody supplied.
  data::ByteMap Forwarded(const graph::Step& step) const {
    data::ByteMap chosen;
    if (step.forward.empty()) {
      return chosen;
    }
    const data::ByteMap available = action_->Headers();
    for (const std::string& pattern : step.forward) {
      for (const auto& [name, value] : available) {
        if (MatchesFolded(name, pattern)) {
          chosen[name] = value;
        }
      }
    }
    return chosen;
  }

  absl::Status RunCall(Scope& scope, StepId step);

 private:
  absl::Status StartCall(Scope& scope, StepId step, CallHandle& handle);
  absl::Status PumpCall(StepId step, CallHandle& handle);
  absl::Status AwaitCall(const graph::Step& step, CallHandle& handle);

  std::shared_ptr<const CompiledProgram> program_;
  const ResolvedFlow* absl_nonnull flow_;
  std::shared_ptr<actions::Action> action_;
  std::shared_ptr<HostBridge> bridge_;
  std::shared_ptr<net::WireStream> dispatch_stream_;
  std::shared_ptr<ExecutionSnapshot> snapshot_;
  std::shared_ptr<Monitor> monitor_ = std::make_shared<Monitor>();
  absl::flat_hash_map<std::string, std::shared_ptr<nodes::NodeMap>> node_maps_;
  absl::flat_hash_map<std::string, uint64_t> node_counts_;
  /// One analysis per body, however many passes read it. Held by pointer so a
  /// Scope can point at it while the table grows around it.
  absl::flat_hash_map<BodyId, std::unique_ptr<graph::Analysis>> analyses_;
  absl::flat_hash_map<std::string, std::unique_ptr<actions::ActionSchema>>
      schemas_;
  /// Bodies whose call ports have been checked; see [PortsChecked].
  absl::flat_hash_set<BodyId> checked_bodies_;
  absl::flat_hash_map<std::string,
                      std::pair<actions::ActionSchema, actions::ActionHandler>>
      bindings_;
};

Scope::~Scope() {
  for (const auto& [step, handle] : calls_) {
    const auto action = handle->action_now();
    if (action == nullptr || action->IsDone()) {
      continue;
    }
    action->Cancel().IgnoreError();

    struct Completion {
      explicit Completion(std::shared_ptr<Monitor> shared)
          : monitor(std::move(shared)), changed(*monitor) {}

      std::shared_ptr<Monitor> monitor;
      Monitor::Condition changed;
      bool ready = false;
    };

    const auto state = std::make_shared<Completion>(runner_->shared_monitor());
    action->Wait().OnReady([state](const auto&) {
      thread::MutexLock lock(&state->monitor->mu());
      state->ready = true;
      state->changed.Wake();
    });
    thread::MutexLock lock(&state->monitor->mu());
    state->monitor->WaitComplete(state->changed, [&] { return state->ready; });
  }
  for (const auto& owned : owned_nodes_) {
    owned.map->Discard(owned.id, owned.node).status().IgnoreError();
  }
  for (const auto& [step, handle] : calls_) {
    const auto action = handle->action_now();
    if (action == nullptr || graph().steps[step].action_id != kNone) {
      continue;
    }
    const auto map = action->GetNodeMap();
    if (map == nullptr) {
      continue;
    }
    const auto discard = [&](std::string_view name) {
      const auto id = actions::Action::MakeNodeId(action->GetId(), name);
      if (!id.ok()) {
        return;
      }
      const auto node = map->GetIfExists(*id);
      if (node.ok() && *node != nullptr) {
        map->Discard(*id, *node).status().IgnoreError();
      }
    };
    const auto schema = action->GetSchema();
    for (const auto* ports : {&schema.inputs, &schema.outputs}) {
      for (const auto& [name, port] : *ports) {
        discard(name);
      }
    }
    for (const auto name :
         {actions::kActionStatusOutput, actions::kActionDispatchStatusOutput,
          actions::kActionLogOutput}) {
      discard(name);
    }
  }
}

const FlowGraph& Scope::graph() const {
  return runner_->graph();
}

Monitor& Scope::monitor() const {
  return runner_->monitor();
}

HostBridge& Scope::bridge() const {
  return runner_->bridge();
}

const Program& Scope::shapes() const {
  return runner_->shapes();
}

// --- Wiring ------------------------------------------------------------------

absl::Status Scope::Prepare() {
  const FlowGraph& flow = graph();
  analysis_ = &runner_->AnalysisOf(body_);

  const bool check_ports = !runner_->PortsChecked(body_);
  for (const StepId step : flow.bodies[body_].steps) {
    done_[step] = false;
    if (flow.steps[step].kind != StepKind::kCall) {
      continue;
    }
    if (!check_ports) {
      calls_.emplace(step, std::make_unique<CallHandle>(monitor()));
      continue;
    }
    ABSL_ASSIGN_OR_RETURN(const actions::ActionSchema* schema,
                          runner_->SchemaOf(flow.steps[step].action));
    // A port the target does not declare, rejected before anything runs.
    for (const auto& [key, ref] : flow.steps[step].ports) {
      const bool input = absl::StartsWith(key, "inputs:");
      const std::string name = flow.refs[ref].name;
      const auto& declared = input ? schema->inputs : schema->outputs;
      if (declared.contains(name)) {
        continue;
      }
      std::vector<std::string> known;
      known.reserve(declared.size());
      for (const auto& [port, unused] : declared) {
        known.push_back(port);
      }
      std::sort(known.begin(), known.end());
      return Fail(
          absl::StrCat(flow.steps[step].action, " has no ",
                       input ? "input" : "output", " port '", name,
                       "' (declared: ",
                       known.empty() ? "none" : absl::StrJoin(known, ", "),
                       "), but ", flow.refs[ref].label, " names one."),
          absl::StatusCode::kNotFound);
    }
    calls_.emplace(step, std::make_unique<CallHandle>(monitor()));
  }

  for (const RefId ref : analysis_->refs) {
    const auto readers = analysis_->readers.find(ref);
    const int count = readers == analysis_->readers.end() ? 0 : readers->second;
    if (count <= 0 || presets_.contains(ref)) {
      continue;
    }
    // A fold's accumulator is read by name and produced by the fold itself, one
    // value at a time. Giving it a bus would be a producer nothing ever asks
    // for -- and a pump waiting to be wanted is a flow that never ends.
    if (flow.refs[ref].kind == RefKind::kBound &&
        flow.refs[ref].role == "fold") {
      continue;
    }
    if (flow.refs[ref].kind == RefKind::kStatus) {
      lazies_.emplace(ref, std::make_unique<Lazy>(monitor(), [this, ref] {
                        return StatusItem(ref);
                      }));
      continue;
    }
    // Which subject of a race won: produced by the barrier, so reading it is
    // waiting for the barrier -- the same shape as a status.
    if (flow.refs[ref].kind == RefKind::kWinner) {
      lazies_.emplace(ref, std::make_unique<Lazy>(monitor(), [this, ref] {
                        return WinnerItem(ref);
                      }));
      continue;
    }
    auto bus = std::make_unique<Bus>(
        monitor(), flow.refs[ref].label,
        [this, ref](const Sink& sink) { return Produce(ref, sink); }, count);
    if (analysis_->materialise.contains(ref)) {
      // Read from inside a loop or a branch: buffered once here, replayed per
      // pass, so the buffer is the one reader of the underlying stream.
      ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, bus->Take());
      buffers_.emplace(ref,
                       std::make_unique<Buffer>(monitor(), std::move(reader)));
    }
    buses_.emplace(ref, std::move(bus));
  }

  // A node of the flow's own that nothing writes still has to end, or a reader
  // of it would wait for a value that was never coming.
  for (const RefId ref : analysis_->nodes) {
    if (flow.refs[ref].id_expr != kNone) {
      continue;
    }
    const auto readers = analysis_->readers.find(ref);
    if (readers == analysis_->readers.end() || readers->second <= 0) {
      continue;
    }
    if (std::find(analysis_->destinations.begin(),
                  analysis_->destinations.end(),
                  ref) != analysis_->destinations.end()) {
      continue;
    }
    unwritten_.push_back(ref);
  }

  for (const RefId ref : analysis_->destinations) {
    const auto writers = analysis_->writers.find(ref);
    const int count = writers == analysis_->writers.end() ? 0 : writers->second;
    const graph::Ref& one = flow.refs[ref];
    const bool tolerant = one.kind == RefKind::kCallPort && one.call != kNone &&
                          flow.steps[one.call].tolerant;
    destinations_.emplace(ref, std::make_unique<Destination>(
                                   monitor(), one.label,
                                   [this, ref] { return DestinationNode(ref); },
                                   count, tolerant));
  }
  return absl::OkStatus();
}

// --- Lookups -----------------------------------------------------------------

Scope* absl_nullable Scope::FindOwner(RefId ref) {
  const BodyId owner = graph().refs[ref].owner;
  for (Scope* at = this; at != nullptr; at = at->parent_) {
    if (at->body_ == owner) {
      return at;
    }
  }
  return nullptr;
}

Scope* absl_nonnull Scope::Owner(RefId ref) {
  Scope* found = FindOwner(ref);
  return found == nullptr ? this : found;
}

absl::StatusOr<CallHandle*> Scope::Call(StepId step) {
  for (Scope* at = this; at != nullptr; at = at->parent_) {
    const auto found = at->calls_.find(step);
    if (found != at->calls_.end()) {
      ABSL_RETURN_IF_ERROR(at->EnsureStep(step));
      return found->second.get();
    }
  }
  return Fail(absl::StrCat("Internal flow error: ", graph().steps[step].label,
                           " was never started."));
}

Destination* absl_nullable Scope::FindDestination(RefId ref) {
  for (Scope* at = this; at != nullptr; at = at->parent_) {
    const auto found = at->destinations_.find(ref);
    if (found != at->destinations_.end()) {
      return found->second.get();
    }
  }
  return nullptr;
}

absl::StatusOr<Destination*> Scope::DestinationOf(RefId ref) {
  Destination* found = FindDestination(ref);
  if (found != nullptr) {
    return found;
  }
  return Fail(absl::StrCat("Internal flow error: nothing writes ",
                           graph().refs[ref].label, "."));
}

absl::StatusOr<ReaderPtr> Scope::Subscribe(RefId ref) {
  if (ref == kNone) {
    return Fail("Internal flow error: nothing to read.");
  }
  Scope* owner = FindOwner(ref);
  if (owner == nullptr) {
    return Fail(absl::StrCat("Internal flow error: ", graph().refs[ref].label,
                             " is not in scope here."));
  }
  if (const auto found = owner->presets_.find(ref);
      found != owner->presets_.end()) {
    return ReaderPtr(new ListReader(found->second));
  }
  if (const auto found = owner->lazies_.find(ref);
      found != owner->lazies_.end()) {
    return found->second->Replay();
  }
  if (const auto found = owner->buffers_.find(ref);
      found != owner->buffers_.end()) {
    return found->second->Replay();
  }
  const auto found = owner->buses_.find(ref);
  if (found == owner->buses_.end()) {
    return Fail(absl::StrCat("Internal flow error: ", graph().refs[ref].label,
                             " has no reader slot left."));
  }
  return found->second->Take();
}

// --- Nodes -------------------------------------------------------------------

absl::StatusOr<NodePtr> Scope::LocalNode(RefId ref) {
  Scope* owner = Owner(ref);
  Monitor& monitor = this->monitor();
  {
    thread::MutexLock lock(&monitor.mu());
    if (owner->opening_.contains(ref)) {
      ABSL_RETURN_IF_ERROR(
          monitor.Wait([owner, ref] { return owner->nodes_.contains(ref); }));
      return owner->nodes_.at(ref);
    }
    owner->opening_.insert(ref);
  }
  absl::StatusOr<NodePtr> node = owner->MakeLocalNode(ref);
  {
    thread::MutexLock lock(&monitor.mu());
    owner->nodes_.insert_or_assign(ref, node);
  }
  monitor.Wake();
  return node;
}

absl::StatusOr<NodePtr> Scope::MakeLocalNode(RefId ref) {
  if (work_ != nullptr) {
    ABSL_RETURN_IF_ERROR(work_->CheckActive());
  }
  const graph::Ref& one = graph().refs[ref];
  std::shared_ptr<nodes::NodeMap> map;
  if (one.node_map.empty()) {
    map = runner_->action()->GetNodeMap();
  } else {
    ABSL_ASSIGN_OR_RETURN(map, runner_->NodeMapNamed(one.node_map));
  }
  if (map == nullptr) {
    return Fail(absl::StrCat(one.label, " has no node map to live in."));
  }
  std::string node_id;
  if (one.id_expr != kNone) {
    // A declaration with an id attaches to the node that names -- the one a
    // caller passed in a header, say -- and writes there instead.
    ABSL_ASSIGN_OR_RETURN(const Value named, Evaluate(one.id_expr));
    const Value* field = named.Get("id");
    node_id = AsText(field == nullptr ? named : *field);
    if (node_id.empty()) {
      return Fail(
          absl::StrCat(one.label, " was given no node id to attach to."));
    }
  } else {
    ABSL_ASSIGN_OR_RETURN(node_id, runner_->FreshNodeId(one.name));
  }
  ABSL_ASSIGN_OR_RETURN(auto node, map->Get(node_id));
  if (one.id_expr == kNone) {
    thread::MutexLock lock(&monitor().mu());
    Scope* owner = resource_owner_ == nullptr ? this : resource_owner_;
    owner->owned_nodes_.push_back({map, node, std::move(node_id)});
  }
  return node;
}

absl::StatusOr<NodePtr> Scope::DestinationNode(RefId ref) {
  const graph::Ref& one = graph().refs[ref];
  switch (one.kind) {
    case RefKind::kCallPort: {
      ABSL_ASSIGN_OR_RETURN(CallHandle * handle, Call(one.call));
      return handle->Node(one.name, one.direction);
    }
    case RefKind::kFlowPort:
      return runner_->action()->GetOutput(one.name);
    case RefKind::kNode:
      return LocalNode(ref);
    default:
      return Fail(
          absl::StrCat(one.label, " is not something a flow can write."));
  }
}

absl::StatusOr<NodePtr> Scope::ReadableNode(RefId ref) {
  const graph::Ref& one = graph().refs[ref];
  switch (one.kind) {
    case RefKind::kNode:
      return LocalNode(ref);
    case RefKind::kCallPort: {
      ABSL_ASSIGN_OR_RETURN(CallHandle * handle, Call(one.call));
      return handle->Node(one.name, one.direction);
    }
    case RefKind::kFlowPort:
      if (one.direction == syntax::PortDirection::kInput) {
        return runner_->action()->GetInput(one.name);
      }
      return NodePtr{};
    default:
      return NodePtr{};
  }
}

absl::Status Scope::ReadNode(const NodePtr& node, bool tolerant,
                             const Sink& sink) {
  // Every value a flow reads enters here, so this is the one read worth
  // batching: `NextFragments` hands back what the store already had and waits
  // only when it had nothing, so a batch costs the same await as a value would.
  std::vector<ItemPtr> items;
  items.reserve(kQueueDepth);
  while (true) {
    absl::StatusOr<std::vector<std::optional<data::NodeFragment>>> batch =
        node->NextFragments(kQueueDepth).Await();
    if (!batch.ok()) {
      // The producer aborted the node. A `try call` says the composition
      // expects that and the stream simply ends; otherwise the call step is the
      // one that reports it, so there is no need to fail twice.
      if (tolerant) {
        return absl::OkStatus();
      }
      return batch.status();
    }
    bool ended = false;
    for (std::optional<data::NodeFragment>& fragment : *batch) {
      if (!fragment.has_value()) {
        ended = true;
        break;
      }
      ABSL_ASSIGN_OR_RETURN(data::Chunk * chunk, fragment->GetChunk());
      // A null chunk is a marker rather than a value: a final one ends the
      // node, and any other is skipped, which is how an empty stream stays
      // empty instead of turning into a value nobody wrote.
      if (chunk->IsNull()) {
        if (!fragment->continued) {
          ended = true;
          break;
        }
        continue;
      }
      // Moved out of the fragment, which nothing else reads: a value's payload
      // is the value, and copying it here is a copy per stage.
      items.push_back(Item::OfChunk(std::move(*chunk)));
    }
    ABSL_RETURN_IF_ERROR(sink.Many(items));
    if (ended) {
      return absl::OkStatus();
    }
  }
}

// --- Producing streams -------------------------------------------------------

absl::Status Scope::Produce(RefId ref, const Sink& sink) {
  const graph::Ref& one = graph().refs[ref];
  // One place, upstream of the bus that fans the stream out, so the values
  // `skip n` spoke for are the same ones every reader misses.
  long long taken = 0;
  const long long skip = one.skip;
  const auto pass = [&](ItemPtr item) -> absl::Status {
    if (taken < skip) {
      ++taken;
      return absl::OkStatus();
    }
    return sink.One(std::move(item));
  };
  const Sink skipping(pass);
  const Sink& onward = skip > 0 ? skipping : sink;
  switch (one.kind) {
    case RefKind::kDerived:
      return ProduceStage(ref, onward);
    case RefKind::kZip:
      return ProduceZip(ref, onward);
    case RefKind::kMerge:
      return ProduceMerge(ref, onward);
    case RefKind::kExpr: {
      ABSL_ASSIGN_OR_RETURN(const Value value, Evaluate(one.expr));
      return onward.One(Item::Of(value));
    }
    case RefKind::kHeader: {
      ABSL_ASSIGN_OR_RETURN(const std::optional<data::Bytes> raw,
                            runner_->action()->GetHeader(one.header));
      if (!raw.has_value()) {
        return absl::OkStatus();
      }
      return onward.One(Item::Of(Value::String(*raw)));
    }
    case RefKind::kNodeId: {
      ABSL_ASSIGN_OR_RETURN(const NodePtr node, LocalNode(one.subject));
      ABSL_ASSIGN_OR_RETURN(const std::string id, node->GetId());
      return onward.One(Item::Of(Value::String(id)));
    }
    case RefKind::kNode: {
      ABSL_ASSIGN_OR_RETURN(const NodePtr node, LocalNode(ref));
      return ReadNode(node, /*tolerant=*/false, onward);
    }
    case RefKind::kCallPort: {
      ABSL_ASSIGN_OR_RETURN(CallHandle * handle, Call(one.call));
      const auto node = handle->Node(one.name, one.direction);
      if (!node.ok()) {
        return graph().steps[one.call].tolerant ? absl::OkStatus()
                                                : node.status();
      }
      return ReadNode(*node, graph().steps[one.call].tolerant, onward);
    }
    case RefKind::kFlowPort: {
      ABSL_ASSIGN_OR_RETURN(const NodePtr node,
                            runner_->action()->GetInput(one.name));
      return ReadNode(node, /*tolerant=*/false, onward);
    }
    default:
      return Fail(
          absl::StrCat(one.label, " is not something a flow can read."));
  }
}

// Read several streams in step, as one stream of tuples. A fiber per source
// would buy nothing and would put three more lifetimes on the run's monitor.
/// Read several streams in step, as one stream of tuples.
///
/// **No fiber of its own per source, and that is not a shortcut.** A tuple is
/// not complete until every source has answered, so asking them one after
/// another finishes at exactly the moment asking them at once would -- and
/// every reader here is an ordinary subscription, which already blocks, already
/// relays a producer's failure, and already has the buffering the analysis
/// arranged. A fiber per source would buy nothing and would put three more
/// lifetimes on the run's monitor.
///
/// A source that ends **well** is latched: from then on it contributes a null
/// to every tuple, which is what lets a short stream be zipped against a long
/// one without either being padded by its author. A source that ends **badly**
/// ends the whole thing with its status, because a tuple missing a value for a
/// reason nobody has been told about is worse than no tuple at all. It stops
/// when every source has ended.
absl::Status Scope::ProduceZip(RefId ref, const Sink& sink) {
  const graph::Ref& one = graph().refs[ref];
  std::vector<ReaderPtr> readers;
  readers.reserve(one.sources.size());
  for (const RefId source : one.sources) {
    ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(source));
    readers.push_back(std::move(reader));
  }
  std::vector<bool> ended(readers.size(), false);
  size_t running = readers.size();
  HostBridge& host = bridge();

  while (running > 0) {
    std::vector<Value> tuple;
    tuple.reserve(readers.size());
    for (size_t index = 0; index < readers.size(); ++index) {
      if (ended[index]) {
        tuple.push_back(Value::Null());
        continue;
      }
      absl::StatusOr<ItemPtr> item = readers[index]->Next();
      if (!item.ok()) {
        // Give up on the rest rather than leave them reading into a buffer
        // nothing will ever take from.
        for (ReaderPtr& reader : readers) {
          reader->Stop();
        }
        return item.status();
      }
      if (*item == nullptr) {
        ended[index] = true;
        --running;
        tuple.push_back(Value::Null());
        continue;
      }
      ABSL_ASSIGN_OR_RETURN(Value value, (*item)->Read(&host));
      tuple.push_back(std::move(value));
    }
    // The round in which the last source ended produced nothing but the nulls
    // that say so, and a tuple of nothing but nulls is not a value anybody
    // wrote.
    if (running == 0) {
      break;
    }
    ABSL_RETURN_IF_ERROR(sink.One(Item::Of(Value::List(std::move(tuple)))));
  }
  return absl::OkStatus();
}

/// Read several streams at once, as one stream of their values.
///
/// **A fiber per source, unlike a zip, and for the opposite reason.** A tuple
/// is not complete until every source has answered, so asking them one after
/// another finishes when asking them at once would. An interleaved value is
/// complete as soon as *one* source has answered, so reading them in turn would
/// make a fast stream wait behind a slow one -- which is the whole thing this
/// exists to avoid. The fibers are the sources the author named, and they are
/// what "in the order values arrive" costs.
///
/// Publishing takes the bus lock, so concurrent values are ordered by lock
/// acquisition. A successful source end contributes no value; a failed source
/// ends the merged stream with its status.
absl::Status Scope::ProduceMerge(RefId ref, const Sink& sink) {
  const graph::Ref& one = graph().refs[ref];
  std::vector<ReaderPtr> readers;
  readers.reserve(one.sources.size());
  for (const RefId source : one.sources) {
    ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(source));
    readers.push_back(std::move(reader));
  }
  Group group(monitor(), work_);
  const auto drain = [&sink](Reader* absl_nonnull reader) -> absl::Status {
    std::vector<ItemPtr> batch;
    batch.reserve(kQueueDepth);
    while (true) {
      batch.clear();
      ABSL_RETURN_IF_ERROR(reader->NextMany(batch, kQueueDepth));
      if (batch.empty()) {
        return absl::OkStatus();
      }
      ABSL_RETURN_IF_ERROR(sink.Many(batch));
    }
  };
  // One of them on this fiber, so `interleave(a, b)` costs one extra fiber and
  // not two.
  for (size_t index = 1; index < readers.size(); ++index) {
    Reader* reader = readers[index].get();
    group.Spawn([&drain, reader]() -> absl::Status { return drain(reader); });
  }
  absl::Status mine;
  if (!readers.empty()) {
    mine = drain(readers.front().get());
  }
  absl::Status joined = group.Join();
  ABSL_RETURN_IF_ERROR(mine);
  return joined;
}

/// Read a derived ref's source and reshape it with one stage.
absl::Status Scope::ProduceStage(RefId ref, const Sink& sink) {
  const graph::Ref& one = graph().refs[ref];
  const graph::Stage& stage = one.stage;
  ABSL_ASSIGN_OR_RETURN(ReaderPtr source, Subscribe(one.source));
  HostBridge& host = bridge();
  const std::string& name = stage.name;

  if (name == "consume") {
    ABSL_ASSIGN_OR_RETURN(ItemPtr item, source->Next());
    if (item == nullptr) {
      return absl::FailedPreconditionError(
          "'consume' requires exactly one item; the stream is empty.");
    }
    ABSL_ASSIGN_OR_RETURN(ItemPtr extra, source->Next());
    if (extra != nullptr) {
      source->Stop();
      return absl::FailedPreconditionError(
          "'consume' requires exactly one item; a second item arrived.");
    }
    return sink.One(std::move(item));
  }

  // For a stage that gathers: one value at a time, and whatever it publishes it
  // publishes itself. Reading is batched even here, since the source hands back
  // what it already had.
  std::vector<ItemPtr> pending;
  pending.reserve(kQueueDepth);
  const auto each = [&](absl::FunctionRef<absl::Status(const ItemPtr&)> body)
      -> absl::Status {
    while (true) {
      pending.clear();
      ABSL_RETURN_IF_ERROR(source->NextMany(pending, kQueueDepth));
      if (pending.empty()) {
        return absl::OkStatus();
      }
      for (const ItemPtr& item : pending) {
        ABSL_RETURN_IF_ERROR(body(item));
      }
    }
  };

  /// For a stage that reshapes each value: a batch in, a batch out.
  ///
  /// The input batch is only ever what the source *already had*, so nothing is
  /// held back waiting for company -- a stage handed one value publishes one
  /// value, and a pipeline still paces itself value by value. What the batch
  /// saves is the handover: one lock and one broadcast at each end for up to
  /// `kQueueDepth` values instead of one of each per value, which on a moving
  /// pipeline was most of what a value cost.
  ///
  /// A body that fails publishes what it had produced first, so a reader that
  /// had seen three of five values sees three values and then the failure --
  /// the same order the one-at-a-time path gave it.
  /// What a `try` does with a value the stage could not do.
  ///
  /// The failure is a value like any other: written to the stream `into` named,
  /// as the same status record `status x` yields, or -- with nowhere to send it
  /// -- logged once at warning, which is the least a language whose failures
  /// are values should do with one it was told to tolerate.
  const auto tolerate = [&](const ItemPtr& item,
                            const absl::Status& why) -> absl::Status {
    if (stage.failures != kNone) {
      ABSL_ASSIGN_OR_RETURN(Destination * destination,
                            DestinationOf(stage.failures));
      return destination->Write(Item::Of(StatusRecord(why)), &host);
    }
    actions::LogOptions options;
    options.level = "warning";
    return LogValue(Value::String(absl::StrCat(
                        one.label, " skipped a value: ", why.message())),
                    item.get(), options);
  };

  const auto per_value =
      [&](absl::FunctionRef<absl::Status(const ItemPtr&, std::vector<ItemPtr>&)>
              body,
          bool reads_values = true) -> absl::Status {
    if (stage.parallel > 1) {
      return InParallel(stage, *source, sink, body, reads_values);
    }
    std::vector<ItemPtr> in;
    std::vector<ItemPtr> out;
    in.reserve(kQueueDepth);
    out.reserve(kQueueDepth);
    while (true) {
      in.clear();
      ABSL_RETURN_IF_ERROR(source->NextMany(in, kQueueDepth));
      if (in.empty()) {
        return absl::OkStatus();
      }
      if (reads_values) {
        PrimeBatch(in, &host);
      }
      for (const ItemPtr& item : in) {
        absl::Status ran = body(item, out);
        if (!ran.ok()) {
          if (!stage.tolerant) {
            sink.Many(out).IgnoreError();
            return ran;
          }
          // Emit completed values, account for this failure, and continue the
          // stream according to the stage's `try` semantics.
          ABSL_RETURN_IF_ERROR(sink.Many(out));
          ABSL_RETURN_IF_ERROR(tolerate(item, ran));
        }
      }
      ABSL_RETURN_IF_ERROR(sink.Many(out));
    }
  };

  if (name == "at") {
    const Value key =
        stage.indexed ? Value::Integer(stage.index) : Value::String(stage.text);
    if (stage.named_or_indexed) {
      // A destructuring `let`: the field where the value has one, and the
      // position where it is a list.
      const Value position = Value::Integer(stage.index);
      return per_value(
          [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
            ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
            Value found = Lookup(value, key);
            if (found.IsNull()) {
              found = Lookup(value, position);
            }
            out.push_back(Item::Of(std::move(found)));
            return absl::OkStatus();
          });
    }
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
          out.push_back(Item::Of(Lookup(value, key)));
          return absl::OkStatus();
        });
  }
  if (name == "map") {
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
          ABSL_ASSIGN_OR_RETURN(Value mapped, EvaluateWith(stage.expr, value));
          out.push_back(Item::Of(std::move(mapped)));
          return absl::OkStatus();
        });
  }
  if (name == "take while" || name == "take until") {
    while (true) {
      ABSL_ASSIGN_OR_RETURN(ItemPtr item, source->Next());
      if (item == nullptr) {
        return absl::OkStatus();
      }
      const auto condition = [&]() -> absl::StatusOr<Value> {
        ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
        return EvaluateWith(stage.expr, value);
      }();
      if (!condition.ok()) {
        if (!stage.tolerant) {
          return condition.status();
        }
        ABSL_RETURN_IF_ERROR(tolerate(item, condition.status()));
        continue;
      }
      if (Truthy(*condition) == (name == "take until")) {
        source->Stop();
        return absl::OkStatus();
      }
      ABSL_RETURN_IF_ERROR(sink.One(std::move(item)));
    }
  }
  if (name == "where") {
    return per_value([&](const ItemPtr& item,
                         std::vector<ItemPtr>& out) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      ABSL_ASSIGN_OR_RETURN(const Value kept, EvaluateWith(stage.expr, value));
      if (Truthy(kept)) {
        out.push_back(item);
      }
      return absl::OkStatus();
    });
  }
  if (name == "log" || name == "logf") {
    // Shaped like `where`, not like `map`: the value is read so the log can say
    // something about it, and then emitted *as the item it was* bytes, mimetype
    // and all so dropping a log into a pipeline changes nothing about.
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          ABSL_RETURN_IF_ERROR(WriteLog(stage.log, item));
          out.push_back(item);
          return absl::OkStatus();
        });
  }
  if (name == "match") {
    // Compiled once: the pattern is written once in the source, and a bad one
    // is the flow's own mistake rather than something a value could fix.
    const pattern::Compiled compiled = pattern::Compile(stage.text);
    if (!compiled.ok()) {
      return Fail(absl::StrCat("The pattern '", stage.text,
                               "' cannot be read: ", compiled.error));
    }
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
          Value found = MatchCompiled(compiled.pattern, AsText(value));
          // A value the pattern does not fit is dropped, which is what makes
          // this a combined `where` and `map` stage.
          if (!found.IsNull()) {
            out.push_back(Item::Of(std::move(found)));
          }
          return absl::OkStatus();
        });
  }
  if (name == "flatten") {
    // The inverse of `batch`: a list becomes its own values, and anything else
    // is one value however it is looked at, so a mixed stream is flattened
    // rather than refused.
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
          if (value.kind() != Value::Kind::kList) {
            out.push_back(item);
            return absl::OkStatus();
          }
          for (const Value& held : value.items()) {
            out.push_back(Item::Of(held));
          }
          return absl::OkStatus();
        });
  }
  if (name == "pace") {
    // Spaced out, not sampled: every value goes on, later than it arrived. The
    // producer is held back behind the queue rather than being asked to stop,
    // which is what makes this a rate limit and not a drop.
    absl::Time next = absl::InfinitePast();
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          const absl::Time now = absl::Now();
          if (now < next) {
            thread::SleepFor(next - now);
          }
          next = std::max(now, next) + stage.duration;
          out.push_back(item);
          return absl::OkStatus();
        },
        /*reads_values=*/false);
  }
  if (name == "timeout") {
    // The gap between values, not the total: a stream that keeps arriving runs
    // as long as it likes. Read one at a time, because a batch that waits for
    // the first value and then takes four more would be timing the batch.
    while (true) {
      absl::StatusOr<ItemPtr> item =
          source->NextUntil(absl::Now() + stage.duration);
      if (!item.ok()) {
        if (!absl::IsDeadlineExceeded(item.status())) {
          return item.status();
        }
        return Fail(
            absl::StrCat("Nothing arrived on ", graph().refs[one.source].label,
                         " for ", absl::FormatDuration(stage.duration), "."),
            absl::StatusCode::kDeadlineExceeded);
      }
      if (*item == nullptr) {
        return absl::OkStatus();
      }
      ABSL_RETURN_IF_ERROR(sink.One(*std::move(item)));
    }
  }
  if (name == "mime") {
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          if (Matches(item->Mimetype(), stage.text)) {
            out.push_back(item);
          }
          return absl::OkStatus();
        },
        /*reads_values=*/false);
  }
  if (name == "first") {
    if (stage.count <= 0) {
      return absl::OkStatus();
    }
    long long taken = 0;
    while (taken < stage.count) {
      ABSL_ASSIGN_OR_RETURN(const ItemPtr item, source->Next());
      if (item == nullptr) {
        return absl::OkStatus();
      }
      ABSL_RETURN_IF_ERROR(sink.One(item));
      ++taken;
    }
    source->Stop();
    return absl::OkStatus();
  }
  if (name == "last") {
    std::deque<ItemPtr> tail;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      tail.push_back(item);
      if (static_cast<long long>(tail.size()) > stage.count) {
        tail.pop_front();
      }
      return absl::OkStatus();
    }));
    for (const ItemPtr& item : tail) {
      ABSL_RETURN_IF_ERROR(sink.One(item));
    }
    return absl::OkStatus();
  }
  if (name == "drop") {
    long long seen = 0;
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          if (++seen > stage.count) {
            out.push_back(item);
          }
          return absl::OkStatus();
        },
        /*reads_values=*/false);
  }
  if (name == "truncate") {
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
          out.push_back(Item::Of(Truncate(value, stage.count)));
          return absl::OkStatus();
        });
  }
  if (name == "batch") {
    std::vector<Value> group;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      group.push_back(value);
      if (static_cast<long long>(group.size()) < stage.count) {
        return absl::OkStatus();
      }
      ABSL_RETURN_IF_ERROR(sink.One(Item::Of(Value::List(std::move(group)))));
      group.clear();
      return absl::OkStatus();
    }));
    if (group.empty()) {
      return absl::OkStatus();
    }
    return sink.One(Item::Of(Value::List(std::move(group))));
  }
  if (name == "window") {
    // The overlapping counterpart of `batch`.
    if (stage.count <= 0) {
      return Fail(absl::StrCat("'window ", stage.count,
                               "' is not a width; a window holds at least one "
                               "value."));
    }
    const auto width = static_cast<size_t>(stage.count);
    // A deque of at most `width`, so the cost is the window and not the stream:
    // one over something endless is what this is for.
    std::deque<Value> held;
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      held.push_back(value);
      if (held.size() < width) {
        // Nothing yet: a window narrower than it was asked for is not a window,
        // and a stream shorter than one yields nothing at all. That is the
        // deliberate difference from `batch`, whose last list may be short.
        return absl::OkStatus();
      }
      if (held.size() > width) {
        held.pop_front();
      }
      return sink.One(
          Item::Of(Value::List(std::vector<Value>(held.begin(), held.end()))));
    });
  }
  if (name == "chunk") {
    // The other direction from `let`: one value, cut into pieces of a size
    // somebody downstream cares about.
    if (stage.count <= 0) {
      return Fail(absl::StrCat("'chunk ", stage.count,
                               "' is not a size; a chunk holds at least one "
                               "byte."));
    }
    const auto size = static_cast<size_t>(stage.count);
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      if (!value.IsTextlike()) {
        // Nothing to cut. A list is `batch`'s business and everything else is
        // one value however it is looked at, so it goes through unchanged
        // rather than being refused.
        return sink.One(Item::Of(value));
      }
      const std::string& text = value.text();
      const bool bytes = value.kind() == Value::Kind::kBytes;
      size_t at = 0;
      while (at < text.size()) {
        size_t take = std::min(size, text.size() - at);
        if (!bytes) {
          // Back off to the start of the character this would have split.
          while (take > 1 && at + take < text.size() &&
                 utf8::IsContinuation(text[at + take])) {
            --take;
          }
        }
        std::string piece = text.substr(at, take);
        ABSL_RETURN_IF_ERROR(
            sink.One(Item::Of(bytes ? Value::Bytes(std::move(piece))
                                    : Value::String(std::move(piece)))));
        at += take;
      }
      return absl::OkStatus();
    });
  }
  if (name == "then") {
    // All of this one, then all of that one. Two writers to a node interleave
    // by arrival, which is fine for pages and wrong for a conversation; this is
    // how a flow says which comes first.
    const auto forward = [&](const ItemPtr& item,
                             std::vector<ItemPtr>& out) -> absl::Status {
      out.push_back(item);
      return absl::OkStatus();
    };
    ABSL_RETURN_IF_ERROR(per_value(forward, /*reads_values=*/false));
    ABSL_ASSIGN_OR_RETURN(source, Subscribe(stage.stream));
    return per_value(forward, /*reads_values=*/false);
  }
  if (name == "group") {
    // `batch`, closed by a question rather than by a count: values pile up
    // until one of them says the group is finished, and the group goes on as a
    // list.
    std::vector<Value> gathered;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      gathered.push_back(value);
      ABSL_ASSIGN_OR_RETURN(const Value closes,
                            EvaluateWith(stage.expr, value));
      if (!Truthy(closes)) {
        return absl::OkStatus();
      }
      ABSL_RETURN_IF_ERROR(
          sink.One(Item::Of(Value::List(std::move(gathered)))));
      gathered.clear();
      return absl::OkStatus();
    }));
    if (gathered.empty()) {
      return absl::OkStatus();
    }
    return sink.One(Item::Of(Value::List(std::move(gathered))));
  }
  if (name == "collect") {
    std::vector<Value> collected;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      collected.push_back(value);
      return absl::OkStatus();
    }));
    return sink.One(Item::Of(Value::List(std::move(collected))));
  }
  if (name == "count") {
    std::int64_t total = 0;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr&) -> absl::Status {
      ++total;
      return absl::OkStatus();
    }));
    return sink.One(Item::Of(Value::Integer(total)));
  }
  if (name == "sum" || name == "avg") {
    // `sum` with no expression adds the values themselves; with one it adds
    // what the expression makes of each, which is what `| sum it.price` says.
    Value total = Value::Integer(0);
    std::int64_t seen = 0;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, StageValue(stage, item, host));
      ABSL_ASSIGN_OR_RETURN(total, Add(total, value));
      ++seen;
      return absl::OkStatus();
    }));
    if (name == "sum") {
      return sink.One(Item::Of(std::move(total)));
    }
    // The mean of nothing is not zero: an empty stream yields nothing, the same
    // way `min` of nothing does.
    if (seen == 0) {
      return absl::OkStatus();
    }
    const Value counted = Value::Double(static_cast<double>(seen));
    ABSL_ASSIGN_OR_RETURN(const Value mean, Divide(total, counted));
    return sink.One(Item::Of(mean));
  }
  if (name == "min" || name == "max") {
    // The smallest or largest of no values is not a value, so an empty stream
    // yields nothing rather than a zero somebody would have to know to ignore.
    const int wanted = name == "min" ? -1 : 1;
    std::optional<Value> best;
    ItemPtr keep;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, StageValue(stage, item, host));
      if (!best.has_value() || Order(value, *best) == wanted) {
        best = value;
        keep = item;
      }
      return absl::OkStatus();
    }));
    if (!best.has_value()) {
      return absl::OkStatus();
    }
    // The item as it arrived where the stage compared the values themselves, so
    // a pipe that only moves them still re-writes the producer's own bytes; the
    // computed value where an expression chose it.
    if (stage.expr == kNone && keep != nullptr) {
      return sink.One(keep);
    }
    return sink.One(Item::Of(*std::move(best)));
  }
  if (name == "fold") {
    // Two things bound rather than one: `it` is the value in hand and the name
    // the author chose is what the last pass produced.
    Value carried = Value::Of(stage.start);
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      ABSL_ASSIGN_OR_RETURN(carried, EvaluateFold(stage, carried, value));
      return absl::OkStatus();
    }));
    return sink.One(Item::Of(std::move(carried)));
  }
  if (name == "scan") {
    // `fold`, with the values published as they are computed rather than only
    // the last one.
    Value carried = Value::Of(stage.start);
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      ABSL_ASSIGN_OR_RETURN(carried, EvaluateFold(stage, carried, value));
      // Copied, not moved: the next value needs the state this one produced.
      return sink.One(Item::Of(carried));
    });
  }
  if (name == "sort") {
    // The whole stream, because which value comes first is not knowable until
    // the last one has arrived.
    std::vector<std::pair<Value, ItemPtr>> keyed;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value key, StageValue(stage, item, host));
      keyed.emplace_back(key, item);
      return absl::OkStatus();
    }));
    const bool descending = stage.descending;
    std::stable_sort(keyed.begin(), keyed.end(),
                     [descending](const std::pair<Value, ItemPtr>& left,
                                  const std::pair<Value, ItemPtr>& right) {
                       const int order = Order(left.first, right.first);
                       return descending ? order > 0 : order < 0;
                     });
    std::vector<ItemPtr> ordered;
    ordered.reserve(keyed.size());
    for (auto& [key, item] : keyed) {
      ordered.push_back(std::move(item));
    }
    return sink.Many(ordered);
  }
  if (name == "distinct") {
    absl::flat_hash_set<std::string> seen;
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      if (!seen.insert(AsText(value)).second) {
        return absl::OkStatus();
      }
      return sink.One(item);
    });
  }
  if (name == "join") {
    std::vector<std::string> pieces;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      pieces.push_back(AsText(value));
      return absl::OkStatus();
    }));
    return sink.One(Item::Of(Value::String(absl::StrJoin(pieces, stage.text))));
  }
  if (name == "strformat") {
    // The one-value shorthand: `| strformat "took %s"` is exactly
    // `| map strformat("took %s", it)`, which is the shape almost every use of
    // it has. The full builtin is there when more than one value goes in.
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      const Value arguments[] = {value};
      return sink.One(Item::Of(
          Value::String(Strformat(Value::String(stage.text), arguments))));
    });
  }
  if (name == "text") {
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      return sink.One(Item::Of(Value::String(AsText(value))));
    });
  }
  if (name == "json") {
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      return sink.One(Item::Of(AsJson(value)));
    });
  }
  if (name == "packb") {
    // An item read from a node keeps the producer's bytes, so a value that
    // arrived packed is passed on untouched, tag and all.
    return each([&](const ItemPtr& item) -> absl::Status {
      if (item->chunk().has_value() &&
          BaseMimetype(item->Mimetype()) == data::kMsgpackMimetype) {
        return sink.One(item);
      }
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      ABSL_ASSIGN_OR_RETURN(data::Chunk chunk,
                            host.ToChunk(value, data::kMsgpackMimetype));
      return sink.One(Item::OfChunk(std::move(chunk)));
    });
  }
  return Fail(absl::StrCat("Unknown stage '", name, "'."));
}

// --- Outcomes ----------------------------------------------------------------

absl::StatusOr<ItemPtr> Scope::StatusItem(RefId ref) {
  const graph::Ref& one = graph().refs[ref];
  absl::Status outcome;
  if (one.subject_step != kNone &&
      graph::RecordsOutcome(graph().steps[one.subject_step].kind)) {
    // A block's or a loop's outcome is recorded by the step itself, so reading
    // it is waiting for the step to be over and then looking. Waiting for a
    // step is what `after` does, and this is the same wait.
    ABSL_RETURN_IF_ERROR(StepDone(one.subject_step));
    for (Scope* at = this; at != nullptr; at = at->parent_) {
      thread::MutexLock lock(&monitor().mu());
      const auto found = at->outcomes_.find(one.subject_step);
      if (found != at->outcomes_.end()) {
        return Item::Of(StatusRecord(found->second));
      }
    }
    return Item::Of(StatusRecord(absl::OkStatus()));
  }
  if (one.subject_step != kNone) {
    ABSL_ASSIGN_OR_RETURN(CallHandle * handle, Call(one.subject_step));
    ABSL_RETURN_IF_ERROR(handle->Outcome(&outcome));
    return Item::Of(StatusRecord(outcome));
  }
  if (one.subject != kNone) {
    ABSL_RETURN_IF_ERROR(NodeOutcome(one.subject, &outcome));
    return Item::Of(StatusRecord(outcome));
  }
  return Fail(absl::StrCat(one.label, " has no status to read."));
}

// Which subject of a race finished first, as the value the barrier is. On the
// scope that ran the step, because a nested body's race is that body's.
/// Which subject of a race finished first, as the value the barrier is.
///
/// Reading it is waiting for the barrier -- the same wait `after` does -- and
/// then looking at what it recorded. On the scope that ran the step, because a
/// nested body's race is that body's.
absl::StatusOr<ItemPtr> Scope::WinnerItem(RefId ref) {
  const graph::Ref& one = graph().refs[ref];
  if (one.subject_step == kNone) {
    return Fail(absl::StrCat(one.label, " has no race to read."));
  }
  ABSL_RETURN_IF_ERROR(StepDone(one.subject_step));
  for (Scope* at = this; at != nullptr; at = at->parent_) {
    thread::MutexLock lock(&monitor().mu());
    const auto found = at->winners_.find(one.subject_step);
    if (found != at->winners_.end()) {
      return Item::Of(Value::Integer(found->second));
    }
  }
  // The barrier is done and recorded nothing, which happens when it was given
  // up on rather than won. Reading it then says so rather than answering 0.
  return Fail(absl::StrCat(one.label, " never finished."),
              absl::StatusCode::kFailedPrecondition);
}

/// A node's outcome: its writers are done, or its stream has ended.
///
/// A node this flow writes is finished when the last writer has closed it, so
/// the status is the one the node was closed or aborted with. One it only reads
/// is finished when the stream ends, and the status is the reader's -- which is
/// how an output cut off mid-stream gets noticed.
absl::Status Scope::NodeOutcome(RefId ref, absl::Status* absl_nonnull outcome) {
  if (Destination* written = FindDestination(ref); written != nullptr) {
    if (written->writers() <= 0) {
      // Nothing in this flow writes it, so nothing in this flow will close it
      // either unless this barrier does.
      ABSL_RETURN_IF_ERROR(written->End());
    }
    ABSL_RETURN_IF_ERROR(written->Finished());
    ABSL_ASSIGN_OR_RETURN(const NodePtr node, written->Node());
    const std::optional<absl::Status> aborted = node->GetWriterAbortStatus();
    *outcome = aborted.has_value() ? *aborted : node->GetWriterStatus();
    return absl::OkStatus();
  }
  ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(ref));
  while (true) {
    absl::StatusOr<ItemPtr> item = reader->Next();
    if (!item.ok()) {
      // The producer cut the stream short, and that is the outcome rather than
      // this flow's own failure.
      *outcome = item.status();
      return absl::OkStatus();
    }
    if (*item == nullptr) {
      break;
    }
  }
  ABSL_ASSIGN_OR_RETURN(const NodePtr node, ReadableNode(ref));
  *outcome = node == nullptr ? absl::OkStatus() : node->GetReaderStatus();
  return absl::OkStatus();
}

// --- Expressions -------------------------------------------------------------

absl::StatusOr<Value> Scope::ValueOf(RefId ref) {
  Scope* owner = Owner(ref);
  // A fold's accumulator is not read from a stream: the stage folding right now
  // put it there, and this is the name that reads it.
  {
    thread::MutexLock lock(&monitor().mu());
    const auto folded = owner->folds_.find(ref);
    if (folded != owner->folds_.end()) {
      return folded->second;
    }
  }
  ValueCursor* cursor = nullptr;
  {
    thread::MutexLock lock(&monitor().mu());
    // One cursor per ref, and one *subscription* per ref.
    ABSL_RETURN_IF_ERROR(monitor().Wait([owner, ref] {
      return owner->cursors_.contains(ref) ||
             !owner->opening_cursors_.contains(ref);
    }));
    const auto found = owner->cursors_.find(ref);
    if (found != owner->cursors_.end()) {
      cursor = found->second.get();
    } else {
      owner->opening_cursors_.insert(ref);
    }
  }
  if (cursor == nullptr) {
    // Outside the lock: subscribing may open a node.
    absl::StatusOr<ReaderPtr> reader = Subscribe(ref);
    // Taken before the reader is moved out below: what is left behind still has
    // this status, but a reader of the code should not have to know that.
    absl::Status subscribed = reader.status();
    {
      thread::MutexLock lock(&monitor().mu());
      // Whatever happened, this ref is no longer being opened: a failure that
      // left the flag set would leave every other reader of it waiting for a
      // cursor nobody is making any more.
      owner->opening_cursors_.erase(ref);
      if (subscribed.ok()) {
        const graph::Ref& one = graph().refs[ref];
        auto made = std::make_unique<ValueCursor>(bridge(), *std::move(reader),
                                                  one.unary, one.label);
        cursor = made.get();
        owner->cursors_.emplace(ref, std::move(made));
      }
    }
    monitor().Wake();
    if (!subscribed.ok()) {
      return subscribed;
    }
  }
  return cursor->Next(monitor());
}

absl::StatusOr<Value> Scope::Evaluate(ExprId expr) {
  return EvaluateWith(expr, expression_it_, false);
}

absl::StatusOr<Value> Scope::EvaluateWith(ExprId expr, const Value& it,
                                          bool join) {
  if (expr == kNone) {
    return Value::Null();
  }
  if (work_ != nullptr) {
    ABSL_RETURN_IF_ERROR(work_->CheckActive());
  }
  const graph::Expr& one = graph().exprs[expr];
  if (one.node == nullptr) {
    return Value::Null();
  }
  if (one.activation != kNone) {
    if (work_ == nullptr) {
      return Fail("Expression has no owning execution scope.");
    }
    auto child = std::make_shared<Scope>(
        *runner_, one.activation, this,
        absl::flat_hash_map<RefId, std::vector<ItemPtr>>{});
    child->resource_owner_ =
        join ? child.get()
             : (resource_owner_ == nullptr ? this : resource_owner_);
    child->expression_result_ = std::make_shared<a11::Promise<Value>>();
    child->expression_it_ = it;
    auto result = child->expression_result_->future();
    auto done = std::make_shared<a11::Promise<a11::Unit>>();
    auto completed = done->future();
    if (!join && one.pipeline_owned) {
      thread::MutexLock lock(&monitor().mu());
      expression_cleanups_.insert_or_assign(expr, done);
    }
    const bool pipeline_owned = one.pipeline_owned;
    work_->Spawn([child, done, join, pipeline_owned]() -> absl::Status {
      const absl::Status status = child->Run();
      if (!status.ok()) {
        child->expression_result_->SetStatus(status).IgnoreError();
      } else {
        child->expression_result_->SetValue(Value::Null()).IgnoreError();
      }
      done->SetResult(status.ok() ? absl::StatusOr<a11::Unit>(a11::Unit{})
                                  : absl::StatusOr<a11::Unit>(status))
          .IgnoreError();
      return join || pipeline_owned ? absl::OkStatus() : status;
    });
    auto value = result.Await();
    if (join) {
      const auto cleanup = completed.Await();
      if (!cleanup.ok()) {
        return cleanup.status();
      }
    }
    return value;
  }
  absl::flat_hash_map<RefId, Value> taken;
  EvalContext context;
  context.lookup =
      [&](const syntax::Node& node) -> absl::StatusOr<std::optional<Value>> {
    for (const auto& [bound, ref] : one.bound) {
      if (bound != &node) {
        continue;
      }
      if (const auto cached = taken.find(ref); cached != taken.end()) {
        return std::optional<Value>(cached->second);
      }
      ABSL_ASSIGN_OR_RETURN(Value value, ValueOf(ref));
      taken.emplace(ref, value);
      return std::optional<Value>(std::move(value));
    }
    return std::optional<Value>();
  };
  context.it = it;
  context.has_it = true;
  context.bridge = &bridge();
  context.shapes = &shapes();
  auto value = flow::Evaluate(*one.node, context);
  for (const RefId ref : one.refs) {
    if (taken.find(ref) == taken.end()) {
      SkipRef(ref);
    } else if (expression_result_ != nullptr &&
               graph().refs[ref].owner == body_) {
      const auto cursor = cursors_.find(ref);
      if (cursor != cursors_.end()) {
        cursor->second->Stop();
      }
    }
  }
  return value;
}

void Scope::SkipRef(RefId initial) {
  std::vector<RefId> pending{initial};
  while (!pending.empty()) {
    const RefId ref = pending.back();
    pending.pop_back();
    Scope* owner = FindOwner(ref);
    if (owner == nullptr ||
        owner->buffers_.find(ref) != owner->buffers_.end()) {
      continue;
    }
    const auto bus = owner->buses_.find(ref);
    if (bus == owner->buses_.end() || !bus->second->Skip()) {
      continue;
    }
    for (const RefId upstream : graph().Upstreams(ref)) {
      pending.push_back(upstream);
    }
    for (const RefId upstream : graph().ValueRefs(ref)) {
      pending.push_back(upstream);
    }
    const graph::Ref& node = graph().refs[ref];
    if (node.kind == RefKind::kCallPort && node.call != kNone &&
        graph().steps[node.call].deferred) {
      bool inactive;
      {
        thread::MutexLock lock(&monitor().mu());
        inactive = owner->started_.find(node.call) == owner->started_.end();
      }
      if (inactive) {
        for (const StepId step : graph().bodies[owner->body_].steps) {
          if (graph().steps[step].activation == node.call) {
            for (const RefId input : graph().Sources(step)) {
              pending.push_back(input);
            }
          }
        }
        for (const RefId input : graph().ValueSources(node.call)) {
          pending.push_back(input);
        }
      }
    }
  }
}

absl::Status Scope::EnsureStep(StepId step) {
  if (work_ != nullptr) {
    ABSL_RETURN_IF_ERROR(work_->CheckActive());
  }
  if (work_ == nullptr) {
    return Fail("A step has no owning execution scope.");
  }
  {
    thread::MutexLock lock(&monitor().mu());
    if (!started_.insert(step).second) {
      return absl::OkStatus();
    }
  }
  work_->Spawn([this, step]() -> absl::Status { return RunStep(step); });
  if (graph().steps[step].kind == StepKind::kCall) {
    for (const StepId feed : graph().bodies[body_].steps) {
      if (graph().steps[feed].activation == step) {
        ABSL_RETURN_IF_ERROR(EnsureStep(feed));
      }
    }
  }
  return absl::OkStatus();
}

absl::Status Scope::InParallel(
    const graph::Stage& stage, Reader& source, const Sink& sink,
    absl::FunctionRef<absl::Status(const ItemPtr&, std::vector<ItemPtr>&)> body,
    bool reads_values) {
  HostBridge& host = bridge();
  Monitor& monitor = this->monitor();
  // One condition for the whole stage: the workers wait on each other, and
  // there are as many of them as the author asked for.
  Monitor::Condition turn(monitor);

  struct Shared {
    /// Which value is read next, and which is published next.
    std::int64_t reading = 0;
    std::int64_t publishing = 0;
    bool ended = false;
    absl::Status failure;
    /// Whether somebody is inside the reader right now: one cursor, one reader.
    bool taking = false;
  };

  Shared shared;
  const bool ordered = stage.ordered;
  const bool tolerant = stage.tolerant;

  const auto worker = [&]() -> absl::Status {
    std::vector<ItemPtr> out;
    while (true) {
      // A turn at the reader, and the number this value keeps.
      ItemPtr item;
      std::int64_t index = 0;
      {
        thread::MutexLock lock(&monitor.mu());
        ABSL_RETURN_IF_ERROR(monitor.Wait(
            turn, [&shared] { return !shared.taking || shared.ended; }));
        if (shared.ended || !shared.failure.ok()) {
          return absl::OkStatus();
        }
        shared.taking = true;
      }
      absl::StatusOr<ItemPtr> read = source.Next();
      {
        thread::MutexLock lock(&monitor.mu());
        shared.taking = false;
        if (!read.ok()) {
          if (shared.failure.ok()) {
            shared.failure = read.status();
          }
          shared.ended = true;
        } else if (*read == nullptr) {
          shared.ended = true;
        } else {
          item = *std::move(read);
          index = shared.reading++;
        }
      }
      turn.Wake();
      if (item == nullptr) {
        return absl::OkStatus();
      }

      out.clear();
      if (reads_values) {
        (void)item->Read(&host);
      }
      const absl::Status ran = body(item, out);

      // Publishing. Ordered means waiting for the values before this one, which
      // is what puts the stream back in the order it was read in.
      {
        thread::MutexLock lock(&monitor.mu());
        if (ordered) {
          ABSL_RETURN_IF_ERROR(monitor.Wait(turn, [&shared, index] {
            return shared.publishing == index || !shared.failure.ok();
          }));
        }
        if (!shared.failure.ok()) {
          return absl::OkStatus();
        }
      }
      absl::Status published;
      if (ran.ok()) {
        published = sink.Many(out);
      } else if (tolerant) {
        published = Tolerated(stage, item, ran, host);
      } else {
        published = ran;
      }
      {
        thread::MutexLock lock(&monitor.mu());
        if (ordered) {
          ++shared.publishing;
        }
        if (!published.ok() && shared.failure.ok()) {
          shared.failure = published;
          shared.ended = true;
        }
      }
      turn.Wake();
      if (!published.ok()) {
        return absl::OkStatus();
      }
    }
  };

  Group group(monitor, work_);
  for (int index = 1; index < stage.parallel; ++index) {
    group.Spawn([&worker]() -> absl::Status { return worker(); });
  }
  // This fiber is one of the workers, so `parallel 2` costs one extra fiber and
  // not two.
  absl::Status mine = worker();
  absl::Status joined = group.Join();
  {
    thread::MutexLock lock(&monitor.mu());
    if (!shared.failure.ok()) {
      return shared.failure;
    }
  }
  ABSL_RETURN_IF_ERROR(mine);
  return joined;
}

/// What a `try` stage does with a value it could not do: see `tolerate` in
/// [Scope::ProduceStage], which this is the parallel path's copy of.
absl::Status Scope::Tolerated(const graph::Stage& stage, const ItemPtr& item,
                              const absl::Status& why, HostBridge& host) {
  if (stage.failures != kNone) {
    ABSL_ASSIGN_OR_RETURN(Destination * destination,
                          DestinationOf(stage.failures));
    return destination->Write(Item::Of(StatusRecord(why)), &host);
  }
  actions::LogOptions options;
  options.level = "warning";
  return LogValue(
      Value::String(absl::StrCat("a value was skipped: ", why.message())),
      item.get(), options);
}

absl::StatusOr<Value> Scope::StageValue(const graph::Stage& stage,
                                        const ItemPtr& item, HostBridge& host) {
  ABSL_ASSIGN_OR_RETURN(Value value, item->Read(&host));
  if (stage.expr == kNone) {
    return value;
  }
  return EvaluateWith(stage.expr, value);
}

absl::StatusOr<Value> Scope::EvaluateFold(const graph::Stage& stage,
                                          const Value& carried,
                                          const Value& it) {
  if (stage.carry != kNone) {
    thread::MutexLock lock(&monitor().mu());
    folds_.insert_or_assign(stage.carry, carried);
  }
  return EvaluateWith(stage.expr, it);
}

// --- Execution ---------------------------------------------------------------

absl::Status Scope::Run() {
  ABSL_RETURN_IF_ERROR(Prepare());
  Group group(monitor(), parent_ == nullptr ? nullptr : parent_->work_);
  work_ = &group;
  // A node of the flow's own that nothing writes is ended here, and without a
  // fiber each: `Finalize` hands the write and the close to the node's own
  // writer pump, so the awaitables are collected and waited on together.
  std::vector<a11::Task> closing;
  closing.reserve(unwritten_.size());
  for (const RefId ref : unwritten_) {
    ABSL_ASSIGN_OR_RETURN(const NodePtr node, LocalNode(ref));
    closing.push_back(node->Finalize({.wait = true}));
  }
  for (const absl::StatusOr<a11::Unit>& closed : a11::AwaitAll(closing)) {
    ABSL_RETURN_IF_ERROR(closed.status());
  }
  for (const auto& [ref, bus] : buses_) {
    Bus* one = bus.get();
    group.Spawn([one]() -> absl::Status { return one->Pump(); });
  }
  for (const auto& [ref, buffer] : buffers_) {
    Buffer* one = buffer.get();
    group.Spawn([one]() -> absl::Status { return one->Fill(); });
  }
  for (const StepId step : graph().bodies[body_].steps) {
    if (!graph().steps[step].deferred) {
      ABSL_RETURN_IF_ERROR(EnsureStep(step));
    }
  }
  const absl::Status result = group.Join();
  if (!result.ok()) {
    for (const auto& owned : owned_nodes_) {
      owned.node->AbortWithStatus(result).Await().IgnoreError();
    }
    for (const auto& [step, handle] : calls_) {
      const auto action = handle->action_now();
      if (action != nullptr) {
        action->Cancel().IgnoreError();
      }
    }
  }
  work_ = nullptr;
  return result;
}

void Scope::MarkDone(StepId step) {
  {
    thread::MutexLock lock(&monitor().mu());
    done_[step] = true;
    completion_order_[step] = monitor().CompletionOrder();
  }
  monitor().Wake();
}

absl::Status Scope::StepDone(StepId step) {
  for (Scope* at = this; at != nullptr; at = at->parent_) {
    const auto found = at->done_.find(step);
    if (found == at->done_.end()) {
      continue;
    }
    ABSL_RETURN_IF_ERROR(at->EnsureStep(step));
    thread::MutexLock lock(&monitor().mu());
    return monitor().Wait([at, step] { return at->done_.find(step)->second; });
  }
  return Fail(absl::StrCat("Internal flow error: ", graph().steps[step].label,
                           " is not in scope."));
}

absl::Status Scope::RunStep(StepId step) {
  absl::Status status;
  for (const StepId dependency : graph().steps[step].after) {
    status = StepDone(dependency);
    if (!status.ok()) {
      break;
    }
  }
  if (status.ok()) {
    status = Execute(step);
  }
  if (absl::IsCancelled(status)) {
    const auto cause = work_->CheckActive();
    if (!cause.ok()) {
      status = cause;
    }
  }
  const auto held = analysis_->held.find(step);
  if (held != analysis_->held.end()) {
    for (const RefId ref : held->second) {
      if (absl::StatusOr<Destination*> destination = DestinationOf(ref);
          destination.ok()) {
        (*destination)->Release().IgnoreError();
      }
    }
  }
  if (graph::RecordsOutcome(graph().steps[step].kind)) {
    thread::MutexLock lock(&monitor().mu());
    outcomes_.try_emplace(step, status);
  }
  MarkDone(step);
  return status;
}

absl::Status Scope::Execute(StepId step) {
  const graph::Step& one = graph().steps[step];
  switch (one.kind) {
    case StepKind::kCall:
      return runner_->RunCall(*this, step);
    case StepKind::kPipe: {
      const auto transfer = [&]() -> absl::Status {
        if (one.discard) {
          // `-> _`.
          ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(one.source));
          std::vector<ItemPtr> dropped;
          dropped.reserve(kQueueDepth);
          while (true) {
            dropped.clear();
            if (absl::Status read = reader->NextMany(dropped, kQueueDepth);
                !read.ok()) {
              // The same reading of `try` the writing path takes: a source that
              // gave way is this statement's outcome rather than the flow's.
              if (!one.tolerant) {
                return read;
              }
              Record(step, read);
              return absl::OkStatus();
            }
            if (dropped.empty()) {
              return absl::OkStatus();
            }
          }
        }
        ABSL_ASSIGN_OR_RETURN(Destination * destination,
                              DestinationOf(one.destination));
        ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(one.source));
        std::vector<ItemPtr> batch;
        batch.reserve(kQueueDepth);
        while (true) {
          batch.clear();
          // Both halves under the same `try`, because the author cannot tell
          // which gave way: a source that aborted and a destination that refused
          // the write are one event from here -- "this pipe did not finish".
          if (absl::Status read = reader->NextMany(batch, kQueueDepth);
              !read.ok()) {
            if (!one.tolerant) {
              return read;
            }
            Record(step, read);
            return absl::OkStatus();
          }
          if (batch.empty()) {
            return absl::OkStatus();
          }
          if (absl::Status wrote = destination->WriteMany(batch, &bridge());
              !wrote.ok()) {
            if (!one.tolerant) {
              return wrote;
            }
            Record(step, wrote);
            return absl::OkStatus();
          }
        }
      };
      absl::Status result = transfer();
      for (const ExprId expr : one.expression_cleanups) {
        std::shared_ptr<a11::Promise<a11::Unit>> cleanup;
        {
          thread::MutexLock lock(&monitor().mu());
          const auto found = expression_cleanups_.find(expr);
          if (found != expression_cleanups_.end()) {
            cleanup = found->second;
          }
        }
        if (cleanup == nullptr) {
          continue;
        }
        const auto status = cleanup->future().Await().status();
        if (!status.ok()) {
          Record(step, status);
        }
      }
      return result;
    }
    case StepKind::kSkip: {
      // Values are removed where the stream is produced.
      return absl::OkStatus();
    }
    case StepKind::kCapture: {
      ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(one.source));
      ABSL_ASSIGN_OR_RETURN(const ItemPtr item, reader->Next());
      Value value;
      if (item != nullptr) {
        ABSL_ASSIGN_OR_RETURN(value, item->Read(&bridge()));
      }
      reader->Stop();
      if (one.slot == "$result" && expression_result_ != nullptr) {
        expression_result_->SetValue(value).IgnoreError();
      }
      thread::MutexLock lock(&monitor().mu());
      captures_.insert_or_assign(one.slot, value);
      return absl::OkStatus();
    }
    case StepKind::kWait:
    case StepKind::kDrain:
      return RunWait(step);
    case StepKind::kCancel: {
      if (one.target == kNone) {
        return absl::OkStatus();
      }
      ABSL_ASSIGN_OR_RETURN(CallHandle * handle, Call(one.target));
      ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<actions::Action> action,
                            handle->Action());
      return action->Cancel();
    }
    case StepKind::kFail:
      return Failure(step);
    case StepKind::kAbort:
      return AbortNode(step);
    case StepKind::kLog:
      return WriteLog(one.log, nullptr);
    case StepKind::kBlock: {
      // The same nesting an `if` branch runs in. What a block adds is that its
      // outcome is *its own*: a failure inside it is the block's, and a `try`
      // says the flow means to read it rather than end there.
      if (one.bodies.empty()) {
        return absl::OkStatus();
      }
      const absl::Status ran =
          Scope(*runner_, one.bodies.front(), this, {}).Run();
      Record(step, ran);
      return one.tolerant ? absl::OkStatus() : ran;
    }
    case StepKind::kIf: {
      ABSL_ASSIGN_OR_RETURN(const Value taken, Evaluate(one.condition));
      const BodyId body =
          Truthy(taken) ? one.bodies.front() : one.bodies.back();
      if (graph().bodies[body].steps.empty()) {
        return absl::OkStatus();
      }
      return Scope(*runner_, body, this, {}).Run();
    }
    case StepKind::kForEach:
    case StepKind::kRepeat: {
      // Recorded so a name bound to the loop can read it, exactly as a block's
      // is. OK when every pass succeeded, which is what `group.Join()` already
      // answers -- including for a loop its `until` ended early.
      const absl::Status ran =
          one.kind == StepKind::kForEach ? RunForEach(step) : RunRepeat(step);
      Record(step, ran);
      return ran;
    }
  }
  return Fail(
      absl::StrCat("Cannot run a ", graph::StepKindName(one.kind), "."));
}

absl::Status Scope::RunWaitMany(StepId step) {
  const graph::Step& one = graph().steps[step];
  if (!one.race) {
    absl::Status first;
    for (size_t i = 0; i < one.subjects.size(); ++i) {
      ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(one.subjects[i]));
      ABSL_ASSIGN_OR_RETURN(ItemPtr item, reader->Next());
      if (item == nullptr) {
        continue;
      }
      ABSL_ASSIGN_OR_RETURN(Value record, item->Read(&bridge()));
      const absl::Status status = StatusOfRecord(record);
      if (first.ok()) {
        first = status;
      }
      if (!status.ok() &&
          (i >= one.subject_tolerant.size() || !one.subject_tolerant[i])) {
        return status;
      }
    }
    Record(step, first);
    return absl::OkStatus();
  }

  struct Candidate {
    CallHandle* call = nullptr;
    Scope* scope = nullptr;
    StepId step = kNone;
    RefId node = kNone;
  };

  struct Observed {
    std::shared_ptr<Monitor> monitor;
    std::vector<std::optional<absl::Status>> status;
    std::vector<uint64_t> order;
  };

  struct Subscriptions {
    std::vector<a11::Task> tasks;

    ~Subscriptions() {
      for (const auto& task : tasks) {
        task.Cancel().IgnoreError();
      }
    }
  } subscriptions;

  auto observed = std::make_shared<Observed>();
  observed->monitor = runner_->shared_monitor();
  observed->status.resize(one.subjects.size());
  observed->order.resize(one.subjects.size());
  std::vector<Candidate> candidates;
  for (const RefId subject : one.subjects) {
    const graph::Ref& status = graph().refs[subject];
    Candidate candidate;
    candidate.step = status.subject_step;
    candidate.node = status.subject;
    if (candidate.step != kNone) {
      for (Scope* scope = this; scope != nullptr; scope = scope->parent_) {
        if (scope->body_ != graph().steps[candidate.step].body) {
          continue;
        }
        candidate.scope = scope;
        ABSL_RETURN_IF_ERROR(scope->EnsureStep(candidate.step));
        if (graph().steps[candidate.step].kind == StepKind::kCall) {
          ABSL_ASSIGN_OR_RETURN(candidate.call, scope->Call(candidate.step));
        }
        break;
      }
      if (candidate.scope == nullptr) {
        return Fail("Completion subject is outside its execution scope.");
      }
    } else if (candidate.node != kNone) {
      const graph::Ref& node = graph().refs[candidate.node];
      if (node.kind == RefKind::kCallPort) {
        ABSL_ASSIGN_OR_RETURN(CallHandle * handle, Call(node.call));
        (void)handle;
      }
    } else {
      return Fail("Race subject has no completion identity.");
    }
    candidates.push_back(candidate);
  }
  if (candidates.empty()) {
    return Fail("A race needs at least one subject.");
  }
  absl::flat_hash_map<nodes::AsyncNode*,
                      std::pair<NodePtr, std::vector<size_t>>>
      node_subjects;
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (candidates[i].node == kNone) {
      continue;
    }
    ABSL_ASSIGN_OR_RETURN(NodePtr node, ReadableNode(candidates[i].node));
    if (node == nullptr) {
      ABSL_ASSIGN_OR_RETURN(node, DestinationNode(candidates[i].node));
    }
    if (node == nullptr) {
      return Fail("Race subject is not a node-backed stream.");
    }
    auto& entry = node_subjects[node.get()];
    entry.first = node;
    entry.second.push_back(i);
  }
  for (const auto& [identity, entry] : node_subjects) {
    auto completion = entry.first->ObserveCompletion();
    subscriptions.tasks.push_back(completion);
    completion.OnReady([observed, indices = entry.second](
                           const absl::StatusOr<a11::Unit>& result) {
      {
        thread::MutexLock lock(&observed->monitor->mu());
        const auto order = observed->monitor->CompletionOrder();
        for (const auto i : indices) {
          observed->status[i] = result.status();
          observed->order[i] = order;
        }
      }
      observed->monitor->Wake();
    });
  }
  size_t winner = 0;
  {
    thread::MutexLock lock(&monitor().mu());
    bool initial = true;
    const auto ready = [&] {
      uint64_t earliest = std::numeric_limits<uint64_t>::max();
      bool found = false;
      for (size_t i = 0; i < candidates.size(); ++i) {
        const Candidate& candidate = candidates[i];
        const bool terminal = candidate.call != nullptr
                                  ? candidate.call->finished()
                              : candidate.scope != nullptr
                                  ? candidate.scope->done_.at(candidate.step)
                                  : observed->status[i].has_value();
        if (!terminal) {
          continue;
        }
        const uint64_t order =
            candidate.call != nullptr ? candidate.call->completion_order()
            : candidate.scope != nullptr
                ? candidate.scope->completion_order_.at(candidate.step)
                : observed->order[i];
        if (!found || (!initial && order < earliest)) {
          winner = i;
          earliest = order;
          found = true;
        }
        if (initial) {
          break;
        }
      }
      initial = false;
      return found;
    };
    if (one.timeout.has_value()) {
      ABSL_RETURN_IF_ERROR(
          monitor().WaitUntil(absl::Now() + *one.timeout, ready));
    } else {
      ABSL_RETURN_IF_ERROR(monitor().Wait(ready));
    }
  }
  absl::Status status;
  const Candidate& chosen = candidates[winner];
  if (chosen.call != nullptr) {
    ABSL_RETURN_IF_ERROR(chosen.call->Outcome(&status));
  } else {
    thread::MutexLock lock(&monitor().mu());
    if (chosen.scope != nullptr) {
      const auto found = chosen.scope->outcomes_.find(chosen.step);
      if (found != chosen.scope->outcomes_.end()) {
        status = found->second;
      }
    } else {
      status = *observed->status[winner];
    }
  }
  {
    thread::MutexLock lock(&monitor().mu());
    winners_.insert_or_assign(step, static_cast<int64_t>(winner));
    outcomes_.insert_or_assign(step, status);
  }
  const bool tolerated =
      winner < one.subject_tolerant.size() && one.subject_tolerant[winner];
  return tolerated ? absl::OkStatus() : status;
}

/// Read the subject's status, and let a bad one through when it is ours.
///
/// A subject a flow said it would handle -- a `try` step -- reports and the
/// flow carries on. Anything else that finished badly ends the flow here, with
/// the status it finished with rather than a new one.
absl::Status Scope::RunWait(StepId step) {
  const graph::Step& one = graph().steps[step];
  if (!one.subjects.empty()) {
    return RunWaitMany(step);
  }
  Value record;
  if (one.timeout.has_value() && *one.timeout < absl::InfiniteDuration()) {
    // Reading the outcome blocks on whatever the subject is doing, so a timeout
    // is a race against that read rather than something the read itself knows
    // about.
    struct Waiting {
      Monitor* absl_nonnull monitor;
      bool ready = false;
      absl::Status status = {};
      Value value = {};
    };

    Monitor& monitor = this->monitor();
    auto waiting = std::make_shared<Waiting>(Waiting{.monitor = &monitor});
    const RefId outcome = one.outcome;
    Group reading(monitor);
    reading.Spawn([this, outcome, waiting]() -> absl::Status {
      absl::Status status;
      Value value;
      absl::StatusOr<ReaderPtr> reader = Subscribe(outcome);
      if (!reader.ok()) {
        status = reader.status();
      } else if (absl::StatusOr<ItemPtr> item = (*reader)->Next(); !item.ok()) {
        status = item.status();
      } else if (*item != nullptr) {
        absl::StatusOr<Value> read = (*item)->Read(&this->bridge());
        if (read.ok()) {
          value = *std::move(read);
        } else {
          status = read.status();
        }
      }
      {
        thread::MutexLock lock(&waiting->monitor->mu());
        waiting->status = status;
        waiting->value = std::move(value);
        waiting->ready = true;
      }
      waiting->monitor->Wake();
      return absl::OkStatus();
    });
    absl::Status waited;
    {
      thread::MutexLock lock(&monitor.mu());
      waited = monitor.WaitUntil(absl::Now() + *one.timeout,
                                 [waiting] { return waiting->ready; });
      if (waited.ok()) {
        ABSL_RETURN_IF_ERROR(waiting->status);
        record = waiting->value;
      }
    }
    if (absl::IsDeadlineExceeded(waited)) {
      // The group's destructor ends the read on the way out.
      return Fail(absl::StrCat("Waiting for ", SubjectOf(one),
                               " timed out "
                               "after ",
                               absl::FormatDuration(*one.timeout), "."),
                  absl::StatusCode::kDeadlineExceeded);
    }
    ABSL_RETURN_IF_ERROR(waited);
    ABSL_RETURN_IF_ERROR(reading.Join());
  } else {
    ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(one.outcome));
    ABSL_ASSIGN_OR_RETURN(const ItemPtr item, reader->Next());
    if (item != nullptr) {
      ABSL_ASSIGN_OR_RETURN(record, item->Read(&bridge()));
    }
  }
  if (record.kind() == Value::Kind::kObject) {
    Record(step, StatusOfRecord(record));
  }
  if (one.tolerant || record.kind() != Value::Kind::kObject) {
    return absl::OkStatus();
  }
  const Value* ok = record.Get("ok");
  if (ok != nullptr && Truthy(*ok)) {
    return absl::OkStatus();
  }
  return StatusOfRecord(record);
}

/// The status a `fail` statement raises.
absl::Status Scope::Failure(StepId step) {
  ABSL_ASSIGN_OR_RETURN(const absl::Status chosen, ChosenStatus(step));
  return chosen;
}

/// The status a `fail` or an `abort` names, evaluated.
///
/// StatusOr of a Status, and the nesting is the point: the outer one is whether
/// the *expressions* could be read, and the inner one is what the flow asked
/// for. A `fail` returns the inner one as its own outcome; an `abort` hands it
/// to a node.
absl::StatusOr<absl::Status> Scope::ChosenStatus(StepId step) {
  const graph::Step& one = graph().steps[step];
  Value code;
  if (!one.code_name.empty()) {
    code = Value::String(one.code_name);
  } else {
    ABSL_ASSIGN_OR_RETURN(code, Evaluate(one.code));
  }
  Value message;
  bool has_message = one.message != kNone;
  if (has_message) {
    ABSL_ASSIGN_OR_RETURN(message, Evaluate(one.message));
  }
  if (!has_message && code.kind() == Value::Kind::kObject) {
    // `fail check` -- raise the status the flow recovered from.
    return StatusOfRecord(code);
  }
  if (!has_message && !StatusCodeOf(code).has_value()) {
    message = code;
    has_message = true;
    code = Value::Null();
  }
  absl::StatusCode resolved =
      StatusCodeOf(code).value_or(absl::StatusCode::kInternal);
  if (resolved == absl::StatusCode::kOk) {
    resolved = absl::StatusCode::kInternal;
  }
  const std::string text = has_message ? AsText(message) : "";
  return Fail(
      text.empty() ? absl::StrCat(runner_->plan().name, " failed.") : text,
      resolved);
}

absl::Status Scope::AbortNode(StepId step) {
  const graph::Step& one = graph().steps[step];
  ABSL_ASSIGN_OR_RETURN(const absl::Status reason, ChosenStatus(step));
  ABSL_ASSIGN_OR_RETURN(Destination * destination,
                        DestinationOf(one.destination));
  return destination->Abort(reason);
}

/// Write one entry to the flow's own log.
///
/// `subject` is the value a stage is looking at, and null in a statement -- the
/// only difference between the two, since `it` means nothing where there is no
/// value in hand.
///
/// Logged values retain their representation. `logf` produces formatted text;
/// `log` preserves records and chunk bytes
/// with their mimetype. Consumers choose
/// how to render the value.
///
/// A failure to log is the flow's, not the log's: writing is what
/// actions::Action::Log already declines to fail on, so what can go wrong here
/// is only evaluating what was written, and that is a mistake in the flow.
absl::Status Scope::WriteLog(const graph::LogTail& tail,
                             const ItemPtr& subject) {
  std::optional<Value> in_hand;
  if (subject != nullptr) {
    ABSL_ASSIGN_OR_RETURN(in_hand, subject->Read(&bridge()));
  }
  const auto evaluate = [&](ExprId expr) -> absl::StatusOr<Value> {
    if (in_hand.has_value()) {
      return EvaluateWith(expr, *in_hand);
    }
    return Evaluate(expr);
  };

  actions::LogOptions options;
  options.level = tail.level;
  // The flow's name rather than the step's: a consumer filtering logs wants the
  // flow they came from, and the line already says which statement it was.
  options.channel = runner_->plan().name;
  if (tail.line > 0) {
    options.lineno = tail.line;
  }

  if (tail.has_format) {
    std::vector<Value> arguments;
    arguments.reserve(tail.arguments.size());
    for (const ExprId expr : tail.arguments) {
      ABSL_ASSIGN_OR_RETURN(Value value, evaluate(expr));
      arguments.push_back(std::move(value));
    }
    return Logged(runner_->action()->Log(
        Strformat(Value::String(tail.format), arguments), options));
  }

  if (!tail.arguments.empty()) {
    ABSL_ASSIGN_OR_RETURN(const Value value, evaluate(tail.arguments.front()));
    return Logged(LogValue(value, /*carried=*/nullptr, options));
  }

  // `| log` with nothing written logs the value going past, which is what the
  // stage is for.
  if (subject != nullptr) {
    return Logged(LogValue(*in_hand, subject.get(), options));
  }
  return absl::OkStatus();
}

/// Log one value as the value it is.
///
/// The one rule, and the same one every language's `log` follows: a string is
/// text, and everything else keeps its own representation. `carried` is the
/// item the value was read out of, where there is one -- a chunk that arrived
/// on a port is logged as the bytes and mimetype it arrived with rather than
/// decoded and re-encoded, so audio stays audio and msgpack stays msgpack.
absl::Status Scope::LogValue(const Value& value,
                             const Item* absl_nullable carried,
                             const actions::LogOptions& options) {
  const std::shared_ptr<actions::Action>& action = runner_->action();
  if (value.kind() == Value::Kind::kString) {
    // Built here rather than asked of the host: a host is entitled to answer a
    // request for `text/plain` with the JSON spelling of the string the native
    // bridge does and a logged string is the characters, not a quoted.
    data::Chunk chunk;
    chunk.metadata =
        data::ChunkMetadata{.mimetype = std::string(data::kTextMimetype)};
    chunk.data = value.text();
    return action->Log(std::move(chunk), options);
  }
  // A chunk the flow never opened: log the producer's own bytes and mimetype.
  if (value.kind() == Value::Kind::kChunk &&
      !data::IsStatusChunk(value.chunk())) {
    return action->Log(value.chunk(), options);
  }
  if (carried != nullptr && carried->chunk().has_value() &&
      !data::IsStatusChunk(*carried->chunk())) {
    return action->Log(*carried->chunk(), options);
  }
  ABSL_ASSIGN_OR_RETURN(const data::Chunk chunk,
                        bridge().ToChunk(value, /*mimetype=*/{}));
  return action->Log(chunk, options);
}

/// Turn a refused log into the flow's failure, and anything else into nothing.
absl::Status Scope::Logged(const absl::Status& logged) {
  if (logged.ok()) {
    return absl::OkStatus();
  }
  return Fail(absl::StrCat("This log could not be written: ", logged.message()),
              logged.code());
}

/// Remember how a nested body went, for a name bound to it to read.
///
/// A block and a loop both have an outcome of their own rather than a call's,
/// so both record it here and `StatusItem` reads it back after `StepDone`. One
/// helper because the two must agree: a reader waiting on the wake this does
/// is waiting on the same condition either way.
void Scope::Record(StepId step, const absl::Status& outcome) {
  {
    thread::MutexLock lock(&monitor().mu());
    outcomes_[step] = outcome;
  }
  monitor().Wake();
}

/// Whether a loop's `until`/`while` condition holds of the pass that just ran.
///
/// One implementation for `repeat` and `for`, because the condition means the
/// same thing to both: it is asked at the *tail* of a pass, so the body always
/// runs at least once however the condition starts out. Each stream the
/// condition reads was captured inside the pass -- the streams themselves are
/// gone by the time the question is asked -- which is what the `condition:`
/// slots hold.
absl::StatusOr<bool> Scope::PassCondition(const graph::Step& one,
                                          const Scope& pass) {
  const graph::Expr& condition = graph().exprs[one.condition];
  absl::flat_hash_map<const syntax::Node*, Value> bound;
  for (const auto& [node, ref] : condition.bound) {
    const auto found = pass.captures().find(absl::StrCat("condition:", ref));
    bound[node] =
        found == pass.captures().end() ? Value::Null() : found->second;
  }
  EvalContext context;
  context.bound = &bound;
  context.bridge = &bridge();
  context.shapes = &shapes();
  ABSL_ASSIGN_OR_RETURN(const Value holds,
                        flow::Evaluate(*condition.node, context));
  return Truthy(holds);
}

absl::Status Scope::RunForEach(StepId step) {
  const graph::Step& one = graph().steps[step];
  const BodyId body = one.bodies.front();
  ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(one.source));
  Group group(monitor(), work_);
  const int parallel = std::max(one.parallel, 1);
  std::int64_t index = 0;
  Monitor& monitor = this->monitor();
  auto running = std::make_shared<int>(0);
  while (true) {
    ABSL_ASSIGN_OR_RETURN(const ItemPtr item, reader->Next());
    if (item == nullptr) {
      break;
    }
    absl::flat_hash_map<RefId, std::vector<ItemPtr>> presets;
    if (one.item != kNone) {
      presets[one.item] = {item};
    }
    if (one.index != kNone) {
      presets[one.index] = {Item::Of(Value::Integer(index))};
    }
    if (index == std::numeric_limits<int64_t>::max()) {
      return absl::ResourceExhaustedError("Flow loop index exhausted.");
    }
    ++index;
    if (parallel == 1) {
      // Named rather than a temporary, because an `until` reads what the pass
      // captured and a temporary's captures are gone by the semicolon.
      Scope pass(*runner_, body, this, std::move(presets));
      ABSL_RETURN_IF_ERROR(pass.Run());
      if (one.condition != kNone) {
        ABSL_ASSIGN_OR_RETURN(const bool holds, PassCondition(one, pass));
        if (holds == one.stop_when) {
          // Stop reading, as `| first n` does. It does not cancel whatever is
          // producing -- see Reader::Stop for why that is deliberate -- so a
          // step with more to say still finishes on its own terms.
          reader->Stop();
          break;
        }
      }
      continue;
    }
    // Admission before the fiber, so a wide `parallel` does not turn a long
    // stream into a pile of pending passes.
    {
      thread::MutexLock lock(&monitor.mu());
      ABSL_RETURN_IF_ERROR(
          monitor.Wait([running, parallel] { return *running < parallel; }));
      ++*running;
    }
    group.Spawn([this, body, presets = std::move(presets), running,
                 &monitor]() mutable -> absl::Status {
      const absl::Status status =
          Scope(*runner_, body, this, std::move(presets)).Run();
      {
        thread::MutexLock lock(&monitor.mu());
        --*running;
      }
      monitor.Wake();
      return status;
    });
  }
  return group.Join();
}

absl::Status Scope::RunRepeat(StepId step) {
  const graph::Step& one = graph().steps[step];
  const BodyId body = one.bodies.front();
  Value carried = Value::Of(one.start);
  // Bounded by the author's `max` where there is one, and otherwise only by the
  // condition: a loop that says `until` means it, however many passes that
  // takes.
  for (int64_t index = 0;
       !one.max_iterations.has_value() || index < *one.max_iterations;
       ++index) {
    absl::flat_hash_map<RefId, std::vector<ItemPtr>> presets;
    if (one.carry != kNone) {
      presets[one.carry] = {Item::Of(carried)};
    }
    if (one.index != kNone) {
      presets[one.index] = {Item::Of(Value::Integer(index))};
    }
    Scope pass(*runner_, body, this, std::move(presets));
    ABSL_RETURN_IF_ERROR(pass.Run());
    if (one.condition != kNone) {
      ABSL_ASSIGN_OR_RETURN(const bool holds, PassCondition(one, pass));
      if (holds == one.stop_when) {
        return absl::OkStatus();
      }
    }
    if (one.carry_source != kNone) {
      const auto found = pass.captures().find("carry");
      carried = found == pass.captures().end() ? Value::Null() : found->second;
    }
    if (index == std::numeric_limits<int64_t>::max()) {
      return absl::ResourceExhaustedError("Flow repeat index exhausted.");
    }
  }
  return absl::OkStatus();
}

// --- Calls -------------------------------------------------------------------

absl::Status Runner::Resolve(const std::string& name,
                             actions::ActionSchema* schema,
                             actions::ActionHandler* handler) {
  if (const auto found = bindings_.find(name); found != bindings_.end()) {
    *schema = found->second.first;
    *handler = found->second.second;
    return absl::OkStatus();
  }
  if (const ResolvedFlow* sibling = program_->Flow(name);
      sibling != nullptr && !sibling->plan.interface_only) {
    if (sibling->ambiguous) {
      return absl::FailedPreconditionError(
          absl::StrCat("Ambiguous implementation of '", name, "'."));
    }
    ABSL_ASSIGN_OR_RETURN(
        *schema, FlowSchema(sibling->implementation.value_or(sibling->plan),
                            &program_->program()));
    // A flow of this program that this flow runs is still the same client's, so
    // its own `call` steps belong on the same stream.
    ABSL_ASSIGN_OR_RETURN(
        *handler, MakeHandler(program_, name,
                              RunOptions{.snapshot = snapshot_,
                                         .bridge = bridge_,
                                         .dispatch_stream = dispatch_stream_}));
    bindings_.emplace(name, std::make_pair(*schema, *handler));
    return absl::OkStatus();
  }
  const auto* declaration = program_->Flow(name);
  const std::string wire_name =
      declaration != nullptr && !declaration->plan.binding_name.empty()
          ? declaration->plan.binding_name
          : name;
  if (const auto found = snapshot_->external.find(wire_name);
      found != snapshot_->external.end()) {
    *schema = found->second.first;
    *handler = found->second.second;
    return absl::OkStatus();
  }
  if (snapshot_->sealed) {
    return absl::FailedPreconditionError(
        "Binding is absent from the execution snapshot.");
  }
  const std::shared_ptr<actions::ActionRegistry> registry =
      action_->GetRegistry();
  if (registry == nullptr || !registry->IsRegistered(wire_name)) {
    std::vector<std::string> known = registry == nullptr
                                         ? std::vector<std::string>{}
                                         : registry->ListRegisteredActions();
    std::sort(known.begin(), known.end());
    return Fail(
        absl::StrCat(plan().name, " calls '", name,
                     "', which is not a flow in this program and is not "
                     "registered here (registered: ",
                     known.empty() ? "none" : absl::StrJoin(known, ", "), ")."),
        program_->Flow(name) != nullptr ? absl::StatusCode::kFailedPrecondition
                                        : absl::StatusCode::kNotFound);
  }
  ABSL_ASSIGN_OR_RETURN(*schema, registry->GetSchema(wire_name));
  // Registered without one: the registry reports that as an error rather than
  // as an empty handler, so asking is how it is found out.
  if (absl::StatusOr<actions::ActionHandler> found =
          registry->GetHandler(wire_name);
      found.ok()) {
    *handler = *std::move(found);
  }
  snapshot_->external.emplace(wire_name, std::make_pair(*schema, *handler));
  return absl::OkStatus();
}

absl::Status Runner::Preflight() {
  std::vector<const ResolvedFlow*> pending{flow_};
  absl::flat_hash_set<std::string> visited;
  while (!pending.empty()) {
    const ResolvedFlow* current = pending.back();
    pending.pop_back();
    if (!visited.insert(current->plan.name).second) {
      continue;
    }
    if (current->ambiguous || current->plan.interface_only) {
      return absl::FailedPreconditionError(absl::StrCat(
          "No unambiguous implementation for '", current->plan.name, "'."));
    }
    for (const graph::Step& step : current->graph.steps) {
      if (step.kind != StepKind::kCall) {
        continue;
      }
      actions::ActionSchema schema;
      actions::ActionHandler handler;
      ABSL_RETURN_IF_ERROR(Resolve(step.action, &schema, &handler));
      if (step.mode == "call" && dispatch_stream_ == nullptr &&
          action_->GetStream() == nullptr && action_->GetSession() == nullptr) {
        return absl::FailedPreconditionError(
            absl::StrCat(current->plan.name, " could not call '", step.action,
                         "': calling requires an attached WireStream or "
                         "Session before execution."));
      }
      if (step.mode == "run" && !handler) {
        return absl::FailedPreconditionError(absl::StrCat(
            current->plan.name, " says 'run ", step.action, "', but ",
            step.action,
            " is registered here for its schema alone and has no handler to "
            "run. Say 'call' to dispatch it on the flow's stream."));
      }
      FlowPlan actual;
      Program evidence = program_->program();
      actual.name = step.action;
      for (const auto direction :
           {syntax::PortDirection::kInput, syntax::PortDirection::kOutput}) {
        const auto& ports = direction == syntax::PortDirection::kInput
                                ? schema.inputs
                                : schema.outputs;
        for (const auto& [name, p] : ports) {
          PortPlan port;
          port.name = name;
          port.type = p.type;
          port.declared = p.type;
          port.direction = direction;
          port.unary = p.unary;
          port.required = p.required;
          if (!p.json_schema.empty()) {
            const auto metadata =
                nlohmann::json::parse(p.json_schema, nullptr, false);
            if (metadata.is_object() && metadata.contains("type") &&
                metadata["type"].is_string() && metadata["type"] == "object") {
              auto imported = JsonSchemaToDtos(
                  metadata, absl::StrCat("schema_", evidence.dtos.size()));
              if (imported.diagnostics.empty() && !imported.dtos.empty()) {
                port.declared = imported.dtos.front().name;
                for (auto& shape : imported.dtos) {
                  evidence.dtos.push_back(std::move(shape));
                }
              }
            }
          }
          actual.ports.push_back(std::move(port));
          if (direction != syntax::PortDirection::kInput) {
            continue;
          }
          bool supplied = false;
          const StepId call_id =
              static_cast<StepId>(&step - current->graph.steps.data());
          for (const auto& feed : current->graph.steps) {
            if (feed.kind != StepKind::kPipe || feed.destination == kNone) {
              continue;
            }
            const auto& target = current->graph.refs[feed.destination];
            if (target.kind != RefKind::kCallPort || target.call != call_id ||
                target.direction != syntax::PortDirection::kInput ||
                target.name != name) {
              continue;
            }
            supplied = true;
            if (feed.source == kNone) {
              continue;
            }
            const auto& source = current->graph.refs[feed.source];
            if (source.kind != RefKind::kExpr || source.expr == kNone) {
              continue;
            }
            const auto& expression = current->graph.exprs[source.expr];
            if (expression.node == nullptr) {
              continue;
            }
            const auto constant = syntax::ConstantValue(expression.node);
            if (!constant.has_value() ||
                constant->kind == syntax::Constant::Kind::kNull) {
              continue;
            }
            std::string type;
            switch (constant->kind) {
              case syntax::Constant::Kind::kNull:
                type = "null";
                break;
              case syntax::Constant::Kind::kBool:
                type = "boolean";
                break;
              case syntax::Constant::Kind::kInteger:
                type = "integer";
                break;
              case syntax::Constant::Kind::kDouble:
                type = "number";
                break;
              case syntax::Constant::Kind::kString:
                type = "string";
                break;
              case syntax::Constant::Kind::kDuration:
                type = "duration";
                break;
              case syntax::Constant::Kind::kList:
                type = "list";
                break;
              case syntax::Constant::Kind::kObject:
                type = "object";
                break;
            }
            if (RefinesType(type, p.type, evidence) == Refinement::kNo) {
              return absl::FailedPreconditionError(
                  absl::StrCat("Call '", step.action, "', input '", name,
                               "' has an incompatible literal payload."));
            }
          }
          if (direction == syntax::PortDirection::kInput && p.required &&
              p.autofills.empty() && !supplied) {
            return absl::FailedPreconditionError(
                absl::StrCat("Call '", step.action,
                             "' is missing required input '", name, "'."));
          }
        }
      }
      for (const auto& [name, h] : schema.headers) {
        HeaderPlan header;
        header.name = name;
        header.has_default = h.default_value.has_value();
        if (header.has_default) {
          header.default_value = syntax::Constant::String(*h.default_value);
        }
        actual.headers.push_back(std::move(header));
      }
      auto imported_interface = DeclareAction(schema);
      if (imported_interface.diagnostics.empty()) {
        actual = std::move(imported_interface.interface);
        for (auto& shape : imported_interface.types) {
          evidence.dtos.push_back(std::move(shape));
        }
      }
      if (const auto* nested = program_->Flow(step.action);
          nested != nullptr && nested->implementation.has_value()) {
        actual = *nested->implementation;
      }
      if (const auto* declaration = program_->Flow(step.action);
          declaration != nullptr) {
        for (const auto& expected : declaration->contracts) {
          const auto checked = CheckImplementation(actual, expected, evidence);
          if (!checked.ok()) {
            return absl::FailedPreconditionError(
                std::string(checked.message()));
          }
        }
      }
      for (const auto& [key, ref] : step.ports) {
        const graph::Ref& p = current->graph.refs[ref];
        const auto& ports = p.direction == syntax::PortDirection::kInput
                                ? schema.inputs
                                : schema.outputs;
        if (ports.find(p.name) == ports.end()) {
          return absl::NotFoundError(absl::StrCat(
              step.action, " has no ",
              p.direction == syntax::PortDirection::kInput ? "input" : "output",
              " port '", p.name, "'."));
        }
      }
      if (const ResolvedFlow* nested = program_->Flow(step.action);
          nested != nullptr && !nested->plan.interface_only) {
        pending.push_back(nested);
      }
    }
  }
  return absl::OkStatus();
}

absl::Status Runner::RunCall(Scope& scope, StepId step) {
  ABSL_ASSIGN_OR_RETURN(CallHandle * handle, scope.Call(step));
  if (const absl::Status started = StartCall(scope, step, *handle);
      !started.ok()) {
    // A step that never started still has to say so: everything wiring itself
    // to this call waits for it, and an unanswered wait is a deadlock rather
    // than a failure anybody can see.
    handle->NeverStarted(started);
    if (const auto action = handle->action_now(); action != nullptr) {
      action->Cancel().IgnoreError();
    }
    return graph().steps[step].tolerant ? absl::OkStatus() : started;
  }
  return PumpCall(step, *handle);
}

absl::Status Runner::StartCall(Scope& scope, StepId step, CallHandle& handle) {
  ABSL_RETURN_IF_ERROR(scope.work_->CheckActive());
  const graph::Step& one = graph().steps[step];
  if (thread::Cancelled() || action_->Cancelled()) {
    return absl::CancelledError("Flow stopped before action activation");
  }
  actions::ActionSchema schema;
  actions::ActionHandler handler;
  ABSL_RETURN_IF_ERROR(Resolve(one.action, &schema, &handler));
  const bool local = one.mode == "run";
  if (local && handler == nullptr) {
    return Fail(
        absl::StrCat(plan().name, " says 'run ", one.action, "', but ",
                     one.action,
                     " is registered here for its schema alone and has no "
                     "handler to run. Say 'call' to dispatch it on the stream "
                     "this flow is attached to."),
        absl::StatusCode::kFailedPrecondition);
  }

  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<actions::Action> nested,
                        action_->MakeNested(schema));
  handle.Adopt(nested);
  scope.work_->OwnAction(nested);
  if (!one.node_map.empty()) {
    ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<nodes::NodeMap> map,
                          NodeMapNamed(one.node_map));
    ABSL_RETURN_IF_ERROR(nested->BindNodeMap(map));
  }
  if (!local && dispatch_stream_ != nullptr) {
    // A peer call uses the dispatch stream, which the flow action does not
    // hold.
    ABSL_RETURN_IF_ERROR(nested->BindStream(dispatch_stream_));
  }
  if (local) {
    ABSL_RETURN_IF_ERROR(nested->BindHandler(handler));
    if (!one.tee) {
      // Keep a local step's streams off the wire: the peer that dispatched this
      // flow asked for its outputs, not for every intermediate node inside it.
      ABSL_RETURN_IF_ERROR(nested->BindStream(nullptr));
    }
  }
  // `forward headers` first, so an explicit `with` of the same name -- the more
  // specific of the two -- is the one that lands.
  for (auto& [name, value] : Forwarded(one)) {
    ABSL_RETURN_IF_ERROR(nested->SetHeader(name, value));
  }
  for (const auto& [name, expr] : one.headers) {
    ABSL_ASSIGN_OR_RETURN(const Value value, scope.Evaluate(expr));
    ABSL_RETURN_IF_ERROR(nested->SetHeader(
        name,
        value.kind() == Value::Kind::kBytes ? value.text() : AsText(value)));
  }
  if (one.action_id != kNone) {
    ABSL_ASSIGN_OR_RETURN(const Value value, scope.Evaluate(one.action_id));
    ABSL_RETURN_IF_ERROR(nested->SetId(AsText(value)));
  }

  // Create every port node before the action starts, so a reader that
  // subscribes later still sees the whole stream from its beginning.
  absl::flat_hash_set<std::string> written;
  for (const RefId ref : scope.analysis_->destinations) {
    const graph::Ref& port = graph().refs[ref];
    if (port.kind == RefKind::kCallPort && port.call == step) {
      written.insert(port.name);
    }
  }
  absl::flat_hash_set<std::string> read;
  for (const RefId ref : scope.analysis_->refs) {
    const graph::Ref& port = graph().refs[ref];
    if (port.kind != RefKind::kCallPort || port.call != step) {
      continue;
    }
    if (port.direction != syntax::PortDirection::kOutput) {
      continue;
    }
    const auto readers = scope.analysis_->readers.find(ref);
    if (readers != scope.analysis_->readers.end() && readers->second > 0) {
      read.insert(port.name);
    }
  }
  std::vector<NodePtr> unclosed;
  for (const auto& [name, declared] : schema.inputs) {
    ABSL_ASSIGN_OR_RETURN(const NodePtr node,
                          nested->GetInput(name, std::nullopt));
    if (!written.contains(name) && declared.autofills.empty()) {
      unclosed.push_back(node);
    }
  }
  std::vector<NodePtr> undrained;
  for (const auto& [name, declared] : schema.outputs) {
    ABSL_ASSIGN_OR_RETURN(const NodePtr node,
                          nested->GetOutput(name, std::nullopt));
    if (!read.contains(name)) {
      undrained.push_back(node);
    }
  }
  handle.unclosed = std::move(unclosed);
  handle.undrained = std::move(undrained);
  handle.Started(nested);
  if (thread::Cancelled() || action_->Cancelled()) {
    nested->Cancel().IgnoreError();
    return absl::CancelledError("Flow stopped during action activation");
  }
  return absl::OkStatus();
}

absl::Status Runner::PumpCall(StepId step, CallHandle& handle) {
  const graph::Step& one = graph().steps[step];
  const std::shared_ptr<actions::Action> nested = handle.action_now();
  absl::Status start;
  if (one.mode == "run") {
    start = nested->Run().status();
  } else if (const absl::Status called = nested->Call().Await().status();
             !called.ok()) {
    start = Fail(absl::StrCat(plan().name, " could not call '", one.action,
                              "' on its session: ", called.message()),
                 called.code());
  }
  if (!start.ok()) {
    handle.NeverStarted(start);
    nested->Cancel().IgnoreError();
    return one.tolerant ? absl::OkStatus() : start;
  }
  {
    Group group(*monitor_);
    for (const NodePtr& node : handle.unclosed) {
      group.Spawn([node]() -> absl::Status { return CloseNode(node); });
    }
    const bool tolerant = one.tolerant;
    for (const NodePtr& node : handle.undrained) {
      group.Spawn([node, tolerant]() -> absl::Status {
        while (true) {
          absl::StatusOr<std::optional<data::NodeFragment>> fragment =
              node->NextFragment().Await();
          if (!fragment.ok()) {
            return tolerant ? absl::OkStatus() : fragment.status();
          }
          if (!fragment->has_value()) {
            return absl::OkStatus();
          }
        }
      });
    }
    group.Spawn([this, step, &handle]() -> absl::Status {
      return AwaitCall(graph().steps[step], handle);
    });
    const absl::Status status = group.Join();
    handle.Done();
    if (!status.ok()) {
      return status;
    }
  }
  if (const absl::Status error = handle.error(); !error.ok() && !one.tolerant) {
    return error;
  }
  return absl::OkStatus();
}

absl::Status Runner::AwaitCall(const graph::Step& step, CallHandle& handle) {
  const std::shared_ptr<actions::Action> action = handle.action_now();
  if (action == nullptr) {
    return absl::OkStatus();
  }
  const absl::Status status =
      action->Wait(step.timeout.value_or(absl::InfiniteDuration()))
          .Await()
          .status();
  if (status.ok()) {
    return absl::OkStatus();
  }
  if (absl::IsCancelled(status) && action->Cancelled() &&
      !action_->Cancelled()) {
    // A `cancel` statement cancels the call, which cancels the wait for it.
    // That is this call's outcome, not the flow being cancelled -- unless the
    // flow really is going away too.
    handle.SetError(action->GetStatus());
    return absl::OkStatus();
  }
  handle.SetError(status);
  return absl::OkStatus();
}

}  // namespace

// --- The public face ---------------------------------------------------------

absl::StatusOr<std::shared_ptr<CompiledProgram>> CompiledProgram::Compile(
    std::string source, std::string source_name, const ModuleSources& modules,
    ModuleResolver resolver) {
  auto compiled = std::shared_ptr<CompiledProgram>(new CompiledProgram());
  compiled->source_ = std::move(source);
  compiled->source_name_ = std::move(source_name);
  LinkedModule linked = LinkModules(compiled->source_, compiled->source_name_,
                                    modules, std::move(resolver));
  compiled->linked_source_ = linked.source;
  compiled->members_ = linked.members;
  compiled->parsed_ = std::move(linked.parsed);
  compiled->resolved_ = Resolve(compiled->linked_source_, compiled->parsed_,
                                /*build_graph=*/true);
  linked.Locate(compiled->resolved_.diagnostics);
  compiled->resolved_.program.source_name = compiled->source_name_;
  if (const Diagnostic* first = compiled->resolved_.FirstError();
      first != nullptr) {
    return Fail(absl::StrCat(!first->source.empty() ? first->source
                             : compiled->source_name_.empty()
                                 ? "<flow>"
                                 : compiled->source_name_,
                             ":", first->range.start.line, ":",
                             first->range.start.column, ": ", first->message));
  }
  return compiled;
}

const ResolvedFlow* absl_nullable CompiledProgram::Flow(
    std::string_view name) const {
  // As Program::Flow does: an empty name reaches nothing, so the entry flow is
  // not addressable by a `run` or a `call`.
  if (name.empty()) {
    return nullptr;
  }
  std::string target(name);
  for (const auto& member : members_) {
    if (!member.selection && !member.scoped && !member.type &&
        member.name == name) {
      target = member.target;
      break;
    }
  }
  for (const ResolvedFlow& flow : resolved_.flows) {
    if (!flow.plan.entry && flow.plan.name == target) {
      return &flow;
    }
  }
  return nullptr;
}

const ResolvedFlow* absl_nullable CompiledProgram::Entry() const {
  for (const ResolvedFlow& flow : resolved_.flows) {
    if (flow.plan.entry) {
      return &flow;
    }
  }
  return nullptr;
}

namespace {

/// What a port's declared type is called in an [actions::ActionSchema].
///
/// A schema's `type` is the host-visible payload name. Built-in types use their
/// Python names so generated Flow schemas match hand-written action schemas.
/// Tags and mimetypes are preserved as written.
std::string SchemaType(std::string_view declared) {
  static const auto* const kNames =
      new absl::flat_hash_map<std::string_view, std::string_view>{
          {"string", "str"},
          {"text", "str"},
          {"number", "float"},
          {"integer", "int"},
          {"int", "int"},
          {"bool", "bool"},
          {"boolean", "bool"},
          {"object", "dict"},
          {"json", "dict"},
          {"list", "list"},
          {"array", "list"},
          {"bytes", "bytes"},
          {"duration", "a11.Duration"},
          {"time", "a11.Time"},
          {"any", "application/json"},
      };
  const auto found = kNames->find(declared);
  return std::string(found == kNames->end() ? declared : found->second);
}

}  // namespace

absl::StatusOr<actions::ActionSchema> FlowSchema(const FlowPlan& plan,
                                                 const Program* shapes) {
  actions::ActionSchema schema;
  schema.name = plan.binding_name.empty() ? plan.name : plan.binding_name;
  schema.description = plan.description;
  for (const PortPlan& port : plan.ports) {
    actions::ActionPortSchema entry;
    entry.name = port.name;
    entry.type = SchemaType(port.type);
    entry.description = port.description;
    entry.required = port.required;
    entry.unary = port.unary;
    if (shapes != nullptr) {
      if (const auto* shape = shapes->Dto(port.type); shape != nullptr) {
        entry.json_schema = DtoToJsonSchema(*shape, *shapes).dump();
      }
    }
    if (port.direction == syntax::PortDirection::kInput) {
      schema.inputs.emplace(port.name, std::move(entry));
    } else {
      schema.outputs.emplace(port.name, std::move(entry));
    }
  }
  for (const HeaderPlan& header : plan.headers) {
    actions::ActionHeaderSchema entry;
    entry.name = header.name;
    entry.description = header.description;
    if (header.has_default) {
      entry.default_value = HeaderBytes(header.default_value);
    }
    schema.headers.emplace(header.name, std::move(entry));
  }
  ABSL_RETURN_IF_ERROR(schema.Validate());
  return schema;
}

ActionDeclaration DeclareAction(const actions::ActionSchema& schema) {
  ActionDeclaration result;
  result.interface.name = schema.name;
  result.interface.description = schema.description;
  result.interface.interface_only = true;
  const auto reject = [&](std::string message) {
    Diagnostic diagnostic;
    diagnostic.code = "flow.schema.unsupported-keyword";
    diagnostic.family = Family::kForm;
    diagnostic.severity = Severity::kError;
    diagnostic.message = std::move(message);
    result.diagnostics.push_back(std::move(diagnostic));
  };
  if (const auto status = schema.Validate(); !status.ok()) {
    reject(std::string(status.message()));
    return result;
  }
  if (!utf8::IsValid(schema.name) || !utf8::IsValid(schema.description)) {
    reject(
        "An action declaration requires valid UTF-8 names and descriptions.");
    return result;
  }
  for (const auto* ports : {&schema.inputs, &schema.outputs}) {
    for (const auto& [name, port] : *ports) {
      if (!utf8::IsValid(name) || !utf8::IsValid(port.type) ||
          !utf8::IsValid(port.description)) {
        reject("Action port metadata requires valid UTF-8 text.");
        return result;
      }
    }
  }
  for (const auto& [name, header] : schema.headers) {
    if (!utf8::IsValid(name) || !utf8::IsValid(header.description) ||
        (header.default_value.has_value() &&
         !utf8::IsValid(*header.default_value))) {
      reject("A header has no lossless UTF-8 Flow spelling.");
      return result;
    }
  }
  const auto encode = [](std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string name;
    for (const char character : value) {
      const auto byte = static_cast<unsigned char>(character);
      name += hex[byte >> 4];
      name += hex[byte & 15];
    }
    return name;
  };

  for (const auto direction :
       {syntax::PortDirection::kInput, syntax::PortDirection::kOutput}) {
    const auto& ports = direction == syntax::PortDirection::kInput
                            ? schema.inputs
                            : schema.outputs;
    std::vector<std::string> names;
    for (const auto& [name, port] : ports) {
      names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
      const auto& port = ports.at(name);
      PortPlan entry;
      entry.name = name;
      entry.direction = direction;
      entry.unary = port.unary;
      entry.required = port.required;
      entry.description = port.description;
      entry.type = port.type;
      entry.declared = port.type;
      if (port.type == "str") {
        entry.declared = "string";
      } else if (port.type == "dict") {
        entry.declared = "object";
      } else if (port.type == "float") {
        entry.declared = "number";
      } else if (port.type == "a11.Time") {
        entry.declared = "time";
      } else if (port.type == "a11.Duration") {
        entry.declared = "duration";
      } else if (port.type.find('/') != std::string::npos) {
        entry.declared = nlohmann::json(port.type).dump();
      }
      if (!port.json_schema.empty()) {
        const auto metadata =
            nlohmann::json::parse(port.json_schema, nullptr, false);
        if (metadata.is_object() && metadata.contains("type") &&
            metadata["type"].is_string() && metadata["type"] == "object" &&
            metadata.contains("properties")) {
          const auto representable = [&](const auto& self,
                                         const nlohmann::json& value,
                                         bool record, int depth) -> bool {
            if (depth > 128 || !value.is_object()) {
              return false;
            }
            for (const auto& item : value.items()) {
              const auto& key = item.key();
              if (key == "title" || key == "description" || key == "$schema" ||
                  key == "x-a11-type" || key == "x-a11-order") {
                continue;
              }
              if (key == "type") {
                if (!item.value().is_string()) {
                  return false;
                }
              } else if (record && key == "properties") {
                if (!item.value().is_object()) {
                  return false;
                }
                for (const auto& field : item.value()) {
                  if (!self(self, field, false, depth + 1)) {
                    return false;
                  }
                }
              } else if (record && key == "$defs") {
                if (!item.value().is_object()) {
                  return false;
                }
                for (const auto& definition : item.value()) {
                  if (!self(self, definition, true, depth + 1)) {
                    return false;
                  }
                }
              } else if (record && key == "required") {
                if (!item.value().is_array()) {
                  return false;
                }
                for (const auto& name : item.value()) {
                  if (!name.is_string() || !value.contains("properties") ||
                      !value["properties"].contains(name.get<std::string>())) {
                    return false;
                  }
                }
              } else if (key == "additionalProperties") {
                if (!item.value().is_boolean()) {
                  return false;
                }
              } else if (!record && key == "items") {
                if (!item.value().is_object()) {
                  return false;
                }
                for (const auto& item_key : item.value().items()) {
                  if (item_key.key() != "type" && item_key.key() != "$ref" &&
                      item_key.key() != "description" &&
                      item_key.key() != "title") {
                    return false;
                  }
                }
                const auto type = item.value().find("type");
                if (type != item.value().end() &&
                    (!type->is_string() || *type == "array")) {
                  return false;
                }
              } else if (!record && (key == "enum" || key == "const")) {
                if (item.value().is_null()) {
                  return false;
                }
                if (key == "enum") {
                  if (!item.value().is_array() || item.value().empty()) {
                    return false;
                  }
                  for (const auto& choice : item.value()) {
                    if (choice.is_null()) {
                      return false;
                    }
                  }
                }
              } else if (!record &&
                         (key == "$ref" || key == "default" ||
                          key == "minimum" || key == "maximum" ||
                          key == "minLength" || key == "maxLength" ||
                          key == "minItems" || key == "maxItems" ||
                          key == "uniqueItems" || key == "pattern")) {
                continue;
              } else if (!record && key == "format") {
                if (item.value() != "date-time" && item.value() != "duration") {
                  return false;
                }
              } else if (!record && key == "contentEncoding") {
                if (item.value() != "base64") {
                  return false;
                }
              } else {
                return false;
              }
            }
            return true;
          };
          if (!representable(representable, metadata, true, 0)) {
            reject(
                "Object metadata contains a constraint without a lossless "
                "Flow declaration.");
            continue;
          }
          const auto prefix = absl::StrCat(
              "Action_", encode(schema.name), "_",
              direction == syntax::PortDirection::kInput ? "in_" : "out_",
              encode(name), "_");
          auto imported = JsonSchemaToDtos(metadata, "Payload");
          absl::flat_hash_map<std::string, std::string> renamed;
          for (const auto& dto : imported.dtos) {
            renamed.emplace(dto.name, prefix + encode(dto.name));
          }
          const auto rename_type = [&](std::string value) {
            const size_t bracket = value.find('[');
            if (bracket != std::string::npos && value.back() == ']') {
              const auto element =
                  value.substr(bracket + 1, value.size() - bracket - 2);
              if (const auto found = renamed.find(element);
                  found != renamed.end()) {
                value.replace(bracket + 1, element.size(), found->second);
              }
            } else if (const auto found = renamed.find(value);
                       found != renamed.end()) {
              value = found->second;
            }
            return value;
          };
          for (auto& dto : imported.dtos) {
            dto.name = renamed.at(dto.name);
            for (auto& field : dto.fields) {
              field.declared = rename_type(field.declared);
              field.type = rename_type(field.type);
              field.dto_name = rename_type(field.dto_name);
              field.element = rename_type(field.element);
              field.element_dto_name = rename_type(field.element_dto_name);
              field.syntax_type.reset();
            }
          }
          for (auto& diagnostic : imported.diagnostics) {
            diagnostic.severity = Severity::kError;
            result.diagnostics.push_back(std::move(diagnostic));
          }
          if (imported.diagnostics.empty() && !imported.dtos.empty()) {
            entry.declared = imported.dtos.front().name;
            for (auto& dto : imported.dtos) {
              result.types.push_back(std::move(dto));
            }
          }
        } else if (metadata.is_object() && metadata.contains("type") &&
                   metadata["type"] == "array") {
          const auto array_type =
              [&](const auto& self, const nlohmann::json& value,
                  int depth) -> absl::StatusOr<std::string> {
            if (depth > 128 || !value.is_object() || !value.contains("type") ||
                !value["type"].is_string()) {
              return absl::InvalidArgumentError(
                  "An array item has no representable Flow type.");
            }
            for (const auto& item : value.items()) {
              if (item.key() != "type" && item.key() != "items" &&
                  item.key() != "description" && item.key() != "title" &&
                  item.key() != "$schema") {
                return absl::InvalidArgumentError(
                    "Array metadata contains a constraint without a Flow port "
                    "spelling.");
              }
            }
            const auto type = value["type"].get<std::string>();
            if (type == "array") {
              if (!value.contains("items")) {
                return std::string("list");
              }
              ABSL_ASSIGN_OR_RETURN(auto element,
                                    self(self, value["items"], depth + 1));
              return absl::StrCat("list[", element, "]");
            }
            if (!vocabulary::TypeNames().contains(type)) {
              return absl::InvalidArgumentError(
                  "An array item type has no Flow spelling.");
            }
            return type;
          };
          const auto type = array_type(array_type, metadata, 0);
          if (type.ok()) {
            entry.declared = *type;
          } else {
            reject(std::string(type.status().message()));
          }
        } else {
          bool representable = metadata.is_object();
          if (representable) {
            for (const auto& item : metadata.items()) {
              if (item.key() != "$schema" && item.key() != "title" &&
                  item.key() != "description" && item.key() != "type") {
                representable = false;
              }
            }
            const auto declared_type = metadata.find("type");
            if (declared_type != metadata.end()) {
              Program evidence;
              if (!declared_type->is_string() ||
                  RefinesType(port.type, declared_type->get<std::string>(),
                              evidence) != Refinement::kYes ||
                  RefinesType(declared_type->get<std::string>(), port.type,
                              evidence) != Refinement::kYes) {
                representable = false;
              }
            }
          }
          if (!representable) {
            Diagnostic diagnostic;
            diagnostic.code = "flow.schema.unsupported-keyword";
            diagnostic.message =
                "This action port's metadata has no lossless Flow declaration.";
            result.diagnostics.push_back(std::move(diagnostic));
          }
        }
      }
      result.interface.ports.push_back(std::move(entry));
    }
  }
  std::vector<std::string> names;
  for (const auto& [name, header] : schema.headers) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  for (const auto& name : names) {
    const auto& header = schema.headers.at(name);
    HeaderPlan entry;
    entry.name = name;
    entry.alias = absl::StrCat("header", result.interface.headers.size());
    entry.description = header.description;
    entry.has_default = header.default_value.has_value();
    if (entry.has_default) {
      entry.default_value = syntax::Constant::String(*header.default_value);
    }
    result.interface.headers.push_back(std::move(entry));
  }
  return result;
}

absl::StatusOr<actions::ActionHandler> MakeHandler(
    std::shared_ptr<const CompiledProgram> program, std::string_view flow,
    RunOptions options) {
  if (program == nullptr) {
    return Fail("There is no program to run.");
  }
  const ResolvedFlow* found = program->Flow(flow);
  if (found == nullptr) {
    std::vector<std::string> known;
    for (const ResolvedFlow& one : program->flows()) {
      known.push_back(one.plan.name);
    }
    std::sort(known.begin(), known.end());
    return Fail(
        absl::StrCat("No flow named '", flow, "' in ",
                     program->source_name().empty() ? "this program"
                                                    : program->source_name(),
                     " (declared: ",
                     known.empty() ? "none" : absl::StrJoin(known, ", "), ")."),
        absl::StatusCode::kNotFound);
  }
  std::shared_ptr<HostBridge> bridge = options.bridge;
  if (bridge == nullptr) {
    bridge = NativeHostBridge();
  }
  return actions::MakeAsyncActionHandler(
      [program = std::move(program), name = std::string(flow),
       bridge = std::move(bridge), stream = std::move(options.dispatch_stream),
       snapshot = std::move(options.snapshot)](
          std::shared_ptr<actions::Action> action) -> absl::Status {
        const ResolvedFlow* one = program->Flow(name);
        if (one == nullptr) {
          return Fail(absl::StrCat("No flow named '", name, "' any more."),
                      absl::StatusCode::kNotFound);
        }
        Runner runner(program, *one, std::move(action), bridge, stream,
                      snapshot);
        return runner.Run();
      });
}

absl::StatusOr<actions::ActionHandler> MakeEntryHandler(
    std::shared_ptr<const CompiledProgram> program, RunOptions options) {
  if (program == nullptr) {
    return Fail("There is no program to run.");
  }
  if (program->Entry() == nullptr) {
    return Fail(
        absl::StrCat(
            program->source_name().empty() ? "This program"
                                           : program->source_name(),
            " declares no entry flow. A file that is meant to be run declares "
            "one as `flow { ... }` -- with no name, because an entry point is "
            "not something anything else calls."),
        absl::StatusCode::kNotFound);
  }
  std::shared_ptr<HostBridge> bridge = options.bridge;
  if (bridge == nullptr) {
    bridge = NativeHostBridge();
  }
  return actions::MakeAsyncActionHandler(
      [program = std::move(program), bridge = std::move(bridge),
       stream = std::move(options.dispatch_stream),
       snapshot = std::move(options.snapshot)](
          std::shared_ptr<actions::Action> action) -> absl::Status {
        // Looked up again rather than captured: the handler outlives this call
        // and the program is what owns the flow.
        const ResolvedFlow* one = program->Entry();
        if (one == nullptr) {
          return Fail("This program has no entry flow any more.",
                      absl::StatusCode::kNotFound);
        }
        Runner runner(program, *one, std::move(action), bridge, stream,
                      snapshot);
        return runner.Run();
      });
}

}  // namespace a11::flow
