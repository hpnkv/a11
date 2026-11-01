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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_builder.h>
#include <absl/status/statusor.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "a11/concurrency/executor.h"
#include "a11/data/types.h"
#include "a11/stores/chunk_store_reader.h"
#include "thread/boost_primitives.h"

namespace a11::stores {

struct ChunkStoreReader::LogicalState {
  struct Frame {
    std::shared_ptr<ChunkStore> store;
    std::shared_ptr<ChunkStoreReader> reader;
    std::shared_ptr<ReferenceResolver> resolver;
    std::optional<std::uint64_t> remaining;
    std::string target_id;
  };

  thread::Mutex mu;
  thread::Mutex readers_mu;
  std::vector<Frame> frames ABSL_GUARDED_BY(mu);
  std::vector<std::shared_ptr<ChunkStoreReader>> active
      ABSL_GUARDED_BY(readers_mu);
  std::atomic<bool> cancelled = false;
  ChunkStoreReaderOptions options;
  absl::Status status ABSL_GUARDED_BY(mu) = absl::OkStatus();
  bool ended ABSL_GUARDED_BY(mu) = false;
  bool root_final ABSL_GUARDED_BY(mu) = false;
  std::string mimetype ABSL_GUARDED_BY(mu);
  std::uint64_t emitted ABSL_GUARDED_BY(mu) = 0;

  LogicalState(std::shared_ptr<ChunkStore> root,
               std::shared_ptr<ChunkStoreReader> reader,
               std::shared_ptr<ReferenceResolver> resolver,
               ChunkStoreReaderOptions selected)
      : frames{{std::move(root),
                std::move(reader),
                std::move(resolver),
                std::nullopt,
                {}}},
        options(selected) {
    active.push_back(frames.front().reader);
  }

  ~LogicalState() { Cancel(); }

  void Cancel() {
    cancelled.store(true, std::memory_order_release);
    std::vector<std::shared_ptr<ChunkStoreReader>> readers;
    {
      thread::MutexLock lock(&readers_mu);
      readers = active;
    }
    for (const auto& reader : readers) {
      reader->Cancel();
    }
    thread::MutexLock lock(&mu);
    if (status.ok() && !ended) {
      status = absl::CancelledError("Logical reader cancelled");
    }
  }

  absl::Status Status() {
    thread::MutexLock lock(&mu);
    return status;
  }

  bool SawFinal() {
    thread::MutexLock lock(&mu);
    return root_final;
  }

