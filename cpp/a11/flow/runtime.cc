// Copyright 2026 The A11 Authors.

#include "a11/flow/runtime.h"

#include <algorithm>
#include <deque>
#include <functional>
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

#include "a11/actions/registry.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/flow/vocabulary.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "thread/boost_primitives.h"

namespace a11::flow {
namespace {

using graph::BodyId;
using graph::ExprId;
using graph::FlowGraph;
using graph::kNone;
using graph::RefId;
using graph::RefKind;
using graph::StepId;
using graph::StepKind;

/// The stack every fiber of a flow gets.
///
/// A11's pooled fiber stacks are hundreds of bytes, which is right for a fiber
/// that moves a buffer around and far too small for one of these: any of them can
/// reach the [HostBridge], and on the Python path that means an interpreter frame
/// chain -- a pydantic model validating a nested document is not one frame. Sized
/// for that rather than for the pump, because a flow has tens of fibers and not
/// thousands.
constexpr size_t kStackSize = 256 * 1024;

thread::TreeOptions StackOptions() {
  return thread::TreeOptions{.stack_size = kStackSize};
}

absl::Status Fail(std::string_view message,
                  absl::StatusCode code = absl::StatusCode::kInvalidArgument) {
  return absl::Status(code, message);
}

/// What a barrier is a barrier on, out of its label: `wait x` -> `x`.
std::string SubjectOf(const graph::Step& step) {
  const size_t space = step.label.find(' ');
  if (space == std::string::npos) return step.label;
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
    item->value_ = std::move(value);
    item->decoded_ = true;
    return item;
  }

  const std::optional<data::Chunk>& chunk() const { return chunk_; }

  std::string Mimetype() const {
    if (!chunk_.has_value()) return std::string(data::kJsonMimetype);
    return chunk_->GetMimetype();
  }

  /// The decoded value, read out of the chunk the first time it is asked for.
  absl::StatusOr<Value> Read(HostBridge* absl_nonnull bridge) const {
    thread::MutexLock lock(&mu_);
    if (!decoded_) {
      if (!chunk_.has_value()) {
        value_ = Value::Null();
      } else {
        ABSL_ASSIGN_OR_RETURN(value_, bridge->FromChunk(*chunk_));
      }
      decoded_ = true;
    }
    return value_;
  }

