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

#ifndef A11_FLOW_INTERNAL_RUNTIME_TRANSPORT_H_
#define A11_FLOW_INTERNAL_RUNTIME_TRANSPORT_H_

#include "a11/flow/internal/runtime_monitor.h"

namespace a11::flow::internal {

/// One value travelling through a pipe.
///
/// An item read from a node keeps its native [data::Chunk], so a pipe that only
/// moves values re-writes exactly the producer's bytes and mimetype and never
/// pays for a round trip through the host. A stage that looks at the value
/// decodes it once, and every reader of the same item shares that.
class Item {
 public:
  static std::shared_ptr<Item> OfChunk(data::Chunk chunk);

  static std::shared_ptr<Item> Of(Value value);

  const std::optional<data::Chunk>& chunk() const { return chunk_; }

  std::string Mimetype() const;

  /// The decoded value, read out of the chunk the first time it is asked for.
  absl::StatusOr<Value> Read(HostBridge* absl_nonnull bridge) const;

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
  void Prime(absl::StatusOr<Value> result) const;

 private:
  std::optional<data::Chunk> chunk_;
  mutable thread::Mutex mu_;
  mutable absl::StatusOr<Value> result_;
  mutable bool decoded_ = false;
};

/// One reader's view of a stream.
class Reader {
 public:
  virtual ~Reader() = default;

  /// The next item, or a null pointer at the end of the stream.
  ///
  /// A non-ok status is the producer's failure, relayed to every reader.
  virtual absl::StatusOr<ItemPtr> Next() = 0;

  /// Append up to `limit` currently available items, waiting only when none is
  /// ready.
  ///
  /// Appending nothing means the stream has ended, as a null item does for
  /// [Next].
  virtual absl::Status NextMany(std::vector<ItemPtr>& out, size_t limit);

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

/// A reader over a fixed list of items: a preset, a buffer, a lazy value.
class ListReader : public Reader {
 public:
  explicit ListReader(std::vector<ItemPtr> items) : items_(std::move(items)) {}

  absl::StatusOr<ItemPtr> Next() override;

  absl::Status NextMany(std::vector<ItemPtr>& out, size_t limit) override;

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
  absl::Status Many(std::vector<ItemPtr>& items) const;

 private:
  OneFn one_;
  std::optional<ManyFn> many_;
};

/// One item and an independent terminal state, shared by all subscriptions.
class SingleSlot : public std::enable_shared_from_this<SingleSlot> {
 public:
  using Produce = std::function<absl::Status(const Sink&)>;

  SingleSlot(Monitor& monitor, Produce produce, size_t readers, bool replay,
             bool inline_producer, std::shared_ptr<TransportStats> stats)
      : monitor_(&monitor),
        changed_(monitor),
        produce_(std::move(produce)),
        expected_(readers),
        replay_(replay),
        inline_(inline_producer),
        stats_(std::move(stats)) {}

  absl::StatusOr<ReaderPtr> Take();

  void Wanted();

  bool Skip();

  absl::Status Pump();

  bool inline_producer() const { return inline_; }

  bool replay() const { return replay_; }

 private:
  class Subscription;

  void Retire() ABSL_EXCLUSIVE_LOCKS_REQUIRED(monitor_->mu()) {
    if (!replay_ && ++retired_ == expected_) {
      item_.reset();
    }
  }

  void Start();

  Monitor* absl_nonnull monitor_;
  Monitor::Condition changed_;
  Produce produce_;
  size_t expected_;
  bool replay_;
  bool inline_;
  std::shared_ptr<TransportStats> stats_;
  size_t handed_out_ ABSL_GUARDED_BY(monitor_->mu()) = 0;
  size_t retired_ ABSL_GUARDED_BY(monitor_->mu()) = 0;
  bool wanted_ ABSL_GUARDED_BY(monitor_->mu()) = false;
  bool started_ ABSL_GUARDED_BY(monitor_->mu()) = false;
  bool published_ ABSL_GUARDED_BY(monitor_->mu()) = false;
  bool ended_ ABSL_GUARDED_BY(monitor_->mu()) = false;
  ItemPtr item_ ABSL_GUARDED_BY(monitor_->mu());
  absl::Status error_ ABSL_GUARDED_BY(monitor_->mu());
};

/// A stream with a fixed number of readers, fed by one producer.
class Bus {
 public:
  using Produce = std::function<absl::Status(const Sink&)>;

  Bus(Monitor& monitor, std::string label, Produce produce, int readers);

  const std::string& label() const { return label_; }

  /// One reader's view. Fails when the plan accounted for fewer readers.
  absl::StatusOr<ReaderPtr> Take();

  /// Read the stream once and fan it out until it ends.
  absl::Status Pump();

  void TrackInputPump(const a11::Task& task);

  void StopAbandonedInput();

  absl::Status CompletePump(absl::Status status);

  absl::Status WaitFinished();

  bool Skip();

  void Wanted();

  Monitor& monitor() { return *monitor_; }

  /// Everything parked on this stream: its readers, and its producer waiting
  /// for room. One condition for both, because both wait on the same slots.
  Monitor::Condition& moved() { return moved_; }

 private:
  friend class BusReader;

  bool Abandoned() ABSL_EXCLUSIVE_LOCKS_REQUIRED(monitor_->mu());

