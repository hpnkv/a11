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

#include "graph_snapshot.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/data/types.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/stores/chunk_store.h"
#include "a11/stores/local_chunk_store.h"
#include "runtime.h"

namespace a11::wasm {
namespace {

constexpr size_t kMaxSnapshotBytes = 64 * 1024 * 1024;
constexpr size_t kMaxSnapshotFragments = 1'000'000;

}  // namespace

absl::StatusOr<std::shared_ptr<nodes::NodeMap>> MakeTrackedNodeMap(
    const std::shared_ptr<StoreRegistry>& registry,
    std::string storage_id) {
  if (registry == nullptr) {
    return absl::InvalidArgumentError("store registry is required");
  }
  return nodes::NodeMap::Create(
      [registry](std::string node_id)
          -> absl::StatusOr<std::shared_ptr<stores::ChunkStore>> {
        thread::MutexLock lock(&registry->mu);
        const auto found = registry->stores.find(node_id);
        if (found != registry->stores.end()) {
          return std::static_pointer_cast<stores::ChunkStore>(found->second);
        }
        ABSL_ASSIGN_OR_RETURN(auto store,
                              stores::LocalChunkStore::Create(node_id));
        registry->stores.emplace(std::move(node_id), store);
        return std::static_pointer_cast<stores::ChunkStore>(store);
      }, data::StorageIdentity{.id = std::move(storage_id),
                               .credentials = {}, .services = {}});
}

absl::StatusOr<std::string> ExportGraphSnapshot(StoreRegistry& registry,
                                                const std::string& storage_id) {
  std::vector<std::pair<std::string, std::shared_ptr<stores::LocalChunkStore>>>
      stores;
  {
    thread::MutexLock lock(&registry.mu);
    stores.reserve(registry.stores.size());
    for (const auto& [id, store] : registry.stores) {
      stores.emplace_back(id, store);
    }
  }
  std::sort(stores.begin(), stores.end(),
            [](const auto& left, const auto& right) {
              return left.first < right.first;
            });
  data::WireMessage snapshot;
  snapshot.headers.emplace("wasm_snapshot", "1");
  snapshot.headers.emplace("wasm_storage", storage_id);
  size_t approximate_bytes = 0;
  for (const auto& [id, store] : stores) {
    ABSL_ASSIGN_OR_RETURN(const size_t count, store->Size().Await());
    if (count > kMaxSnapshotFragments - snapshot.node_fragments.size()) {
      return absl::ResourceExhaustedError("snapshot fragment limit exceeded");
    }
    for (size_t arrival = 0; arrival < count; ++arrival) {
      ABSL_ASSIGN_OR_RETURN(
          auto fragment,
          store->GetByArrivalOrder(arrival, absl::InfiniteFuture()).Await());
      if (auto* chunk = std::get_if<data::Chunk>(&fragment.data);
          chunk != nullptr) {
        ABSL_RETURN_IF_ERROR(chunk->Materialize());
      }
      approximate_bytes += fragment.ApproxBytes();
      if (approximate_bytes > kMaxSnapshotBytes) {
        return absl::ResourceExhaustedError("snapshot byte limit exceeded");
      }
      snapshot.node_fragments.push_back(std::move(fragment));
    }
    ABSL_ASSIGN_OR_RETURN(auto closed, store->GetWriteStatus().Await());
    if (closed.has_value()) {
      ABSL_ASSIGN_OR_RETURN(auto closure, data::MakeStatusChunk(*closed, true));
      data::NodeFragment marker;
      marker.id = id;
      marker.data = std::move(closure);
      marker.seq = static_cast<std::uint32_t>(count);
      approximate_bytes += marker.ApproxBytes();
      if (approximate_bytes > kMaxSnapshotBytes ||
          snapshot.node_fragments.size() >= kMaxSnapshotFragments) {
        return absl::ResourceExhaustedError("snapshot limit exceeded");
      }
      snapshot.node_fragments.push_back(std::move(marker));
    }
  }
  ABSL_ASSIGN_OR_RETURN(auto encoded, snapshot.ToMsgpack());
  if (encoded.size() > kMaxSnapshotBytes) {
    return absl::ResourceExhaustedError("snapshot byte limit exceeded");
  }
  return encoded;
}

absl::StatusOr<RestoredStores> RestoreGraphSnapshot(
    std::string_view bytes, const std::string& storage_id) {
  if (bytes.size() > kMaxSnapshotBytes) {
    return absl::ResourceExhaustedError("snapshot byte limit exceeded");
  }
  Runtime validation(storage_id);
  ABSL_RETURN_IF_ERROR(validation.ImportSnapshot(bytes));
  ABSL_ASSIGN_OR_RETURN(auto snapshot, data::WireMessage::FromMsgpack(bytes));
  auto registry = std::make_shared<StoreRegistry>();
  ABSL_ASSIGN_OR_RETURN(auto nodes, MakeTrackedNodeMap(registry, storage_id));
  for (auto& fragment : snapshot.node_fragments) {
    ABSL_ASSIGN_OR_RETURN(auto node, nodes->Get(fragment.id));
    auto store = node->GetChunkStore();
    if (auto* chunk = std::get_if<data::Chunk>(&fragment.data);
        chunk != nullptr && data::IsCloseStatusChunk(*chunk)) {
      ABSL_ASSIGN_OR_RETURN(auto status, data::StatusFromStatusChunk(*chunk));
      ABSL_RETURN_IF_ERROR(
          store->CloseWritesWithStatus(status).Await().status());
    } else {
      ABSL_RETURN_IF_ERROR(store->Put(std::move(fragment)).Await().status());
    }
  }
  return RestoredStores{.nodes = std::move(nodes),
                        .registry = std::move(registry)};
}

}  // namespace a11::wasm