 private:
  std::optional<data::Chunk> chunk_;
  mutable thread::Mutex mu_;
  mutable Value value_;
  mutable bool decoded_ = false;
};

using ItemPtr = std::shared_ptr<Item>;

/// A mimetype without its parameters: `application/x-msgpack;type=X`.
std::string BaseMimetype(std::string_view mimetype) {
  const size_t at = mimetype.find(';');
  std::string base(at == std::string_view::npos ? mimetype
                                                : mimetype.substr(0, at));
  absl::AsciiStrToLower(&base);
  return std::string(absl::StripAsciiWhitespace(base));
}

/// Whether `name` matches `pattern`, where `*` stands for any run of characters.
///
/// The whole of the globbing `mime` and `forward headers` need, and small enough
/// to be obviously right: a pattern is a sequence of literals separated by stars.
bool Matches(std::string_view name, std::string_view pattern) {
  const std::vector<std::string_view> parts = absl::StrSplit(pattern, '*');
  if (parts.size() == 1) return name == pattern;
  if (!absl::StartsWith(name, parts.front())) return false;
  if (!absl::EndsWith(name, parts.back())) return false;
  size_t at = parts.front().size();
  const size_t limit = name.size() - parts.back().size();
  if (at > limit) return false;
  for (size_t index = 1; index + 1 < parts.size(); ++index) {
    if (parts[index].empty()) continue;
    const size_t found = name.find(parts[index], at);
    if (found == std::string_view::npos || found + parts[index].size() > limit) {
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

/// Everything one run waits on, and the one way to give up on all of it.
///
/// A single lock and a single condition variable for the whole run. Coarse on
/// purpose: a flow has tens of steps rather than millions of operations, and what
/// matters far more than contention is that every wait in the runtime can be
/// woken at once when the run is over -- a pump waiting for a reader that will
/// never come is a hung flow, and this is what makes that impossible.
///
/// Blocking work -- a node read, a node write, waiting for an action, anything
/// that reaches the host -- happens with the lock released.
class Monitor {
 public:
  thread::Mutex& mu() ABSL_LOCK_RETURNED(mu_) { return mu_; }

  /// Wake every waiter, because something they may be waiting for has changed.
  void Wake() { cv_.SignalAll(); }

  /// Wait until `ready` holds, or the run is given up on.
  ///
  /// REQUIRES: the lock is held.
  absl::Status Wait(absl::FunctionRef<bool()> ready)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    while (!ready()) {
      if (!stop_.ok()) return stop_;
      cv_.Wait(&mu_);
    }
    return absl::OkStatus();
  }

  /// The same, up to a deadline: `DeadlineExceeded` when it passes first.
  absl::Status WaitUntil(absl::Time deadline,
                         absl::FunctionRef<bool()> ready)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    while (!ready()) {
      if (!stop_.ok()) return stop_;
      if (cv_.WaitWithDeadline(&mu_, deadline) && !ready()) {
        return absl::DeadlineExceededError("The wait timed out");
      }
    }
    return absl::OkStatus();
  }

  /// Give up on the run, waking everything waiting on anything.
  ///
  /// The one thing a single monitor buys that a lock per object would not: a pump
  /// waiting for a reader that will never come, a barrier waiting for a node
  /// nobody will close, and a step waiting for a dependency that failed are all
  /// woken by this, so a failed flow ends instead of hanging.
  void Stop(absl::Status why) {
    if (why.ok()) why = absl::CancelledError("The flow stopped");
    {
      thread::MutexLock lock(&mu_);
      if (!stop_.ok()) return;
      stop_ = std::move(why);
    }
    cv_.SignalAll();
  }

 private:
  thread::Mutex mu_;
  thread::CondVar cv_;
  absl::Status stop_ ABSL_GUARDED_BY(mu_);
};

/// A set of concurrent fibers with one outcome.
///
/// The first failure is the group's, and everything else is cancelled rather than
/// waited out: a step that failed leaves pumps and barriers waiting for things
/// that are not coming.
class Group {
 public:
  explicit Group(Monitor& monitor) : monitor_(&monitor) {}

  Group(const Group&) = delete;
  Group& operator=(const Group&) = delete;

  /// Nothing is left running: a group that was abandoned rather than joined --
  /// because the code around it failed -- cancels its fibres and waits for them.
  ~Group() {
    if (!joined_) Give();
    Join().IgnoreError();
  }

  void Spawn(absl::AnyInvocable<absl::Status() &&> work) {
    {
      thread::MutexLock lock(&monitor_->mu());
      ++spawned_;
    }
    a11::Task task = a11::SubmitTask(std::move(work), StackOptions());
    // Counted through the future rather than inside the work: a fiber cancelled
    // before it started never runs its body, and a group that counted there
    // would wait for a fiber that had already finished.
    task.OnReady([this](const absl::StatusOr<a11::Unit>& result) {
      Finish(result.status());
    });
    tasks_.push_back(std::move(task));
  }

  /// Wait for every fiber, giving up on the run as soon as one fails.
  absl::Status Join() {
    if (joined_) return first_;
    joined_ = true;
    bool failed = false;
    {
      thread::MutexLock lock(&monitor_->mu());
      while (done_ < spawned_ && first_.ok()) cv_.Wait(&monitor_->mu());
      failed = done_ < spawned_;
    }
    if (failed) {
      Give();
      thread::MutexLock lock(&monitor_->mu());
      while (done_ < spawned_) cv_.Wait(&monitor_->mu());
    }
    return first_;
  }

 private:
  /// Stop waiting for anything: the run is over, one way or another.
  ///
  /// Both halves are needed. Stopping the monitor wakes everything parked on a
  /// condition -- a pump with no reader, a barrier on a node nobody will close --
  /// and cancelling the fibres wakes everything parked on a [a11::Future], which
  /// is every node read and every wait for an action.
  void Give() {
    absl::Status why;
    {
      thread::MutexLock lock(&monitor_->mu());
      why = first_;
    }
    monitor_->Stop(why.ok() ? absl::CancelledError("The flow stopped") : why);
    for (const a11::Task& task : tasks_) task.Cancel().IgnoreError();
  }

  void Finish(const absl::Status& status) {
    // Signalled with the lock still held, on purpose: [Join] wakes holding it,
    // and a signal sent after releasing would touch this group's condition
    // variable after the last waiter had already returned and destroyed it.
    thread::MutexLock lock(&monitor_->mu());
    ++done_;
    // A cancellation is not a reason. Everything else in the group is cancelled
    // when one fibre fails, so the real failure has to win however the two race
    // -- otherwise a flow that timed out reports that it was cancelled.
    if (!status.ok() &&
        (first_.ok() ||
         (absl::IsCancelled(first_) && !absl::IsCancelled(status)))) {
      first_ = status;
    }
    cv_.SignalAll();
  }

  Monitor* absl_nonnull monitor_;
  thread::CondVar cv_;
  std::vector<a11::Task> tasks_;
  size_t spawned_ = 0;
  size_t done_ = 0;
  absl::Status first_;
  bool joined_ = false;
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
    if (at_ >= items_.size()) return ItemPtr{};
    return items_[at_++];
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

/// A stream with a fixed number of readers, fed by one producer.
class Bus {
 public:
  /// What a producer is handed to publish one value with.
  using Emit = absl::FunctionRef<absl::Status(ItemPtr)>;
  using Produce = std::function<absl::Status(Emit)>;

  Bus(Monitor& monitor, std::string label, Produce produce, int readers)
      : monitor_(&monitor), label_(std::move(label)),
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

  void Wanted() {
    {
      thread::MutexLock lock(&monitor_->mu());
      wanted_ = true;
    }
    monitor_->Wake();
  }

  Monitor& monitor() { return *monitor_; }

 private:
  friend class BusReader;

  Monitor* absl_nonnull monitor_;
  std::string label_;
  Produce produce_;
  std::vector<std::unique_ptr<Slot>> slots_;
  size_t handed_out_ = 0;
  bool wanted_ = false;
};

class BusReader : public Reader {
 public:
  BusReader(Bus& bus, Slot& slot) : bus_(&bus), slot_(&slot) {}

  absl::StatusOr<ItemPtr> Next() override {
    bus_->Wanted();
    Monitor& monitor = bus_->monitor();
    thread::MutexLock lock(&monitor.mu());
    ABSL_RETURN_IF_ERROR(monitor.Wait(
        [this] { return !slot_->items.empty() || slot_->ended; }));
    if (!slot_->items.empty()) {
      ItemPtr item = std::move(slot_->items.front());
      slot_->items.pop_front();
      monitor.Wake();
      return item;
    }
    ABSL_RETURN_IF_ERROR(slot_->error);
    return ItemPtr{};
  }

  void Stop() override {
    Monitor& monitor = bus_->monitor();
    {
      thread::MutexLock lock(&monitor.mu());
      if (slot_->dropped) return;
      slot_->dropped = true;
      slot_->items.clear();
    }
    // The producer may be waiting for room this reader will never make.
    bus_->Wanted();
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
  // Nothing is read before something asks for it. Reading can *act* -- ending a
  // node, waiting on a call -- and a step held back by `after` must not have its
  // reads run ahead of it.
  {
    thread::MutexLock lock(&monitor_->mu());
    ABSL_RETURN_IF_ERROR(monitor_->Wait([this] { return wanted_; }));
  }
  absl::Status error;
  const auto emit = [this](ItemPtr item) -> absl::Status {
    thread::MutexLock lock(&monitor_->mu());
    ABSL_RETURN_IF_ERROR(monitor_->Wait([this] {
      for (const std::unique_ptr<Slot>& slot : slots_) {
        if (!slot->dropped && slot->items.size() >= kQueueDepth) return false;
      }
      return true;
    }));
    for (const std::unique_ptr<Slot>& slot : slots_) {
      if (!slot->dropped) slot->items.push_back(item);
    }
    monitor_->Wake();
    return absl::OkStatus();
  };
  error = produce_(emit);
  {
    thread::MutexLock lock(&monitor_->mu());
    for (const std::unique_ptr<Slot>& slot : slots_) {
      slot->ended = true;
      slot->error = error;
    }
  }
  monitor_->Wake();
  // The failure belongs to the readers, who each see it where they were reading.
  // Reporting it here as well would fail the flow twice for one cause.
  return absl::OkStatus();
}

/// A single value computed the first time it is asked for, then shared.
///
/// What a status is: reading one waits for its subject and may end a node, so it
/// happens when a step asks, and once however many steps ask.
class Lazy {
 public:
  using Produce = std::function<absl::StatusOr<ItemPtr>()>;

  Lazy(Monitor& monitor, Produce produce)
      : monitor_(&monitor), produce_(std::move(produce)) {}

  absl::StatusOr<ReaderPtr> Replay() {
    ABSL_ASSIGN_OR_RETURN(const ItemPtr item, Get());
    std::vector<ItemPtr> items;
    if (item != nullptr) items.push_back(item);
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

/// A stream buffered once and replayed to every reader.
///
/// What a ref read inside a loop or a branch becomes. The buffer is filled by a
/// single reader of the underlying stream, and each pass iterates the buffer, so
/// every pass sees the same values.
///
/// **It grows while it is read.** The buffer used to read its source to the *end*
/// before handing out a single value, which made a loop that reads anything from
/// outside it wait for that stream to finish -- and, since the buffer is one
/// reader among the others, made every later statement reading the same ref wait
/// with it. A `for` over one node whose body reads another could not begin until
/// the second was closed, which is not something anything in the source says. A
/// reader now waits for the *item it is asking for* and no further.
///
/// What is not fixed here, because it is what replaying means: the whole stream
/// is held in memory once, for as long as the body that reads it runs. That is
/// the price of every pass seeing the same values, and it is the reason only refs
/// a nested body actually reads are buffered at all.
class Buffer {
 public:
  /// The items, and whether there are more coming. Held behind a shared pointer
  /// so that a reader cannot outlive what it reads: the buffer belongs to a
  /// [Scope] and a reader of it is handed to whatever is nested inside that
  /// scope, which is a lifetime the type should not have to be reasoned about.
  /// The previous buffer copied its items into every reader, so it had no such
  /// question to answer; incremental delivery means the reader looks at the
  /// buffer's own storage, and this is what keeps that safe.
  class State {
   public:
    explicit State(Monitor& monitor) : monitor_(&monitor) {}

    /// The item at `index`, once it is there; a null item at the end.
    ///
    /// A failure arrives *after* the items that did, which is what a [BusReader]
    /// does with a slot's error: a pass that had read three of five values saw
    /// three values and then a failure, rather than the failure alone.
    absl::StatusOr<ItemPtr> At(size_t index) {
      thread::MutexLock lock(&monitor_->mu());
      ABSL_RETURN_IF_ERROR(monitor_->Wait(
          [this, index] { return index < items_.size() || ended_; }));
      if (index < items_.size()) return items_[index];
      ABSL_RETURN_IF_ERROR(error_);
      return ItemPtr{};
    }

    void Add(ItemPtr item) {
      {
        thread::MutexLock lock(&monitor_->mu());
        items_.push_back(std::move(item));
      }
      // Per item, because a reader waiting for this one is waiting now.
      monitor_->Wake();
    }

    void End(absl::Status error) {
      {
        thread::MutexLock lock(&monitor_->mu());
        error_ = std::move(error);
        ended_ = true;
      }
      monitor_->Wake();
    }

   private:
    Monitor* absl_nonnull monitor_;
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
    while (true) {
      absl::StatusOr<ItemPtr> item = source_->Next();
      if (!item.ok()) {
        status = item.status();
        break;
      }
      if (*item == nullptr) break;
      state_->Add(*std::move(item));
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

/// The one view of a stream that every *value* read of it shares.
///
/// **Why a value read is not just a read of the first value.** `let a = x` and
/// `let b = x` are two values of `x`, not two names for its first: they take
/// turns on one view of the stream. Which of them gets which is not defined, and
/// `advance` and `after` are how a flow that cares says so. Reading the first
/// value twice was the old behaviour, and it silently discarded everything else
/// `x` had.
///
/// **A provably unary stream is different**, and is the case worth being strict
/// about: there is one value, so every reader of it sees that one value, and a
/// *second* value arriving is a fact about the flow that nothing else would ever
/// report. It is read once, kept, and the stream is then required to be over.
class ValueCursor {
 public:
  ValueCursor(HostBridge& bridge, ReaderPtr reader, bool unary,
              std::string label)
      : bridge_(&bridge), reader_(std::move(reader)), unary_(unary),
        label_(std::move(label)) {}

  /// The next value, or the kept one for a unary stream.
  ///
  /// One reader at a time, the way [Lazy] does it: the turn is taken under the
  /// monitor and the *read* happens outside it, because reading waits on the
  /// monitor itself and holding it across that is a fibre deadlocking on its own
  /// lock. Two steps reading this stream for a value at the same moment therefore
  /// take turns, and get two different values rather than the same one twice.
  absl::StatusOr<Value> Next(Monitor& monitor) {
    {
      thread::MutexLock lock(&monitor.mu());
      ABSL_RETURN_IF_ERROR(monitor.Wait([this] { return !busy_; }));
      if (unary_ && read_) return held_;
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
    if (!unary_ || item == nullptr) return value;
    // One value was promised, so a second is an error naming what promised it:
    // the port said so by not saying `stream`, or the pipeline said so by
    // reducing. Reading to the end is what finds out, and for a stream of one
    // that is one more read.
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
    if (stopped_) return ItemPtr{};
    ABSL_ASSIGN_OR_RETURN(ItemPtr item, state_->At(at_));
    if (item != nullptr) ++at_;
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
  ABSL_RETURN_IF_ERROR(node->PutNullFinal().Await().status());
  return node->DrainAndClose().Await().status();
}

/// A node several steps may write, closed when the last of them is done.
class Destination {
 public:
  using Open = std::function<absl::StatusOr<NodePtr>()>;

  Destination(Monitor& monitor, std::string label, Open open, int writers,
              bool tolerant)
      : monitor_(&monitor), label_(std::move(label)), open_(std::move(open)),
        writers_(writers), tolerant_(tolerant) {
    // Nothing here will write it, so nothing here is holding it open.
    if (writers_ <= 0) finished_ = true;
  }

  const std::string& label() const { return label_; }
  int writers() const { return writers_; }

  absl::StatusOr<NodePtr> Node() {
    {
      thread::MutexLock lock(&monitor_->mu());
      if (opening_) {
        ABSL_RETURN_IF_ERROR(monitor_->Wait([this] { return opened_; }));
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
    monitor_->Wake();
    return node;
  }

  /// Append one value, as the producer wrote it wherever possible.
  absl::Status Write(const ItemPtr& item, HostBridge* absl_nonnull bridge) {
    ABSL_ASSIGN_OR_RETURN(const NodePtr node, Node());
    data::Chunk chunk;
    if (item->chunk().has_value()) {
      chunk = *item->chunk();
    } else {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(bridge));
      ABSL_ASSIGN_OR_RETURN(chunk, bridge->ToChunk(value, {}));
    }
    // One writer at a time, so two steps sharing a destination append whole
    // values rather than interleaving halves of two.
    ABSL_RETURN_IF_ERROR(Enter());
    const absl::Status status = node->PutChunk(std::move(chunk)).Await().status();
    Leave();
    // A `try call` that has already failed or been cancelled has aborted its
    // ports; feeding one is then not the flow's problem.
    if (!status.ok() && !tolerant_) return status;
    return absl::OkStatus();
  }

  /// Close the node now, whoever was writing it.
  ///
  /// What `drain` does to a node the flow does not write itself: a callee given a
  /// node to write does not close it -- it does not own it -- so the flow that
  /// lent it the node is the one that says when it is over.
  absl::Status End() { return Finish(/*forced=*/true); }

  /// One writer is done; close the node when it was the last.
  ///
  /// The close writes the null final chunk that says the stream is over, so a
  /// reader waiting on a whole value is told the value has ended rather than left
  /// waiting.
  absl::Status Release() { return Finish(/*forced=*/false); }

  absl::Status Finished() {
    thread::MutexLock lock(&monitor_->mu());
    return monitor_->Wait([this] { return finished_; });
  }

 private:
  absl::Status Enter() {
    thread::MutexLock lock(&monitor_->mu());
    ABSL_RETURN_IF_ERROR(monitor_->Wait([this] { return !writing_; }));
    writing_ = true;
    return absl::OkStatus();
  }

  void Leave() {
    {
      thread::MutexLock lock(&monitor_->mu());
      writing_ = false;
    }
    monitor_->Wake();
  }

  absl::Status Finish(bool forced) {
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
      status = node.ok() ? CloseNode(*node) : node.status();
      {
        thread::MutexLock lock(&monitor_->mu());
        finished_ = true;
      }
    }
    Leave();
    if (!status.ok() && !tolerant_) return status;
    return absl::OkStatus();
  }

  Monitor* absl_nonnull monitor_;
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
      if (error_.ok()) error_ = std::move(why);
      done_ = true;
    }
    monitor_->Wake();
  }

  void Done() {
    {
      thread::MutexLock lock(&monitor_->mu());
      done_ = true;
    }
    monitor_->Wake();
  }

  void SetError(absl::Status error) {
    thread::MutexLock lock(&monitor_->mu());
    if (error_.ok()) error_ = std::move(error);
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
  absl::StatusOr<NodePtr> Node(std::string name,
                              syntax::PortDirection direction) {
    ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<actions::Action> action,
                          Action());
    if (direction == syntax::PortDirection::kInput) {
      return action->GetInput(std::move(name), std::nullopt);
    }
    return action->GetOutput(std::move(name), std::nullopt);
  }

  /// How the call went, once it has gone.
  ///
  /// Two statuses, which is why this is not a `StatusOr`: the returned one says
  /// whether the *waiting* worked, and `outcome` is what the call finished with.
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
      : runner_(&runner), body_(body), parent_(parent),
        presets_(std::move(presets)) {}

  absl::Status Run();

  /// What the pass captured, by slot: a `repeat`'s carry and its condition.
  const absl::flat_hash_map<std::string, Value>& captures() const {
    return captures_;
  }

 private:
  friend class Runner;

  absl::Status Prepare();
  absl::Status RunStep(StepId step);
  absl::Status Execute(StepId step);
  absl::Status StepDone(StepId step);
  void MarkDone(StepId step);

  absl::Status RunForEach(StepId step);
  absl::Status RunRepeat(StepId step);
  absl::Status RunWait(StepId step);
  absl::Status Failure(StepId step);

  /// An independent view of a ref's values, for one reader.
  absl::StatusOr<ReaderPtr> Subscribe(RefId ref);
  Scope* absl_nonnull Owner(RefId ref);
  Scope* absl_nullable FindOwner(RefId ref);
  Destination* absl_nullable FindDestination(RefId ref);
  absl::StatusOr<Destination*> DestinationOf(RefId ref);
  absl::StatusOr<CallHandle*> Call(StepId step);

  absl::Status Produce(RefId ref, Bus::Emit emit);
  absl::Status ProduceStage(RefId ref, Bus::Emit emit);
  absl::Status ProduceZip(RefId ref, Bus::Emit emit);
  absl::StatusOr<ItemPtr> StatusItem(RefId ref);
  absl::Status NodeOutcome(RefId ref, absl::Status* absl_nonnull outcome);
  absl::StatusOr<NodePtr> ReadableNode(RefId ref);
  absl::StatusOr<NodePtr> DestinationNode(RefId ref);
  absl::StatusOr<NodePtr> LocalNode(RefId ref);
  absl::StatusOr<NodePtr> MakeLocalNode(RefId ref);

  /// Read a node as items, each keeping the producer's own chunk.
  absl::Status ReadNode(const NodePtr& node, bool tolerant, Bus::Emit emit);

  absl::StatusOr<Value> ValueOf(RefId ref);
  absl::StatusOr<Value> Evaluate(ExprId expr);
  absl::StatusOr<Value> EvaluateWith(ExprId expr, const Value& it);

  const FlowGraph& graph() const;
  Monitor& monitor() const;
  HostBridge& bridge() const;
  /// The shapes the program declared, for a cast or a coercion in this body.
  const Program& shapes() const;

  Runner* absl_nonnull runner_;
  BodyId body_ = kNone;
  Scope* absl_nullable parent_ = nullptr;
  absl::flat_hash_map<RefId, std::vector<ItemPtr>> presets_;
  graph::Analysis analysis_;
  absl::flat_hash_map<RefId, std::unique_ptr<Bus>> buses_;
  absl::flat_hash_map<RefId, std::unique_ptr<Lazy>> lazies_;
  absl::flat_hash_map<RefId, std::unique_ptr<Buffer>> buffers_;
  absl::flat_hash_map<RefId, std::unique_ptr<Destination>> destinations_;
  absl::flat_hash_map<StepId, std::unique_ptr<CallHandle>> calls_;
  absl::flat_hash_map<StepId, bool> done_;
  /// How a `[try] { ... }` step went, for the name bound to it to read.
  absl::flat_hash_map<StepId, absl::Status> outcomes_;
  absl::flat_hash_map<std::string, Value> captures_;
  /// One cursor per ref, shared by every value read of it: see [ValueCursor].
  /// On the scope that *owns* the ref, so passes of a loop take turns on the one
  /// view of an outer stream rather than each starting it again.
  absl::flat_hash_map<RefId, std::unique_ptr<ValueCursor>> cursors_;
  /// Refs whose cursor is being made right now, so only one step subscribes.
  absl::flat_hash_set<RefId> opening_cursors_;
  absl::flat_hash_map<RefId, absl::StatusOr<NodePtr>> nodes_;
  absl::flat_hash_set<RefId> opening_;
  std::vector<RefId> unwritten_;
};

/// One execution of one flow, against the action it is running as.
class Runner {
 public:
  Runner(std::shared_ptr<const CompiledProgram> program,
         const ResolvedFlow& flow, std::shared_ptr<actions::Action> action,
         std::shared_ptr<HostBridge> bridge,
         std::shared_ptr<net::WireStream> dispatch_stream)
      : program_(std::move(program)), flow_(&flow), action_(std::move(action)),
        bridge_(std::move(bridge)),
        dispatch_stream_(std::move(dispatch_stream)) {}

  absl::Status Run() {
    Scope root(*this, flow_->graph.root, nullptr, {});
    const absl::Status status = root.Run();
    if (!status.ok()) monitor_.Stop(status);
    return status;
  }

  Monitor& monitor() { return monitor_; }
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
  /// One map per name per execution. Nodes created in it are not in the session's
  /// node map, so a peer neither sees them nor receives their fragments -- which
  /// is the point of putting a step's traffic there.
  absl::StatusOr<std::shared_ptr<nodes::NodeMap>> NodeMapNamed(
      const std::string& name) {
    thread::MutexLock lock(&monitor_.mu());
    const auto found = node_maps_.find(name);
    if (found != node_maps_.end()) return found->second;
    ABSL_ASSIGN_OR_RETURN(std::shared_ptr<nodes::NodeMap> made,
                          nodes::NodeMap::Create());
    node_maps_.emplace(name, made);
    return made;
  }

  /// An id for a node the flow declared, unique within this run.
  ///
  /// Named after the flow's own action and the name the flow gave it, so a node a
  /// peer does see is recognisable rather than a bare identifier.
  absl::StatusOr<std::string> FreshNodeId(const std::string& name) {
    int count = 0;
    {
      thread::MutexLock lock(&monitor_.mu());
      count = ++node_counts_[name];
    }
    const std::string suffix =
        count == 1 ? name : absl::StrCat(name, "-", count);
    return actions::Action::MakeNodeId(action_->GetId(), suffix);
  }

  /// The schema and handler for a call target.
  ///
  /// The handler may be empty: an action registered for its schema alone is one
  /// whose ports are known here and whose *work* happens on the peer, and that is
  /// what a flow composing a gateway's actions from the outside registers. Such
  /// an action can only be `call`ed; what `run` needs is a handler, and saying
  /// `run` without one is an error rather than a quiet trip to the session.
  absl::Status Resolve(const std::string& name, actions::ActionSchema* schema,
                       actions::ActionHandler* handler);

  absl::StatusOr<actions::ActionSchema> SchemaOf(const std::string& action) {
    {
      thread::MutexLock lock(&monitor_.mu());
      const auto found = schemas_.find(action);
      if (found != schemas_.end()) return found->second;
    }
    actions::ActionSchema schema;
    actions::ActionHandler handler;
    ABSL_RETURN_IF_ERROR(Resolve(action, &schema, &handler));
    thread::MutexLock lock(&monitor_.mu());
    schemas_.emplace(action, schema);
    return schema;
  }

  /// The headers `forward headers` sends on to a step, as they arrived.
  ///
  /// A pattern matches the flow's own headers -- what its caller sent -- and `*`
  /// in one matches any run of characters. Nothing is invented: a header that was
  /// not sent is simply not forwarded, because a composition should not fail over
  /// an optional one nobody supplied.
  data::ByteMap Forwarded(const graph::Step& step) const {
    data::ByteMap chosen;
    if (step.forward.empty()) return chosen;
    const data::ByteMap available = action_->Headers();
    for (const std::string& pattern : step.forward) {
      for (const auto& [name, value] : available) {
        if (MatchesFolded(name, pattern)) chosen[name] = value;
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
  Monitor monitor_;
  absl::flat_hash_map<std::string, std::shared_ptr<nodes::NodeMap>> node_maps_;
  absl::flat_hash_map<std::string, int> node_counts_;
  absl::flat_hash_map<std::string, actions::ActionSchema> schemas_;
};

const FlowGraph& Scope::graph() const { return runner_->graph(); }
Monitor& Scope::monitor() const { return runner_->monitor(); }
HostBridge& Scope::bridge() const { return runner_->bridge(); }
const Program& Scope::shapes() const { return runner_->shapes(); }

// --- Wiring ------------------------------------------------------------------

absl::Status Scope::Prepare() {
  const FlowGraph& flow = graph();
  analysis_ = graph::Analyse(flow, body_);

  for (const StepId step : flow.bodies[body_].steps) {
    done_[step] = false;
    if (flow.steps[step].kind != StepKind::kCall) continue;
    ABSL_ASSIGN_OR_RETURN(const actions::ActionSchema schema,
                          runner_->SchemaOf(flow.steps[step].action));
    // A port the target does not declare, rejected before anything runs. The
    // check cannot always happen while compiling -- an action's schema comes from
    // the registry of whatever runtime dispatches the flow -- so it happens here,
    // once, with the same wording the compiler would have used.
    for (const auto& [key, ref] : flow.steps[step].ports) {
      const bool input = absl::StartsWith(key, "inputs:");
      const std::string name = flow.refs[ref].name;
      const auto& declared = input ? schema.inputs : schema.outputs;
      if (declared.contains(name)) continue;
      std::vector<std::string> known;
      known.reserve(declared.size());
      for (const auto& [port, unused] : declared) known.push_back(port);
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

  for (const RefId ref : analysis_.refs) {
    const auto readers = analysis_.readers.find(ref);
    const int count = readers == analysis_.readers.end() ? 0 : readers->second;
    if (count <= 0 || presets_.contains(ref)) continue;
    if (flow.refs[ref].kind == RefKind::kStatus) {
      lazies_.emplace(ref, std::make_unique<Lazy>(
                               monitor(), [this, ref] {
                                 return StatusItem(ref);
                               }));
      continue;
    }
    auto bus = std::make_unique<Bus>(
        monitor(), flow.refs[ref].label,
        [this, ref](Bus::Emit emit) { return Produce(ref, emit); }, count);
    if (analysis_.materialise.contains(ref)) {
      // Read from inside a loop or a branch: buffered once here, replayed per
      // pass, so the buffer is the one reader of the underlying stream.
      ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, bus->Take());
      buffers_.emplace(ref,
                       std::make_unique<Buffer>(monitor(), std::move(reader)));
    }
    buses_.emplace(ref, std::move(bus));
  }

  // A node of the flow's own that nothing writes still has to end, or a reader of
  // it would wait for a value that was never coming.
  for (const RefId ref : analysis_.nodes) {
    if (flow.refs[ref].id_expr != kNone) continue;
    const auto readers = analysis_.readers.find(ref);
    if (readers == analysis_.readers.end() || readers->second <= 0) continue;
    if (std::find(analysis_.destinations.begin(), analysis_.destinations.end(),
                  ref) != analysis_.destinations.end()) {
      continue;
    }
    unwritten_.push_back(ref);
  }

  for (const RefId ref : analysis_.destinations) {
    const auto writers = analysis_.writers.find(ref);
    const int count = writers == analysis_.writers.end() ? 0 : writers->second;
    const graph::Ref& one = flow.refs[ref];
    const bool tolerant = one.kind == RefKind::kCallPort && one.call != kNone &&
                          flow.steps[one.call].tolerant;
    destinations_.emplace(
        ref, std::make_unique<Destination>(
                 monitor(), one.label,
                 [this, ref] { return DestinationNode(ref); }, count, tolerant));
  }
  return absl::OkStatus();
}

// --- Lookups -----------------------------------------------------------------

Scope* absl_nullable Scope::FindOwner(RefId ref) {
  const BodyId owner = graph().refs[ref].owner;
  for (Scope* at = this; at != nullptr; at = at->parent_) {
    if (at->body_ == owner) return at;
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
    if (found != at->calls_.end()) return found->second.get();
  }
  return Fail(absl::StrCat("Internal flow error: ", graph().steps[step].label,
                           " was never started."));
}

Destination* absl_nullable Scope::FindDestination(RefId ref) {
  for (Scope* at = this; at != nullptr; at = at->parent_) {
    const auto found = at->destinations_.find(ref);
    if (found != at->destinations_.end()) return found->second.get();
  }
  return nullptr;
}

absl::StatusOr<Destination*> Scope::DestinationOf(RefId ref) {
  Destination* found = FindDestination(ref);
  if (found != nullptr) return found;
  return Fail(absl::StrCat("Internal flow error: nothing writes ",
                           graph().refs[ref].label, "."));
}

absl::StatusOr<ReaderPtr> Scope::Subscribe(RefId ref) {
  if (ref == kNone) return Fail("Internal flow error: nothing to read.");
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
      ABSL_RETURN_IF_ERROR(monitor.Wait(
          [owner, ref] { return owner->nodes_.contains(ref); }));
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
  return map->Get(std::move(node_id));
}

absl::StatusOr<NodePtr> Scope::DestinationNode(RefId ref) {
  const graph::Ref& one = graph().refs[ref];
  switch (one.kind) {
    case RefKind::kCallPort: {
      ABSL_ASSIGN_OR_RETURN(CallHandle* handle, Call(one.call));
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
      ABSL_ASSIGN_OR_RETURN(CallHandle* handle, Call(one.call));
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
                             Bus::Emit emit) {
  while (true) {
    absl::StatusOr<std::optional<data::NodeFragment>> fragment =
        node->NextFragment().Await();
    if (!fragment.ok()) {
      // The producer aborted the node. A `try call` says the composition expects
      // that and the stream simply ends; otherwise the call step is the one that
      // reports it, so there is no need to fail twice.
      if (tolerant) return absl::OkStatus();
      return fragment.status();
    }
    if (!fragment->has_value()) return absl::OkStatus();
    ABSL_ASSIGN_OR_RETURN(const data::Chunk* chunk, (*fragment)->GetChunk());
    // A null chunk is a marker rather than a value: a final one ends the node,
    // and any other is skipped, which is how an empty stream stays empty instead
    // of turning into a value nobody wrote.
    if (chunk->IsNull()) {
      if (!(*fragment)->continued) return absl::OkStatus();
      continue;
    }
    ABSL_RETURN_IF_ERROR(emit(Item::OfChunk(*chunk)));
  }
}

// --- Producing streams -------------------------------------------------------

absl::Status Scope::Produce(RefId ref, Bus::Emit emit) {
  const graph::Ref& one = graph().refs[ref];
  // One place, upstream of the bus that fans the stream out, so the values
  // `skip n` spoke for are the same ones every reader misses.
  long long taken = 0;
  const long long skip = one.skip;
  const auto pass = [&](ItemPtr item) -> absl::Status {
    if (skip > 0 && taken++ < skip) return absl::OkStatus();
    return emit(std::move(item));
  };
  const Bus::Emit onward = skip > 0 ? Bus::Emit(pass) : emit;
  switch (one.kind) {
    case RefKind::kDerived:
      return ProduceStage(ref, onward);
    case RefKind::kZip:
      return ProduceZip(ref, onward);
    case RefKind::kExpr: {
      ABSL_ASSIGN_OR_RETURN(const Value value, Evaluate(one.expr));
      return onward(Item::Of(value));
    }
    case RefKind::kHeader: {
      ABSL_ASSIGN_OR_RETURN(const std::optional<data::Bytes> raw,
                            runner_->action()->GetHeader(one.header));
      if (!raw.has_value()) {
        if (!one.has_fallback) return absl::OkStatus();
        return onward(Item::Of(Value::Of(one.fallback)));
      }
      return onward(Item::Of(Value::String(*raw)));
    }
    case RefKind::kNodeId: {
      ABSL_ASSIGN_OR_RETURN(const NodePtr node, LocalNode(one.subject));
      ABSL_ASSIGN_OR_RETURN(const std::string id, node->GetId());
      return onward(Item::Of(Value::String(id)));
    }
    case RefKind::kNode: {
      ABSL_ASSIGN_OR_RETURN(const NodePtr node, LocalNode(ref));
      return ReadNode(node, /*tolerant=*/false, onward);
    }
    case RefKind::kCallPort: {
      ABSL_ASSIGN_OR_RETURN(CallHandle* handle, Call(one.call));
      ABSL_ASSIGN_OR_RETURN(const NodePtr node,
                            handle->Node(one.name, one.direction));
      return ReadNode(node, graph().steps[one.call].tolerant, onward);
    }
    case RefKind::kFlowPort: {
      ABSL_ASSIGN_OR_RETURN(const NodePtr node,
                            runner_->action()->GetInput(one.name));
      return ReadNode(node, /*tolerant=*/false, onward);
    }
    default:
      return Fail(absl::StrCat(one.label, " is not something a flow can read."));
  }
}

/// Read several streams in step, as one stream of tuples.
///
/// **No fibre of its own per source, and that is not a shortcut.** A tuple is
/// not complete until every source has answered, so asking them one after
/// another finishes at exactly the moment asking them at once would -- and every
/// reader here is an ordinary subscription, which already blocks, already
/// relays a producer's failure, and already has the buffering the analysis
/// arranged. A fibre per source would buy nothing and would put three more
/// lifetimes on the run's monitor.
///
/// A source that ends **well** is latched: from then on it contributes a null to
/// every tuple, which is what lets a short stream be zipped against a long one
/// without either being padded by its author. A source that ends **badly** ends
/// the whole thing with its status, because a tuple missing a value for a reason
/// nobody has been told about is worse than no tuple at all. It stops when every
/// source has ended.
absl::Status Scope::ProduceZip(RefId ref, Bus::Emit emit) {
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
        for (ReaderPtr& reader : readers) reader->Stop();
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
    if (running == 0) break;
    ABSL_RETURN_IF_ERROR(emit(Item::Of(Value::List(std::move(tuple)))));
  }
  return absl::OkStatus();
}

/// Read a derived ref's source and reshape it with one stage.
absl::Status Scope::ProduceStage(RefId ref, Bus::Emit emit) {
  const graph::Ref& one = graph().refs[ref];
  const graph::Stage& stage = one.stage;
  ABSL_ASSIGN_OR_RETURN(ReaderPtr source, Subscribe(one.source));
  HostBridge& host = bridge();
  const std::string& name = stage.name;

  const auto each =
      [&](absl::FunctionRef<absl::Status(const ItemPtr&)> body) -> absl::Status {
    while (true) {
      ABSL_ASSIGN_OR_RETURN(const ItemPtr item, source->Next());
      if (item == nullptr) return absl::OkStatus();
      ABSL_RETURN_IF_ERROR(body(item));
    }
  };

  if (name == "at") {
    const Value key = stage.indexed ? Value::Integer(stage.index)
                                    : Value::String(stage.text);
    if (stage.named_or_indexed) {
      // A destructuring `let`: the field where the value has one, and the
      // position where it is a list. `Lookup` answers by the *value's* kind, so
      // a record ignores an integer key and a list ignores a string one -- which
      // is why this asks twice rather than choosing once.
      const Value position = Value::Integer(stage.index);
      return each([&](const ItemPtr& item) -> absl::Status {
        ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
        Value found = Lookup(value, key);
        if (found.IsNull()) found = Lookup(value, position);
        return emit(Item::Of(std::move(found)));
      });
    }
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      return emit(Item::Of(Lookup(value, key)));
    });
  }
  if (name == "map") {
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      ABSL_ASSIGN_OR_RETURN(const Value mapped,
                            EvaluateWith(stage.expr, value));
      return emit(Item::Of(mapped));
    });
  }
  if (name == "where") {
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      ABSL_ASSIGN_OR_RETURN(const Value kept, EvaluateWith(stage.expr, value));
      if (!Truthy(kept)) return absl::OkStatus();
      return emit(item);
    });
  }
  if (name == "match") {
    // Compiled once: the pattern is written once in the source, and a bad one is
    // the flow's own mistake rather than something a value could fix.
    const pattern::Compiled compiled = pattern::Compile(stage.text);
    if (!compiled.ok()) {
      return Fail(absl::StrCat("The pattern '", stage.text,
                               "' cannot be read: ", compiled.error));
    }
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      const Value found = MatchCompiled(compiled.pattern, AsText(value));
      // A value the pattern does not fit is dropped, which is what makes this a
      // `where` and a `map` at once and what makes reading a log worth writing.
      if (found.IsNull()) return absl::OkStatus();
      return emit(Item::Of(found));
    });
  }
  if (name == "mime") {
    return each([&](const ItemPtr& item) -> absl::Status {
      if (!Matches(item->Mimetype(), stage.text)) return absl::OkStatus();
      return emit(item);
    });
  }
  if (name == "first") {
    if (stage.count <= 0) return absl::OkStatus();
    long long taken = 0;
    while (taken < stage.count) {
      ABSL_ASSIGN_OR_RETURN(const ItemPtr item, source->Next());
      if (item == nullptr) return absl::OkStatus();
      ABSL_RETURN_IF_ERROR(emit(item));
      ++taken;
    }
    source->Stop();
    return absl::OkStatus();
  }
  if (name == "last") {
    std::deque<ItemPtr> tail;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      tail.push_back(item);
      if (static_cast<long long>(tail.size()) > stage.count) tail.pop_front();
      return absl::OkStatus();
    }));
    for (const ItemPtr& item : tail) ABSL_RETURN_IF_ERROR(emit(item));
    return absl::OkStatus();
  }
  if (name == "drop") {
    long long seen = 0;
    return each([&](const ItemPtr& item) -> absl::Status {
      if (++seen <= stage.count) return absl::OkStatus();
      return emit(item);
    });
  }
  if (name == "truncate") {
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      return emit(Item::Of(Truncate(value, stage.count)));
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
      ABSL_RETURN_IF_ERROR(emit(Item::Of(Value::List(std::move(group)))));
      group.clear();
      return absl::OkStatus();
    }));
    if (group.empty()) return absl::OkStatus();
    return emit(Item::Of(Value::List(std::move(group))));
  }
  if (name == "chunk") {
    // The other direction from `let`: one value, cut into pieces of a size
    // somebody downstream cares about. An upload wants 64 KiB frames and a
    // model wants a paragraph, and both are "this one value, as a stream".
    //
    // A byte count, because that is what the sizes people write are about --
    // a frame, a buffer, a limit. Text is still cut on a character boundary:
    // half a code point is not a piece of text anybody can use, and backing off
    // a byte or two is cheaper than the bug.
    if (stage.count <= 0) {
      return Fail(absl::StrCat("'chunk ", stage.count,
                               "' is not a size; a chunk holds at least one "
                               "byte."));
    }
    const size_t size = static_cast<size_t>(stage.count);
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      if (!value.IsTextlike()) {
        // Nothing to cut. A list is `batch`'s business and everything else is
        // one value however it is looked at, so it goes through unchanged
        // rather than being refused.
        return emit(Item::Of(value));
      }
      const std::string& text = value.text();
      const bool bytes = value.kind() == Value::Kind::kBytes;
      size_t at = 0;
      while (at < text.size()) {
        size_t take = std::min(size, text.size() - at);
        if (!bytes) {
          // Back off to the start of the character this would have split.
          while (take > 1 && at + take < text.size() &&
                 (static_cast<unsigned char>(text[at + take]) & 0xC0) == 0x80) {
            --take;
          }
        }
        std::string piece = text.substr(at, take);
        ABSL_RETURN_IF_ERROR(emit(Item::Of(
            bytes ? Value::Bytes(std::move(piece))
                  : Value::String(std::move(piece)))));
        at += take;
      }
      return absl::OkStatus();
    });
  }
  if (name == "then") {
    // All of this one, then all of that one. Two writers to a node interleave by
    // arrival, which is fine for pages and wrong for a conversation; this is how
    // a flow says which comes first.
    ABSL_RETURN_IF_ERROR(each(
        [&](const ItemPtr& item) -> absl::Status { return emit(item); }));
    ABSL_ASSIGN_OR_RETURN(source, Subscribe(stage.stream));
    return each(
        [&](const ItemPtr& item) -> absl::Status { return emit(item); });
  }
  if (name == "group") {
    // `batch`, closed by a question rather than by a count: values pile up until
    // one of them says the group is finished, and the group goes on as a list.
    // What is left when the stream ends goes too -- a partial group is still what
    // was said.
    std::vector<Value> gathered;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      gathered.push_back(value);
      ABSL_ASSIGN_OR_RETURN(const Value closes,
                            EvaluateWith(stage.expr, value));
      if (!Truthy(closes)) return absl::OkStatus();
      ABSL_RETURN_IF_ERROR(emit(Item::Of(Value::List(std::move(gathered)))));
      gathered.clear();
      return absl::OkStatus();
    }));
    if (gathered.empty()) return absl::OkStatus();
    return emit(Item::Of(Value::List(std::move(gathered))));
  }
  if (name == "collect") {
    std::vector<Value> collected;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      collected.push_back(value);
      return absl::OkStatus();
    }));
    return emit(Item::Of(Value::List(std::move(collected))));
  }
  if (name == "count") {
    std::int64_t total = 0;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr&) -> absl::Status {
      ++total;
      return absl::OkStatus();
    }));
    return emit(Item::Of(Value::Integer(total)));
  }
  if (name == "distinct") {
    absl::flat_hash_set<std::string> seen;
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      if (!seen.insert(AsText(value)).second) return absl::OkStatus();
      return emit(item);
    });
  }
  if (name == "join") {
    std::vector<std::string> pieces;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      pieces.push_back(AsText(value));
      return absl::OkStatus();
    }));
    return emit(Item::Of(Value::String(absl::StrJoin(pieces, stage.text))));
  }
  if (name == "strformat") {
    // The one-value shorthand: `| strformat "took %s"` is exactly
    // `| map strformat("took %s", it)`, which is the shape almost every use of it
    // has. The full builtin is there when more than one value goes in.
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      const Value arguments[] = {value};
      return emit(Item::Of(Value::String(
          Strformat(Value::String(stage.text), arguments))));
    });
  }
  if (name == "text") {
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      return emit(Item::Of(Value::String(AsText(value))));
    });
  }
  if (name == "json") {
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      return emit(Item::Of(AsJson(value)));
    });
  }
  if (name == "packb") {
    // An item read from a node keeps the producer's bytes, so a value that
    // arrived packed is passed on untouched, tag and all. Anything else is
    // decoded once and re-encoded, which is the only point at which a flow pays
    // for the conversion.
    return each([&](const ItemPtr& item) -> absl::Status {
      if (item->chunk().has_value() &&
          BaseMimetype(item->Mimetype()) == data::kMsgpackMimetype) {
        return emit(item);
      }
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      ABSL_ASSIGN_OR_RETURN(data::Chunk chunk,
                            host.ToChunk(value, data::kMsgpackMimetype));
      return emit(Item::OfChunk(std::move(chunk)));
    });
  }
  return Fail(absl::StrCat("Unknown stage '", name, "'."));
}

