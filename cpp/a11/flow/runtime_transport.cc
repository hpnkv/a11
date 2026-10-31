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

#include "a11/flow/internal/runtime_transport.h"

namespace a11::flow::internal {

class SingleSlot::Subscription : public Reader {
 public:
  explicit Subscription(std::shared_ptr<SingleSlot> slot)
      : slot_(std::move(slot)) {}

  ~Subscription() override { Stop(); }

  absl::StatusOr<ItemPtr> Next() override { return Read(std::nullopt); }

  absl::StatusOr<ItemPtr> NextUntil(absl::Time deadline) override {
    // ReplayReader retains the untimed Reader fallback.
    return Read(slot_->replay_ ? std::nullopt
                               : std::optional<absl::Time>(deadline));
  }

  void Stop() override {
    if (stopped_) {
      return;
    }
    stopped_ = true;
    {
      thread::MutexLock lock(&slot_->monitor_->mu());
      if (!consumed_) {
        slot_->Retire();
      }
    }
    slot_->Wanted();
    // Teardown must not evaluate an initializer after its work has ended.
    // Inline production starts only in Read; literals have no stop effects.
  }

 private:
  absl::StatusOr<ItemPtr> Read(std::optional<absl::Time> deadline) {
    if (stopped_) {
      return ItemPtr{};
    }
    slot_->Wanted();
    if (slot_->inline_) {
      slot_->Start();
    }
    thread::MutexLock lock(&slot_->monitor_->mu());
    const auto ready =
        [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(slot_->monitor_->mu()) {
          return (!consumed_ && slot_->published_) || slot_->ended_;
        };
    if (deadline.has_value()) {
      ABSL_RETURN_IF_ERROR(
          slot_->monitor_->WaitUntil(slot_->changed_, *deadline, ready));
    } else {
      ABSL_RETURN_IF_ERROR(slot_->monitor_->Wait(slot_->changed_, ready));
    }
    if (!consumed_ && slot_->published_) {
      ItemPtr item = slot_->item_;
      consumed_ = true;
      slot_->Retire();
      return item;
    }
    ABSL_RETURN_IF_ERROR(slot_->error_);
    return ItemPtr{};
  }

  std::shared_ptr<SingleSlot> slot_;
  bool consumed_ = false;
  bool stopped_ = false;
};

class BusReader : public Reader {
 public:
  BusReader(Bus& bus, Slot& slot) : bus_(&bus), slot_(&slot) {}

  ~BusReader() override { Stop(); }

  absl::StatusOr<ItemPtr> Next() override;

  absl::StatusOr<ItemPtr> NextUntil(absl::Time deadline) override;

  absl::Status NextMany(std::vector<ItemPtr>& out, size_t limit) override;

  void Stop() override;

 private:
  Bus* absl_nonnull bus_;
  Slot* absl_nonnull slot_;
};

class Buffer::State {
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

class ReplayReader : public Reader {
 public:
  explicit ReplayReader(std::shared_ptr<Buffer::State> state)
      : state_(std::move(state)) {}

  absl::StatusOr<ItemPtr> Next() override;

  void Stop() override { stopped_ = true; }

