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

absl::Status CloseNode(const NodePtr& node) {
  return node->Finalize({.wait = true}).Await().status();
}

absl::StatusOr<NodePtr> Destination::Node() {
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

void Destination::Watch(const void* absl_nonnull owner, StepId step,
                        const a11::Task& task, bool exclusive) {
  bool cancel;
  {
    thread::MutexLock lock(&monitor_->mu());
    cancel =
        (!failure_.ok() && !feeds_stopped_) || (feeds_stopped_ && exclusive);
    if (!cancel) {
      feeding_.push_back(Feeding{owner, step, task, exclusive});
    }
  }
  if (cancel) {
    task.Cancel().IgnoreError();
    monitor_->WakeAll();
  }
}

void Destination::StopFeeding(const absl::Status& status) {
  absl::InlinedVector<a11::Task, 1> tasks;
  {
    thread::MutexLock lock(&monitor_->mu());
    if (failure_.ok()) {
      feeds_stopped_ = status.ok() || tolerant_;
      failure_ = status;
    }
    for (const auto& feed : feeding_) {
      if (feed.exclusive) {
        tasks.push_back(feed.task);
      }
    }
  }
  for (const auto& task : tasks) {
    task.Cancel().IgnoreError();
  }
  monitor_->WakeAll();
}

void Destination::Unwatch(const void* absl_nonnull owner, StepId step) {
  thread::MutexLock lock(&monitor_->mu());
  const auto found = std::find_if(
      feeding_.begin(), feeding_.end(), [&](const Feeding& feeding) {
        return feeding.owner == owner && feeding.step == step;
      });
  if (found != feeding_.end()) {
    if (found != feeding_.end() - 1) {
      *found = std::move(feeding_.back());
    }
    feeding_.pop_back();
  }
}

absl::Status Destination::CheckWritable() {
  thread::MutexLock lock(&monitor_->mu());
  if (closed_) {
    return absl::FailedPreconditionError("Cannot attach to a closed sink.");
  }
  if (feeds_stopped_) {
    return absl::OkStatus();
  }
  ABSL_RETURN_IF_ERROR(failure_);
  return absl::OkStatus();
}

absl::Status Destination::WriteMany(std::vector<ItemPtr>& items,
                                    HostBridge* absl_nonnull bridge) {
  const absl::Status written = WriteRange(items, bridge);
  items.clear();
  return written;
}

absl::Status Destination::WriteRange(absl::Span<const ItemPtr> items,
                                     HostBridge* absl_nonnull bridge) {
  if (items.empty() || AcceptStoppedFeed()) {
    return absl::OkStatus();
  }
  ABSL_ASSIGN_OR_RETURN(const NodePtr node, Node());
  absl::StatusOr<std::shared_ptr<stores::ChunkStoreWriter>> writer =
      node->writer();
  ABSL_RETURN_IF_ERROR(writer.status());
  // Serialize writers so shared destinations append complete values. Encode
  // computed batches in one host crossing where supported.
  std::vector<absl::StatusOr<data::Chunk>> encoded;
  const size_t computed_count = static_cast<size_t>(std::count_if(
      items.begin(), items.end(),
      [](const auto& item) { return !item->chunk().has_value(); }));
  if (computed_count > 1) {
    std::vector<const Value*> values;
    std::vector<absl::StatusOr<Value>> read;
    read.reserve(computed_count);
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
  if (AcceptStoppedFeed()) {
    Leave();
    return absl::OkStatus();
  }
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

absl::Status Destination::Abort(absl::Status reason) {
  std::vector<a11::Task> feeding;
  {
    thread::MutexLock lock(&monitor_->mu());
    if (failure_.ok() || feeds_stopped_) {
      failure_ = std::move(reason);
    }
    feeds_stopped_ = false;
    reason = failure_;
    for (const auto& entry : feeding_) {
      feeding.push_back(entry.task);
    }
  }
  for (const auto& task : feeding) {
    task.Cancel().IgnoreError();
  }
  monitor_->WakeAll();
  return Finish(/*forced=*/true, std::move(reason));
}

absl::Status Destination::Finished() {
  thread::MutexLock lock(&monitor_->mu());
  return monitor_->Wait(turn_, [this] { return finished_; });
}

absl::Status Destination::Enter(bool cleanup) {
  thread::MutexLock lock(&monitor_->mu());
  if (cleanup) {
    monitor_->WaitComplete(turn_, [this] { return !writing_; });
  } else {
    ABSL_RETURN_IF_ERROR(monitor_->Wait(turn_, [this] { return !writing_; }));
    if (!feeds_stopped_) {
      ABSL_RETURN_IF_ERROR(failure_);
    }
    if (closed_) {
      return absl::FailedPreconditionError("Cannot attach to a closed sink.");
    }
  }
  writing_ = true;
  return absl::OkStatus();
}

void Destination::Leave() {
  {
    thread::MutexLock lock(&monitor_->mu());
    writing_ = false;
  }
  turn_.Wake();
}

absl::Status Destination::Finish(bool forced, absl::Status reason) {
  ABSL_RETURN_IF_ERROR(Enter(/*cleanup=*/true));
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

}  // namespace a11::flow::internal