// --- Outcomes ----------------------------------------------------------------

absl::StatusOr<ItemPtr> Scope::StatusItem(RefId ref) {
  const graph::Ref& one = graph().refs[ref];
  absl::Status outcome;
  if (one.subject_step != kNone &&
      graph().steps[one.subject_step].kind == StepKind::kBlock) {
    // A block's outcome is recorded by the block itself, so reading it is
    // waiting for the block to be over and then looking. Waiting for a step is
    // what `after` does, and this is the same wait.
    ABSL_RETURN_IF_ERROR(StepDone(one.subject_step));
    for (Scope* at = this; at != nullptr; at = at->parent_) {
      thread::MutexLock lock(&monitor().mu());
      const auto found = at->outcomes_.find(one.subject_step);
      if (found != at->outcomes_.end()) return Item::Of(StatusRecord(found->second));
    }
    return Item::Of(StatusRecord(absl::OkStatus()));
  }
  if (one.subject_step != kNone) {
    ABSL_ASSIGN_OR_RETURN(CallHandle* handle, Call(one.subject_step));
    ABSL_RETURN_IF_ERROR(handle->Outcome(&outcome));
    return Item::Of(StatusRecord(outcome));
  }
  if (one.subject != kNone) {
    ABSL_RETURN_IF_ERROR(NodeOutcome(one.subject, &outcome));
    return Item::Of(StatusRecord(outcome));
  }
  return Fail(absl::StrCat(one.label, " has no status to read."));
}