  bool finished_ = false;
  a11::Task input_pump_ ABSL_GUARDED_BY(monitor_->mu());

  Monitor* absl_nonnull monitor_;
  Monitor::Condition moved_;
  std::string label_;
  Produce produce_;
  std::vector<std::unique_ptr<Slot>> slots_;
  size_t handed_out_ = 0;
  bool wanted_ = false;
};

/// A single value computed the first time it is asked for, then shared.
///
/// What a status is: reading one waits for its subject and may end a node, so
/// it happens when a step asks, and once however many steps ask.
class Lazy {
 public:
  using Produce = std::function<absl::StatusOr<ItemPtr>()>;

  Lazy(Monitor& monitor, Produce produce)
      : monitor_(&monitor), produce_(std::move(produce)) {}

  absl::StatusOr<ReaderPtr> Replay();

 private:
  absl::StatusOr<ItemPtr> Get();

  Monitor* absl_nonnull monitor_;
  Produce produce_;
  bool running_ = false;
  bool ready_ = false;
  absl::StatusOr<ItemPtr> result_;
};

/// Incrementally buffers a stream and replays it to each nested reader.
///
/// Loops and branches use this for outer refs so every pass sees the same
/// values as they arrive. The buffer retains the stream for the body's
/// lifetime.
class Buffer {
 public:
  /// Shared storage retained by readers that outlive the creating scope.
  class State;

  Buffer(Monitor& monitor, ReaderPtr source);

  /// Read the source, publishing each item as it arrives.
  absl::Status Fill();

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

  absl::StatusOr<std::optional<Value>> Extract(Monitor& monitor);

  // Return the next value, or the cached value for a unary stream. Readers
  // reserve a turn under the monitor lock and perform the blocking read after
  // releasing it.
  /// Return the next value, or the cached value for a unary stream.
  ///
  /// Readers reserve a turn under the monitor lock and perform the blocking
  /// read after releasing it.
  absl::StatusOr<Value> Next(Monitor& monitor);

 private:
  /// One value off the stream, and for a unary one the promise checked.
  absl::StatusOr<Value> Read();

  HostBridge* absl_nonnull bridge_;
  ReaderPtr reader_;
  bool unary_ = false;
  std::string label_;
  /// Whether a read is in progress, so the next caller waits its turn.
  bool busy_ = false;
  bool read_ = false;
  Value held_;
};

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

  absl::StatusOr<NodePtr> Node();

  void Watch(const void* absl_nonnull owner, StepId step, const a11::Task& task,
             bool exclusive);

  void StopFeeding(const absl::Status& status);

  bool AcceptStoppedFeed() {
    thread::MutexLock lock(&monitor_->mu());
    return feeds_stopped_;
  }

  void Unwatch(const void* absl_nonnull owner, StepId step);

  absl::Status CheckWritable();

  absl::Status failure() {
    thread::MutexLock lock(&monitor_->mu());
    return failure_;
  }

  /// Append one value, as the producer wrote it wherever possible.
  absl::Status Write(const ItemPtr& item, HostBridge* absl_nonnull bridge) {
    return WriteRange(absl::MakeConstSpan(&item, 1), bridge);
  }

  /// Append a batch in order after the writer queue admits it.
  ///
  /// Admission provides backpressure. Closing the destination drains pending
  /// confirmations and reports any store failure.
  absl::Status WriteBatch(absl::Span<const ItemPtr> items,
                          HostBridge* absl_nonnull bridge) {
    return WriteRange(items, bridge);
  }

  absl::Status WriteMany(std::vector<ItemPtr>& items,
                         HostBridge* absl_nonnull bridge);

 private:
  absl::Status WriteRange(absl::Span<const ItemPtr> items,
                          HostBridge* absl_nonnull bridge);

 public:
  /// Close the node now, whoever was writing it.
  ///
  /// What `close` does to a node the flow does not write itself: a callee given
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
  /// `close` reports an ordinary end; this reports a failure so readers can
  /// distinguish completion from interruption.
  ///
  /// Forced, like End(): whoever was writing it, this is the last word. It also
  /// marks the node finished, so the ordinary Release() every step does on its
  /// way out finds nothing left to close.
  absl::Status Abort(absl::Status reason);

  absl::Status Finished();

 private:
  absl::Status Enter(bool cleanup = false);

  void Leave();

  absl::Status Finish(bool forced, absl::Status reason = absl::OkStatus());

  Monitor* absl_nonnull monitor_;
  /// Writers taking turns on the node, and whoever waits for it to be over.
  Monitor::Condition turn_;
  std::string label_;
  Open open_;

  struct Feeding {
    const void* absl_nonnull owner;
    StepId step;
    a11::Task task;
    bool exclusive;
  };

  absl::InlinedVector<Feeding, 1> feeding_ ABSL_GUARDED_BY(monitor_->mu());
  absl::Status failure_ ABSL_GUARDED_BY(monitor_->mu());
  int writers_ = 0;
  bool tolerant_ = false;
  bool feeds_stopped_ ABSL_GUARDED_BY(monitor_->mu()) = false;
  bool writing_ = false;
  bool closed_ = false;
  bool finished_ = false;
  bool opening_ = false;
  bool opened_ = false;
  absl::StatusOr<NodePtr> node_;
};

}  // namespace a11::flow::internal
#endif  // A11_FLOW_INTERNAL_RUNTIME_TRANSPORT_H_