 private:
  std::shared_ptr<Buffer::State> state_;
  size_t at_ = 0;
  bool stopped_ = false;
};

std::shared_ptr<Item> Item::OfChunk(data::Chunk chunk) {
  auto item = std::make_shared<Item>();
  item->chunk_ = std::move(chunk);
  return item;
}

std::shared_ptr<Item> Item::Of(Value value) {
  auto item = std::make_shared<Item>();
  item->result_ = std::move(value);
  item->decoded_ = true;
  return item;
}

std::string Item::Mimetype() const {
  if (!chunk_.has_value()) {
    return std::string(data::kJsonMimetype);
  }
  return chunk_->GetMimetype();
}

absl::StatusOr<Value> Item::Read(HostBridge* absl_nonnull bridge) const {
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

void Item::Prime(absl::StatusOr<Value> result) const {
  thread::MutexLock lock(&mu_);
  if (decoded_) {
    return;
  }
  result_ = std::move(result);
  decoded_ = true;
}

absl::Status Reader::NextMany(std::vector<ItemPtr>& out, size_t limit) {
  if (limit == 0) {
    return absl::OkStatus();
  }
  ABSL_ASSIGN_OR_RETURN(ItemPtr item, Next());
  if (item != nullptr) {
    out.push_back(std::move(item));
  }
  return absl::OkStatus();
}

absl::StatusOr<ItemPtr> ListReader::Next() {
  if (at_ >= items_.size()) {
    return ItemPtr{};
  }
  return items_[at_++];
}

absl::Status ListReader::NextMany(std::vector<ItemPtr>& out, size_t limit) {
  while (at_ < items_.size() && limit-- > 0) {
    out.push_back(items_[at_++]);
  }
  return absl::OkStatus();
}

absl::Status Sink::Many(std::vector<ItemPtr>& items) const {
  if (many_.has_value()) {
    return (*many_)(items);
  }
  for (ItemPtr& item : items) {
    ABSL_RETURN_IF_ERROR(one_(std::move(item)));
  }
  items.clear();
  return absl::OkStatus();
}

void SingleSlot::Wanted() {
  {
    thread::MutexLock lock(&monitor_->mu());
    wanted_ = true;
  }
  changed_.Wake();
}

bool SingleSlot::Skip() {
  thread::MutexLock lock(&monitor_->mu());
  if (handed_out_ < expected_) {
    ++handed_out_;
    Retire();
  }
  changed_.Wake();
  return !replay_ && retired_ == expected_ && !wanted_;
}

absl::Status SingleSlot::Pump() {
  {
    thread::MutexLock lock(&monitor_->mu());
    ABSL_RETURN_IF_ERROR(monitor_->Wait(
        changed_, [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(monitor_->mu()) {
          return wanted_ || (!replay_ && retired_ == expected_);
        }));
    if (!wanted_) {
      ended_ = true;
      changed_.Wake();
      return absl::OkStatus();
    }
  }
  Start();
  return absl::OkStatus();
}

void SingleSlot::Start() {
  {
    thread::MutexLock lock(&monitor_->mu());
    if (started_) {
      return;
    }
    started_ = true;
  }
  if (inline_ && stats_ != nullptr) {
    stats_->inline_producers.fetch_add(1, std::memory_order_relaxed);
  }
  const auto publish = [this](ItemPtr item) -> absl::Status {
    {
      thread::MutexLock lock(&monitor_->mu());
      if (published_) {
        return absl::InternalError("Single-item transport proof violated.");
      }
      published_ = true;
      if (replay_ || retired_ != expected_) {
        item_ = std::move(item);
      }
    }
    changed_.Wake();
    return absl::OkStatus();
  };
  const auto terminal = produce_(Sink(publish));
  {
    thread::MutexLock lock(&monitor_->mu());
    error_ = terminal;
    ended_ = true;
  }
  changed_.Wake();
}

Bus::Bus(Monitor& monitor, std::string label, Produce produce, int readers)
    : monitor_(&monitor),
      moved_(monitor),
      label_(std::move(label)),
      produce_(std::move(produce)) {
  slots_.reserve(static_cast<size_t>(std::max(readers, 0)));
  for (int index = 0; index < readers; ++index) {
    slots_.push_back(std::make_unique<Slot>());
  }
}

void Bus::TrackInputPump(const a11::Task& task) {
  {
    thread::MutexLock lock(&monitor_->mu());
    if (!finished_) {
      input_pump_ = task;
    }
  }
  StopAbandonedInput();
}

void Bus::StopAbandonedInput() {
  a11::Task task;
  {
    thread::MutexLock lock(&monitor_->mu());
    if (Abandoned()) {
      task = input_pump_;
    }
  }
  if (task.valid()) {
    task.Cancel().IgnoreError();
    monitor_->WakeAll();
  }
}

absl::Status Bus::CompletePump(absl::Status status) {
  {
    thread::MutexLock lock(&monitor_->mu());
    const bool abandoned_input = input_pump_.valid() && Abandoned();
    input_pump_ = {};
    finished_ = true;
    if (absl::IsCancelled(status) && abandoned_input) {
      status = absl::OkStatus();
    }
  }
  moved_.Wake();
  return status;
}

absl::Status Bus::WaitFinished() {
  thread::MutexLock lock(&monitor_->mu());
  return monitor_->Wait(moved_, [this] { return finished_; });
}

bool Bus::Skip() {
  bool unused;
  {
    thread::MutexLock lock(&monitor_->mu());
    if (handed_out_ < slots_.size()) {
      slots_[handed_out_++]->dropped = true;
    }
    unused = Abandoned() && !wanted_;
  }
  moved_.Wake();
  StopAbandonedInput();
  return unused;
}

void Bus::Wanted() {
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

bool Bus::Abandoned() ABSL_EXCLUSIVE_LOCKS_REQUIRED(monitor_->mu()) {
  return std::all_of(slots_.begin(), slots_.end(),
                     [](const auto& slot) { return slot->dropped; });
}

absl::StatusOr<ItemPtr> BusReader::Next() {
  bus_->Wanted();
  Monitor& monitor = bus_->monitor();
  thread::MutexLock lock(&monitor.mu());
  ABSL_RETURN_IF_ERROR(monitor.Wait(
      bus_->moved(), [this] { return !slot_->items.empty() || slot_->ended; }));
  if (!slot_->items.empty()) {
    ItemPtr item = std::move(slot_->items.front());
    slot_->items.pop_front();
    bus_->moved().Wake();
    return item;
  }
  ABSL_RETURN_IF_ERROR(slot_->error);
  return ItemPtr{};
}

absl::StatusOr<ItemPtr> BusReader::NextUntil(absl::Time deadline) {
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

absl::Status BusReader::NextMany(std::vector<ItemPtr>& out, size_t limit) {
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

void BusReader::Stop() {
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
  bus_->StopAbandonedInput();
}

absl::StatusOr<ReaderPtr> Lazy::Replay() {
  ABSL_ASSIGN_OR_RETURN(const ItemPtr item, Get());
  std::vector<ItemPtr> items;
  if (item != nullptr) {
    items.push_back(item);
  }
  return ReaderPtr(new ListReader(std::move(items)));
}

absl::StatusOr<ItemPtr> Lazy::Get() {
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

Buffer::Buffer(Monitor& monitor, ReaderPtr source)
    : state_(std::make_shared<State>(monitor)), source_(std::move(source)) {}

absl::Status Buffer::Fill() {
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

absl::StatusOr<std::optional<Value>> ValueCursor::Extract(Monitor& monitor) {
  {
    thread::MutexLock lock(&monitor.mu());
    ABSL_RETURN_IF_ERROR(monitor.Wait([this] { return !busy_; }));
    busy_ = true;
  }
  auto read = reader_->Next();
  absl::StatusOr<std::optional<Value>> result = std::optional<Value>();
  if (!read.ok()) {
    result = read.status();
  } else if (*read != nullptr) {
    auto value = (*read)->Read(bridge_);
    if (value.ok()) {
      result = std::optional<Value>(std::move(*value));
    } else {
      result = value.status();
    }
  }
  {
    thread::MutexLock lock(&monitor.mu());
    busy_ = false;
  }
  monitor.Wake();
  return result;
}

absl::StatusOr<Value> ValueCursor::Next(Monitor& monitor) {
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

absl::StatusOr<Value> ValueCursor::Read() {
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

absl::StatusOr<ItemPtr> ReplayReader::Next() {
  if (stopped_) {
    return ItemPtr{};
  }
  ABSL_ASSIGN_OR_RETURN(ItemPtr item, state_->At(at_));
  if (item != nullptr) {
    ++at_;
  }
  return item;
}

absl::StatusOr<ReaderPtr> SingleSlot::Take() {
  thread::MutexLock lock(&monitor_->mu());
  if (!replay_ && handed_out_++ >= expected_) {
    return absl::InternalError(
        "Single-item transport has no subscription left.");
  }
  return ReaderPtr(new Subscription(shared_from_this()));
}

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

absl::StatusOr<ReaderPtr> Buffer::Replay() {
  return ReaderPtr(new ReplayReader(state_));
}

}  // namespace a11::flow::internal