/// A node's outcome: its writers are done, or its stream has ended.
///
/// A node this flow writes is finished when the last writer has closed it, so the
/// status is the one the node was closed or aborted with. One it only reads is
/// finished when the stream ends, and the status is the reader's -- which is how
/// an output cut off mid-stream gets noticed.
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
    if (*item == nullptr) break;
  }
  ABSL_ASSIGN_OR_RETURN(const NodePtr node, ReadableNode(ref));
  *outcome = node == nullptr ? absl::OkStatus() : node->GetReaderStatus();
  return absl::OkStatus();
}

// --- Expressions -------------------------------------------------------------

absl::StatusOr<Value> Scope::ValueOf(RefId ref) {
  Scope* owner = Owner(ref);
  ValueCursor* cursor = nullptr;
  {
    thread::MutexLock lock(&monitor().mu());
    // One cursor per ref, and one *subscription* per ref. The plan set aside a
    // single reader slot for all the value reads of a ref together, so two steps
    // arriving here at once must not both take it -- checking for the cursor and
    // then subscribing is not enough, because both would find nothing and both
    // would subscribe, and the second would be told the stream has more readers
    // than the plan accounted for. Whoever arrives first makes it; the rest wait
    // for it to be there.
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
    {
      thread::MutexLock lock(&monitor().mu());
      // Whatever happened, this ref is no longer being opened: a failure that
      // left the flag set would leave every other reader of it waiting for a
      // cursor nobody is making any more.
      owner->opening_cursors_.erase(ref);
      if (reader.ok()) {
        const graph::Ref& one = graph().refs[ref];
        auto made = std::make_unique<ValueCursor>(bridge(), *std::move(reader),
                                                  one.unary, one.label);
        cursor = made.get();
        owner->cursors_.emplace(ref, std::move(made));
      }
    }
    monitor().Wake();
    if (!reader.ok()) return reader.status();
  }
  return cursor->Next(monitor());
}

