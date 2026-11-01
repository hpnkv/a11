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

#include "a11/actions/reference_mirror.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/time/time.h>

#include "a11/concurrency/executor.h"
#include "a11/net/wire_stream.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/stores/chunk_store_reader.h"
#include "a11/uuid.h"
#include "thread/boost_primitives.h"

namespace a11::actions {

struct ReferenceMirrorRegistry::State
    : public std::enable_shared_from_this<ReferenceMirrorRegistry::State> {
  struct Key {
    const nodes::NodeMap* map;
    std::string storage;
    std::string id;

    bool operator==(const Key&) const = default;

    template <typename H>
    friend H AbslHashValue(H hash, const Key& key) {
      return H::combine(std::move(hash), key.map, key.storage, key.id);
    }
  };

  enum class Phase { kStarting, kReplaying, kClosed, kFailed };

  struct Mirror {
    std::string id;
    std::weak_ptr<nodes::NodeMap> source_map;
    Phase phase = Phase::kStarting;
    absl::Status status;
  };

  explicit State(std::shared_ptr<net::WireStream> wire,
                 std::vector<std::string> accessible)
      : stream(std::move(wire)), direct(accessible.begin(), accessible.end()) {}

  absl::StatusOr<data::NodeRef> Rewrite(
      const data::NodeRef& reference,
      const std::shared_ptr<nodes::NodeMap>& source_map) {
    ABSL_RETURN_IF_ERROR(reference.Validate());
    if (reference.length == 0) {
      data::NodeRef empty = reference;
      empty.storage.reset();
      return empty;
    }
    if (reference.storage.has_value() &&
        direct.find(reference.storage->id) != direct.end()) {
      return reference;
    }
    if (source_map == nullptr) {
      return absl::FailedPreconditionError(
          "Reference source map is unavailable");
    }
    Key key{source_map.get(),
            reference.storage.has_value() ? reference.storage->id : "",
            reference.id};
    ABSL_ASSIGN_OR_RETURN(auto source, source_map->ResolveForRead(reference));
    auto mirror = std::make_shared<Mirror>();
    mirror->id = std::string(kReferenceMirrorPrefix) + a11::NewUuid();
    mirror->source_map = source_map;
    bool start = false;
    {
      thread::MutexLock lock(&mu);
      const auto [found, inserted] = mirrors.emplace(key, mirror);
      if (!inserted && !found->second->source_map.expired()) {
        mirror = found->second;
      } else {
        if (!inserted) {
          found->second = mirror;
        }
        if (active >= kMaxActiveMirrors) {
          mirrors.erase(key);
          return absl::ResourceExhaustedError("Active mirror limit reached");
        }
        ++active;
        start = true;
      }
    }
    if (start) {
      auto self = shared_from_this();
      auto pending = a11::Submit<int>([self, mirror, source = std::move(source),
                                       source_map]() -> absl::StatusOr<int> {
        ABSL_RETURN_IF_ERROR(self->Pump(mirror, source, source_map));
        return 0;
      });
      a11::Then(pending,
                [self, mirror](const absl::StatusOr<int>& done)
                    -> absl::StatusOr<a11::Unit> {
                  const absl::Status result =
                      done.ok() ? absl::OkStatus() : done.status();
                  self->Finish(mirror, result);
                  if (!result.ok()) {
                    self->SendFailure(mirror->id, result).IgnoreError();
                  }
                  return a11::Unit{};
                });
    }
    data::NodeRef rewritten = reference;
    rewritten.id = mirror->id;
    rewritten.storage.reset();
    return rewritten;
  }

  absl::Status Pump(const std::shared_ptr<Mirror>& mirror,
                    const std::shared_ptr<nodes::AsyncNode>& source,
                    const std::shared_ptr<nodes::NodeMap>& source_map) {
    stores::ChunkStoreReaderOptions options;
    options.follow_node_refs = false;
    options.num_chunks_to_buffer = 8;
    ABSL_ASSIGN_OR_RETURN(auto cursor, source->OpenReader(options));
    const auto owner = source->Owner();
    const auto nested_map = owner == nullptr ? source_map : owner;
    {
      thread::MutexLock lock(&mu);
      mirror->phase = Phase::kReplaying;
    }
    std::uint32_t next_seq = 0;
    bool continued = true;
    while (true) {
      auto next = cursor->Next(absl::Seconds(1)).Await();
      if (!next.ok() && absl::IsDeadlineExceeded(next.status())) {
        const absl::Status wire = stream->GetStatus();
        if (!wire.ok()) {
          cursor->Cancel();
          return wire;
        }
        continue;
      }
      ABSL_RETURN_IF_ERROR(next.status());
      if (!next->has_value()) {
        break;
      }
      data::NodeFragment fragment = std::move(**next);
      fragment.id = mirror->id;
      if (auto* nested = std::get_if<data::NodeRef>(&fragment.data)) {
        ABSL_ASSIGN_OR_RETURN(*nested, Rewrite(*nested, nested_map));
      }
      if (!fragment.seq.has_value()) {
        return absl::DataLossError("Mirrored fragment has no sequence");
      }
      if (*fragment.seq == std::numeric_limits<std::uint32_t>::max()) {
        return absl::ResourceExhaustedError("Mirror sequence limit reached");
      }
      next_seq = *fragment.seq + 1;
      continued = fragment.continued;
      data::WireMessage message;
      message.node_fragments.push_back(std::move(fragment));
      ABSL_RETURN_IF_ERROR(stream->Send(std::move(message)));
    }
    if (continued) {
      data::WireMessage message;
      message.node_fragments.push_back(data::NodeFragment{
          .id = mirror->id,
          .data = data::Chunk{.metadata =
                                  data::ChunkMetadata{
                                      .mimetype = "application/octet-stream"}},
          .seq = next_seq,
          .continued = false,
      });
      ABSL_RETURN_IF_ERROR(stream->Send(std::move(message)));
    }
    return absl::OkStatus();
  }

  void Finish(const std::shared_ptr<Mirror>& mirror, absl::Status result) {
    thread::MutexLock lock(&mu);
    mirror->status = std::move(result);
    mirror->phase = mirror->status.ok() ? Phase::kClosed : Phase::kFailed;
    --active;
  }

  absl::Status SendFailure(const std::string& id, const absl::Status& status) {
    ABSL_ASSIGN_OR_RETURN(auto chunk, data::MakeStatusChunk(status, true));
    data::WireMessage message;
    message.node_fragments.push_back(data::NodeFragment{
        .id = id, .data = std::move(chunk), .seq = 0, .continued = false});
    return stream->Send(std::move(message));
  }

  static constexpr size_t kMaxActiveMirrors = 128;
  std::shared_ptr<net::WireStream> stream;
  absl::flat_hash_set<std::string> direct;
  thread::Mutex mu;
  absl::flat_hash_map<Key, std::shared_ptr<Mirror>> mirrors ABSL_GUARDED_BY(mu);
  size_t active ABSL_GUARDED_BY(mu) = 0;
};

ReferenceMirrorRegistry::ReferenceMirrorRegistry(
    std::shared_ptr<net::WireStream> stream,
    std::vector<std::string> direct_storage_ids)
    : state_(std::make_shared<State>(std::move(stream),
                                     std::move(direct_storage_ids))) {}

absl::StatusOr<data::NodeRef> ReferenceMirrorRegistry::Rewrite(
    const data::NodeRef& reference,
    const std::shared_ptr<nodes::NodeMap>& source_map) {
  return state_->Rewrite(reference, source_map);
}

}  // namespace a11::actions