  absl::StatusOr<std::optional<data::NodeFragment>> Next(
      absl::Duration timeout) {
    thread::MutexLock lock(&mu);
    if (!status.ok()) {
      return status;
    }
    if (ended) {
      return std::nullopt;
    }
    if (options.max_chunks_to_read == 0) {
      ended = true;
      return std::nullopt;
    }
    const absl::Time deadline = timeout == absl::InfiniteDuration()
                                    ? absl::InfiniteFuture()
                                    : absl::Now() + timeout;
    size_t steps = 0;
    while (!frames.empty()) {
      if (cancelled.load(std::memory_order_acquire)) {
        return absl::CancelledError("Logical reader cancelled");
      }
      if (++steps % 128 == 0) {
        if (deadline != absl::InfiniteFuture() && absl::Now() >= deadline) {
          return absl::DeadlineExceededError("Reference read timed out");
        }
        thread::SleepFor(absl::Microseconds(1));
      }
      Frame& frame = frames.back();
      if (frame.remaining == 0) {
        frame.reader->Cancel();
        {
          thread::MutexLock readers_lock(&readers_mu);
          active.pop_back();
        }
        frames.pop_back();
        continue;
      }
      const absl::Duration remaining_time =
          deadline == absl::InfiniteFuture()
              ? absl::InfiniteDuration()
              : std::max(absl::ZeroDuration(), deadline - absl::Now());
      auto next = frame.reader->Next(remaining_time).Await();
      if (!next.ok()) {
        if (absl::IsDeadlineExceeded(next.status())) {
          return next.status();
        }
        status = frame.target_id.empty()
                     ? next.status()
                     : absl::StatusBuilder(next.status()).SetPrepend()
                           << "Reference target '" << frame.target_id << "': ";
        return status;
      }
      if (!next->has_value()) {
        if (frame.remaining.has_value()) {
          status = absl::DataLossError(
              "Referenced node ended before its fragment window");
          return status;
        }
        frame.reader->Cancel();
        {
          thread::MutexLock readers_lock(&readers_mu);
          active.pop_back();
        }
        frames.pop_back();
        continue;
      }
      if (frame.remaining.has_value()) {
        --*frame.remaining;
      }
      auto fragment = std::move(**next);
      if (frames.size() == 1 && !fragment.continued) {
        root_final = true;
      }
      if (auto* reference = std::get_if<data::NodeRef>(&fragment.data)) {
        if (!options.follow_node_refs) {
          status = absl::UnimplementedError("Reference following is disabled");
          return status;
        }
        if (reference->length == 0) {
          continue;
        }
        if (frames.size() > options.max_ref_depth) {
          status = absl::ResourceExhaustedError("Reference depth exceeded");
          return status;
        }
        if (frame.resolver == nullptr) {
          status = absl::NotFoundError("No resolver for referenced node");
          return status;
        }
        auto target = frame.resolver->Resolve(*reference);
        if (!target.ok()) {
          status = target.status();
          return status;
        }
        for (const auto& ancestor : frames) {
          if (ancestor.store == target->store) {
            status = absl::FailedPreconditionError("Reference cycle detected");
            return status;
          }
        }
        auto child_options = options;
        child_options.offset = reference->offset;
        child_options.max_chunks_to_read.reset();
        child_options.sticky_mimetype = false;
        child_options.num_chunks_to_buffer = 0;
        auto child = ChunkStoreReader::Create(target->store, child_options);
        if (!child.ok()) {
          status = child.status();
          return status;
        }
        frames.push_back(Frame{target->store, *child, target->resolver,
                               reference->length, reference->id});
        {
          thread::MutexLock readers_lock(&readers_mu);
          active.push_back(*child);
        }
        continue;
      }
      data::Chunk& chunk = std::get<data::Chunk>(fragment.data);
      if (chunk.IsNull()) {
        if (!fragment.continued && frames.size() == 1) {
          ended = true;
          return std::nullopt;
        }
        continue;
      }
      if (options.sticky_mimetype) {
        if (chunk.metadata.has_value() && !chunk.metadata->mimetype.empty()) {
          mimetype = chunk.metadata->mimetype;
        } else if (!mimetype.empty()) {
          if (!chunk.metadata.has_value()) {
            chunk.metadata = data::ChunkMetadata{};
          }
          chunk.metadata->mimetype = mimetype;
        }
      }
      ++emitted;
      if (options.max_chunks_to_read.has_value() &&
          emitted >= *options.max_chunks_to_read) {
        ended = true;
      }
      return fragment;
    }
    ended = true;
    return std::nullopt;
  }
};

absl::StatusOr<std::shared_ptr<ChunkStoreReader::LogicalState>>
ChunkStoreReader::GetLogical() {
  thread::MutexLock lock(&logical_mu_);
  if (logical_ == nullptr) {
    auto raw_options = options();
    raw_options.max_chunks_to_read.reset();
    raw_options.sticky_mimetype = false;
    auto raw = ChunkStoreReader::Create(store(), raw_options);
    if (!raw.ok()) {
      return raw.status();
    }
    logical_ =
        std::make_shared<LogicalState>(store(), *raw, resolver_, options());
  }
  return logical_;
}

void ChunkStoreReader::CancelLogical() {
  std::shared_ptr<LogicalState> state;
  {
    thread::MutexLock lock(&logical_mu_);
    state = logical_;
  }
  if (state != nullptr) {
    state->Cancel();
  }
}

absl::Status ChunkStoreReader::GetLogicalStatus() const {
  std::shared_ptr<LogicalState> state;
  {
    thread::MutexLock lock(&logical_mu_);
    state = logical_;
  }
  return state == nullptr ? absl::OkStatus() : state->Status();
}

bool ChunkStoreReader::LogicalSawFinal() const {
  std::shared_ptr<LogicalState> state;
  {
    thread::MutexLock lock(&logical_mu_);
    state = logical_;
  }
  return state != nullptr && state->SawFinal();
}

a11::Future<std::optional<data::NodeFragment>> ChunkStoreReader::NextLogical(
    absl::Duration timeout) {
  if (timeout < absl::ZeroDuration() && timeout != absl::InfiniteDuration()) {
    return a11::FailedFuture<std::optional<data::NodeFragment>>(
        absl::InvalidArgumentError("timeout must be non-negative or infinite"));
  }
  auto state = GetLogical();
  if (!state.ok()) {
    return a11::FailedFuture<std::optional<data::NodeFragment>>(state.status());
  }
  return a11::Submit<std::optional<data::NodeFragment>>(
      [state = *std::move(state), timeout]() { return state->Next(timeout); });
}

a11::Future<std::vector<std::optional<data::NodeFragment>>>
ChunkStoreReader::NextLogicalMany(size_t limit, absl::Duration timeout) {
  using Batch = std::vector<std::optional<data::NodeFragment>>;
  if (limit == 0) {
    return a11::FailedFuture<Batch>(
        absl::InvalidArgumentError("Logical read limit must be positive"));
  }
  if (timeout < absl::ZeroDuration() && timeout != absl::InfiniteDuration()) {
    return a11::FailedFuture<Batch>(
        absl::InvalidArgumentError("timeout must be non-negative or infinite"));
  }
  auto state = GetLogical();
  if (!state.ok()) {
    return a11::FailedFuture<Batch>(state.status());
  }
  return a11::Submit<Batch>(
      [state = *std::move(state), limit, timeout]() -> absl::StatusOr<Batch> {
        Batch result;
        result.reserve(limit);
        for (size_t index = 0; index < limit; ++index) {
          auto next = state->Next(index == 0 ? timeout : absl::ZeroDuration());
          if (!next.ok()) {
            if (!result.empty() && absl::IsDeadlineExceeded(next.status())) {
              break;
            }
            return next.status();
          }
          result.push_back(std::move(*next));
          if (!result.back().has_value()) {
            break;
          }
        }
        return result;
      });
}

}  // namespace a11::stores