absl::StatusOr<Value> Scope::Evaluate(ExprId expr) {
  return EvaluateWith(expr, Value::Null());
}

absl::StatusOr<Value> Scope::EvaluateWith(ExprId expr, const Value& it) {
  if (expr == kNone) return Value::Null();
  const graph::Expr& one = graph().exprs[expr];
  if (one.node == nullptr) return Value::Null();
  absl::flat_hash_map<const syntax::Node*, Value> bound;
  // One value per *ref* per evaluation, not per mention of it: `strformat("%s %s",
  // x, x)` names one value twice and must not take two off the stream. Across
  // statements they are separate reads, which is the whole point.
  absl::flat_hash_map<RefId, Value> taken;
  for (const auto& [node, ref] : one.bound) {
    const auto held = taken.find(ref);
    if (held != taken.end()) {
      bound[node] = held->second;
      continue;
    }
    ABSL_ASSIGN_OR_RETURN(Value value, ValueOf(ref));
    taken.emplace(ref, value);
    bound[node] = std::move(value);
  }
  EvalContext context;
  context.bound = &bound;
  context.it = it;
  context.has_it = true;
  context.bridge = &bridge();
  context.shapes = &shapes();
  return flow::Evaluate(*one.node, context);
}

// --- Execution ---------------------------------------------------------------

absl::Status Scope::Run() {
  ABSL_RETURN_IF_ERROR(Prepare());
  Group group(monitor());
  for (const RefId ref : unwritten_) {
    group.Spawn([this, ref]() -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const NodePtr node, LocalNode(ref));
      return CloseNode(node);
    });
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
    group.Spawn([this, step]() -> absl::Status { return RunStep(step); });
  }
  return group.Join();
}

void Scope::MarkDone(StepId step) {
  {
    thread::MutexLock lock(&monitor().mu());
    done_[step] = true;
  }
  monitor().Wake();
}

absl::Status Scope::StepDone(StepId step) {
  for (Scope* at = this; at != nullptr; at = at->parent_) {
    const auto found = at->done_.find(step);
    if (found == at->done_.end()) continue;
    thread::MutexLock lock(&monitor().mu());
    return monitor().Wait(
        [at, step] { return at->done_.find(step)->second; });
  }
  return Fail(absl::StrCat("Internal flow error: ", graph().steps[step].label,
                           " is not in scope."));
}

absl::Status Scope::RunStep(StepId step) {
  absl::Status status;
  for (const StepId dependency : graph().steps[step].after) {
    status = StepDone(dependency);
    if (!status.ok()) break;
  }
  if (status.ok()) status = Execute(step);
  const auto held = analysis_.held.find(step);
  if (held != analysis_.held.end()) {
    for (const RefId ref : held->second) {
      if (absl::StatusOr<Destination*> destination = DestinationOf(ref);
          destination.ok()) {
        (*destination)->Release().IgnoreError();
      }
    }
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
      ABSL_ASSIGN_OR_RETURN(Destination* destination,
                            DestinationOf(one.destination));
      ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(one.source));
      while (true) {
        ABSL_ASSIGN_OR_RETURN(const ItemPtr item, reader->Next());
        if (item == nullptr) return absl::OkStatus();
        ABSL_RETURN_IF_ERROR(destination->Write(item, &bridge()));
      }
    }
    case StepKind::kSkip: {
      // With a count the values are already gone: it was applied where the
      // stream is produced. Reading here would take a reader slot this step was
      // never counted for.
      if (one.count.has_value()) return absl::OkStatus();
      ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(one.source));
      while (true) {
        ABSL_ASSIGN_OR_RETURN(const ItemPtr item, reader->Next());
        if (item == nullptr) return absl::OkStatus();
      }
    }
    case StepKind::kCapture: {
      ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(one.source));
      ABSL_ASSIGN_OR_RETURN(const ItemPtr item, reader->Next());
      Value value;
      if (item != nullptr) {
        ABSL_ASSIGN_OR_RETURN(value, item->Read(&bridge()));
      }
      reader->Stop();
      thread::MutexLock lock(&monitor().mu());
      captures_.insert_or_assign(one.slot, value);
      return absl::OkStatus();
    }
    case StepKind::kWait:
    case StepKind::kDrain:
      return RunWait(step);
    case StepKind::kCancel: {
      if (one.target == kNone) return absl::OkStatus();
      ABSL_ASSIGN_OR_RETURN(CallHandle* handle, Call(one.target));
      ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<actions::Action> action,
                            handle->Action());
      return action->Cancel();
    }
    case StepKind::kFail:
      return Failure(step);
    case StepKind::kBlock: {
      // The same nesting an `if` branch runs in. What a block adds is that its
      // outcome is *its own*: a failure inside it is the block's, and a `try`
      // says the flow means to read it rather than end there.
      if (one.bodies.empty()) return absl::OkStatus();
      const absl::Status ran =
          Scope(*runner_, one.bodies.front(), this, {}).Run();
      {
        thread::MutexLock lock(&monitor().mu());
        outcomes_[step] = ran;
      }
      monitor().Wake();
      return one.tolerant ? absl::OkStatus() : ran;
    }
    case StepKind::kIf: {
      ABSL_ASSIGN_OR_RETURN(const Value taken, Evaluate(one.condition));
      const BodyId body = Truthy(taken) ? one.bodies.front() : one.bodies.back();
      if (graph().bodies[body].steps.empty()) return absl::OkStatus();
      return Scope(*runner_, body, this, {}).Run();
    }
    case StepKind::kForEach:
      return RunForEach(step);
    case StepKind::kRepeat:
      return RunRepeat(step);
  }
  return Fail(absl::StrCat("Cannot run a ", graph::StepKindName(one.kind), "."));
}

/// Read the subject's status, and let a bad one through when it is ours.
///
/// A subject a flow said it would handle -- a `try` step -- reports and the flow
/// carries on. Anything else that finished badly ends the flow here, with the
/// status it finished with rather than a new one.
absl::Status Scope::RunWait(StepId step) {
  const graph::Step& one = graph().steps[step];
  Value record;
  if (one.timeout.has_value() && *one.timeout < absl::InfiniteDuration()) {
    // Reading the outcome blocks on whatever the subject is doing, so a timeout
    // is a race against that read rather than something the read itself knows
    // about. The read runs in a group so that losing the race ends it: a fibre
    // left holding a pointer to this scope after the statement has failed and
    // the flow has been torn down is a crash, not a leak.
    struct Waiting {
      Monitor* absl_nonnull monitor;
      bool ready = false;
      absl::Status status;
      Value value;
    };
    Monitor& monitor = this->monitor();
    auto waiting = std::make_shared<Waiting>(Waiting{.monitor = &monitor});
    const RefId outcome = one.outcome;
    Group reading(monitor);
    reading.Spawn(
        [this, outcome, waiting]() -> absl::Status {
          absl::Status status;
          Value value;
          absl::StatusOr<ReaderPtr> reader = Subscribe(outcome);
          if (!reader.ok()) {
            status = reader.status();
          } else if (absl::StatusOr<ItemPtr> item = (*reader)->Next();
                     !item.ok()) {
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
      return Fail(absl::StrCat("Waiting for ", SubjectOf(one), " timed out "
                               "after ", absl::FormatDuration(*one.timeout),
                               "."),
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
  if (one.tolerant || record.kind() != Value::Kind::kObject) {
    return absl::OkStatus();
  }
  const Value* ok = record.Get("ok");
  if (ok != nullptr && Truthy(*ok)) return absl::OkStatus();
  return StatusOfRecord(record);
}

/// The status a `fail` statement raises.
absl::Status Scope::Failure(StepId step) {
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
  if (resolved == absl::StatusCode::kOk) resolved = absl::StatusCode::kInternal;
  const std::string text = has_message ? AsText(message) : "";
  return Fail(text.empty() ? absl::StrCat(runner_->plan().name, " failed.")
                           : text,
              resolved);
}

absl::Status Scope::RunForEach(StepId step) {
  const graph::Step& one = graph().steps[step];
  const BodyId body = one.bodies.front();
  ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(one.source));
  Group group(monitor());
  const int parallel = std::max(one.parallel, 1);
  std::int64_t index = 0;
  Monitor& monitor = this->monitor();
  auto running = std::make_shared<int>(0);
  while (true) {
    ABSL_ASSIGN_OR_RETURN(const ItemPtr item, reader->Next());
    if (item == nullptr) break;
    absl::flat_hash_map<RefId, std::vector<ItemPtr>> presets;
    if (one.item != kNone) presets[one.item] = {item};
    if (one.index != kNone) {
      presets[one.index] = {Item::Of(Value::Integer(index))};
    }
    ++index;
    if (parallel == 1) {
      ABSL_RETURN_IF_ERROR(Scope(*runner_, body, this, std::move(presets)).Run());
      continue;
    }
    // Admission before the fibre, so a wide `parallel` does not turn a long
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
  // condition: a loop that says `until` means it, however many passes that takes.
  for (int index = 0;
       !one.max_iterations.has_value() || index < *one.max_iterations; ++index) {
    absl::flat_hash_map<RefId, std::vector<ItemPtr>> presets;
    if (one.carry != kNone) presets[one.carry] = {Item::Of(carried)};
    if (one.index != kNone) {
      presets[one.index] = {Item::Of(Value::Integer(index))};
    }
    Scope pass(*runner_, body, this, std::move(presets));
    ABSL_RETURN_IF_ERROR(pass.Run());
    if (one.condition != kNone) {
      const graph::Expr& condition = graph().exprs[one.condition];
      absl::flat_hash_map<const syntax::Node*, Value> bound;
      for (const auto& [node, ref] : condition.bound) {
        const auto found =
            pass.captures().find(absl::StrCat("condition:", ref));
        bound[node] =
            found == pass.captures().end() ? Value::Null() : found->second;
      }
      EvalContext context;
      context.bound = &bound;
      context.bridge = &bridge();
  context.shapes = &shapes();
      ABSL_ASSIGN_OR_RETURN(const Value holds,
                            flow::Evaluate(*condition.node, context));
      if (Truthy(holds) == one.stop_when) return absl::OkStatus();
    }
    if (one.carry_source != kNone) {
      const auto found = pass.captures().find("carry");
      carried = found == pass.captures().end() ? Value::Null() : found->second;
    }
  }
  return absl::OkStatus();
}

// --- Calls -------------------------------------------------------------------

absl::Status Runner::Resolve(const std::string& name,
                             actions::ActionSchema* schema,
                             actions::ActionHandler* handler) {
  if (const ResolvedFlow* sibling = program_->Flow(name); sibling != nullptr) {
    ABSL_ASSIGN_OR_RETURN(*schema, FlowSchema(sibling->plan));
    // A flow of this program that this flow runs is still the same client's, so
    // its own `call` steps belong on the same stream. It cannot inherit that: a
    // `run` step's action is bound to no stream precisely so its nodes stay off
    // the wire.
    ABSL_ASSIGN_OR_RETURN(
        *handler, MakeHandler(program_, name,
                              RunOptions{.bridge = bridge_,
                                         .dispatch_stream = dispatch_stream_}));
    return absl::OkStatus();
  }
  const std::shared_ptr<actions::ActionRegistry> registry =
      action_->GetRegistry();
  if (registry == nullptr || !registry->IsRegistered(name)) {
    std::vector<std::string> known =
        registry == nullptr ? std::vector<std::string>{}
                            : registry->ListRegisteredActions();
    std::sort(known.begin(), known.end());
    return Fail(
        absl::StrCat(plan().name, " calls '", name,
                     "', which is not a flow in this program and is not "
                     "registered here (registered: ",
                     known.empty() ? "none" : absl::StrJoin(known, ", "), ")."),
        absl::StatusCode::kNotFound);
  }
  ABSL_ASSIGN_OR_RETURN(*schema, registry->GetSchema(name));
  // Registered without one: the registry reports that as an error rather than as
  // an empty handler, so asking is how it is found out.
  if (absl::StatusOr<actions::ActionHandler> found = registry->GetHandler(name);
      found.ok()) {
    *handler = *std::move(found);
  }
  return absl::OkStatus();
}

absl::Status Runner::RunCall(Scope& scope, StepId step) {
  ABSL_ASSIGN_OR_RETURN(CallHandle* handle, scope.Call(step));
  if (const absl::Status started = StartCall(scope, step, *handle);
      !started.ok()) {
    // A step that never started still has to say so: everything wiring itself to
    // this call waits for it, and an unanswered wait is a deadlock rather than a
    // failure anybody can see.
    handle->NeverStarted(started);
    return started;
  }
  return PumpCall(step, *handle);
}

absl::Status Runner::StartCall(Scope& scope, StepId step, CallHandle& handle) {
  const graph::Step& one = graph().steps[step];
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
  if (!one.node_map.empty()) {
    ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<nodes::NodeMap> map,
                          NodeMapNamed(one.node_map));
    ABSL_RETURN_IF_ERROR(nested->BindNodeMap(map));
  }
  if (!local && dispatch_stream_ != nullptr) {
    // The flow is a client's, and this call is the peer's: give it the stream the
    // flow itself deliberately does not hold.
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
        name, value.kind() == Value::Kind::kBytes ? value.text()
                                                  : AsText(value)));
  }
  if (one.action_id != kNone) {
    ABSL_ASSIGN_OR_RETURN(const Value value, scope.Evaluate(one.action_id));
    ABSL_RETURN_IF_ERROR(nested->SetId(AsText(value)));
  }

  // Create every port node before the action starts, so a reader that subscribes
  // later still sees the whole stream from its beginning.
  absl::flat_hash_set<std::string> written;
  for (const RefId ref : scope.analysis_.destinations) {
    const graph::Ref& port = graph().refs[ref];
    if (port.kind == RefKind::kCallPort && port.call == step) {
      written.insert(port.name);
    }
  }
  absl::flat_hash_set<std::string> read;
  for (const RefId ref : scope.analysis_.refs) {
    const graph::Ref& port = graph().refs[ref];
    if (port.kind != RefKind::kCallPort || port.call != step) continue;
    if (port.direction != syntax::PortDirection::kOutput) continue;
    const auto readers = scope.analysis_.readers.find(ref);
    if (readers != scope.analysis_.readers.end() && readers->second > 0) {
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
    if (!read.contains(name)) undrained.push_back(node);
  }
  handle.unclosed = std::move(unclosed);
  handle.undrained = std::move(undrained);
  handle.Started(nested);
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
    return start;
  }
  {
    Group group(monitor_);
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
          if (!fragment->has_value()) return absl::OkStatus();
        }
      });
    }
    group.Spawn([this, step, &handle]() -> absl::Status {
      return AwaitCall(graph().steps[step], handle);
    });
    const absl::Status status = group.Join();
    handle.Done();
    if (!status.ok()) return status;
  }
  if (const absl::Status error = handle.error();
      !error.ok() && !one.tolerant) {
    return error;
  }
  return absl::OkStatus();
}

absl::Status Runner::AwaitCall(const graph::Step& step, CallHandle& handle) {
  const std::shared_ptr<actions::Action> action = handle.action_now();
  if (action == nullptr) return absl::OkStatus();
  const absl::Status status =
      action->Wait(step.timeout.value_or(absl::InfiniteDuration()))
          .Await()
          .status();
  if (status.ok()) return absl::OkStatus();
  if (absl::IsCancelled(status) && action->Cancelled() &&
      !action_->Cancelled()) {
    // A `cancel` statement cancels the call, which cancels the wait for it. That
    // is this call's outcome, not the flow being cancelled -- unless the flow
    // really is going away too.
    handle.SetError(action->GetStatus());
    return absl::OkStatus();
  }
  handle.SetError(status);
  return absl::OkStatus();
}

}  // namespace

// --- The public face ---------------------------------------------------------

absl::StatusOr<std::shared_ptr<CompiledProgram>> CompiledProgram::Compile(
    std::string source, std::string source_name) {
  auto compiled = std::shared_ptr<CompiledProgram>(new CompiledProgram());
  compiled->source_ = std::move(source);
  compiled->source_name_ = std::move(source_name);
  compiled->parsed_ = Parse(compiled->source_);
  compiled->resolved_ =
      Resolve(compiled->source_, compiled->parsed_, /*build_graph=*/true);
  compiled->resolved_.program.source_name = compiled->source_name_;
  if (const Diagnostic* first = compiled->resolved_.FirstError();
      first != nullptr) {
    return Fail(absl::StrCat(
        compiled->source_name_.empty() ? "<flow>" : compiled->source_name_, ":",
        first->range.start.line, ":", first->range.start.column, ": ",
        first->message));
  }
  return compiled;
}

const ResolvedFlow* absl_nullable CompiledProgram::Flow(
    std::string_view name) const {
  for (const ResolvedFlow& flow : resolved_.flows) {
    if (flow.plan.name == name) return &flow;
  }
  return nullptr;
}

namespace {

/// What a port's declared type is called in an [actions::ActionSchema].
///
/// A schema's `type` is the *host's* name for the payload, because that is what a
/// caller and a model are shown -- and A11's own schemas spell a built-in with the
/// Python type's name, so a flow's ports have to as well or a composition would
/// describe itself differently from a hand-written action. A tag and a mimetype go
/// through as they were written: they already name a concrete thing.
std::string SchemaType(std::string_view declared) {
  static const auto* const kNames =
      new absl::flat_hash_map<std::string_view, std::string_view>{
          {"string", "str"},   {"text", "str"},     {"number", "float"},
          {"integer", "int"},  {"int", "int"},      {"bool", "bool"},
          {"boolean", "bool"}, {"object", "dict"},  {"json", "dict"},
          {"list", "list"},    {"array", "list"},   {"bytes", "bytes"},
          {"duration", "a11.Duration"}, {"time", "a11.Time"},
          {"any", "application/json"},
      };
  const auto found = kNames->find(declared);
  return std::string(found == kNames->end() ? declared : found->second);
}

}  // namespace

absl::StatusOr<actions::ActionSchema> FlowSchema(const FlowPlan& plan) {
  actions::ActionSchema schema;
  schema.name = plan.name;
  schema.description = plan.description;
  for (const PortPlan& port : plan.ports) {
    actions::ActionPortSchema entry;
    entry.name = port.name;
    entry.type = SchemaType(port.type);
    entry.description = port.description;
    entry.required = port.required;
    entry.unary = port.unary;
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
    schema.headers.emplace(header.name, std::move(entry));
  }
  ABSL_RETURN_IF_ERROR(schema.Validate());
  return schema;
}

absl::StatusOr<actions::ActionHandler> MakeHandler(
    std::shared_ptr<const CompiledProgram> program, std::string_view flow,
    RunOptions options) {
  if (program == nullptr) return Fail("There is no program to run.");
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
  if (bridge == nullptr) bridge = NativeHostBridge();
  return actions::MakeAsyncActionHandler(
      [program = std::move(program), name = std::string(flow),
       bridge = std::move(bridge),
       stream = std::move(options.dispatch_stream)](
          std::shared_ptr<actions::Action> action) -> absl::Status {
        const ResolvedFlow* one = program->Flow(name);
        if (one == nullptr) {
          return Fail(absl::StrCat("No flow named '", name, "' any more."),
                      absl::StatusCode::kNotFound);
        }
        Runner runner(program, *one, std::move(action), bridge, stream);
        return runner.Run();
      });
}

}  // namespace a11::flow
