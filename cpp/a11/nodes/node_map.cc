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

#include "a11/nodes/node_map.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>

#include "a11/data/types.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/internal/exception_guarded_factory.h"
#include "a11/stores/chunk_store.h"
#include "a11/stores/local_chunk_store.h"
#include "thread/boost_primitives.h"

namespace a11::nodes {

NodeMap::NodeMap(ChunkStoreFactory factory,
                 std::optional<data::StorageIdentity> storage)
    // A factory may cross a language boundary, so it is guarded once here
    // rather than at every call. See
    // nodes/internal/exception_guarded_factory.h.
    : factory_(internal::GuardFactory(std::move(factory))),
      storage_(std::move(storage)) {}

absl::StatusOr<std::shared_ptr<NodeMap>> NodeMap::Create(
    ChunkStoreFactory factory, std::optional<data::StorageIdentity> storage) {
  if (storage.has_value()) {
    ABSL_RETURN_IF_ERROR(storage->Validate());
  }
  if (!factory) {
    factory = [](std::string node_id)
        -> absl::StatusOr<std::shared_ptr<stores::ChunkStore>> {
      ABSL_ASSIGN_OR_RETURN(
          std::shared_ptr<stores::LocalChunkStore> store,
          stores::LocalChunkStore::Create(std::move(node_id)));
      return std::static_pointer_cast<stores::ChunkStore>(store);
    };
  }

  struct MakeSharedEnabler final : NodeMap {
    explicit MakeSharedEnabler(ChunkStoreFactory factory,
                               std::optional<data::StorageIdentity> storage)
        : NodeMap(std::move(factory), std::move(storage)) {}
  };

  return std::make_shared<MakeSharedEnabler>(std::move(factory),
                                             std::move(storage));
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
absl::StatusOr<std::shared_ptr<AsyncNode>> NodeMap::Get(std::string node_id) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(node_id));
  {
    thread::MutexLock lock(&mu_);
    const auto found = nodes_.find(node_id);
    if (found != nodes_.end()) {
      return found->second;
    }
  }

  // A factory may cross a language boundary. Never invoke it while holding
  // the map mutex: Python callbacks in particular may re-enter this NodeMap.
  // The factory was guarded when this NodeMap adopted it; see the constructor.
  const absl::StatusOr<std::shared_ptr<stores::ChunkStore>> store =
      factory_(node_id);
  if (!store.ok()) {
    return store.status();
  }
  if (*store == nullptr) {
    return absl::InternalError("chunk-store factory returned null");
  }
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> node,
                        AsyncNode::Create(*store));
  {
    thread::MutexLock lock(&mu_);
    const auto iterator = nodes_.emplace(std::move(node_id), node).first;
    // Two callers may race through the factory. The first published node is
    // authoritative; the unused store remains privately owned and is safely
    // destroyed after this call.
    return iterator->second;
  }
}

absl::StatusOr<std::shared_ptr<AsyncNode>> NodeMap::GetIfExists(
    std::string_view node_id) const {
  ABSL_RETURN_IF_ERROR(data::ValidateName(node_id));
  thread::MutexLock lock(&mu_);
  const auto found = nodes_.find(std::string(node_id));
  if (found == nodes_.end()) {
    return std::shared_ptr<AsyncNode>();
  }
  return found->second;
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
absl::Status NodeMap::RegisterStorage(std::string storage_id,
                                      std::shared_ptr<NodeMap> storage) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(storage_id));
  if (storage == nullptr || storage.get() == this) {
    return absl::InvalidArgumentError(
        "Storage namespace requires another node map");
  }
  thread::MutexLock lock(&mu_);
  const auto [found, inserted] =
      storages_.emplace(std::move(storage_id), storage);
  if (!inserted && found->second != storage) {
    return absl::AlreadyExistsError("Storage identity is already configured");
  }
  return absl::OkStatus();
}

absl::Status NodeMap::SetStorageResolver(
    StorageResolver resolver, std::vector<std::string> accessible_ids) {
  if (!resolver) {
    return absl::InvalidArgumentError("Storage resolver must be callable");
  }
  for (const auto& id : accessible_ids) {
    ABSL_RETURN_IF_ERROR(data::ValidateName(id));
  }
  auto guarded = internal::GuardStorageResolver(std::move(resolver));
  thread::MutexLock lock(&mu_);
  storage_resolver_ = std::move(guarded);
  accessible_ids_ = std::move(accessible_ids);
  return absl::OkStatus();
}

std::vector<std::string> NodeMap::StorageIds() const {
  thread::MutexLock lock(&mu_);
  std::vector<std::string> result = accessible_ids_;
  if (storage_.has_value()) {
    result.push_back(storage_->id);
  }
  for (const auto& [id, unused] : storages_) {
    result.push_back(id);
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

absl::StatusOr<std::shared_ptr<AsyncNode>> NodeMap::Resolve(
    const data::NodeRef& reference) {
  ABSL_RETURN_IF_ERROR(reference.Validate());
  if (reference.offset != 0 || reference.length.has_value()) {
    return absl::UnimplementedError(
        "Resolving a NodeRef window requires a fragment-window reader");
  }
  if (!reference.storage.has_value() ||
      (storage_.has_value() && reference.storage->id == storage_->id)) {
    return Get(reference.id);
  }
  std::shared_ptr<NodeMap> storage;
  std::shared_ptr<AsyncNode> mirrored;
  StorageResolver resolver;
  {
    thread::MutexLock lock(&mu_);
    const auto found = storages_.find(reference.storage->id);
    if (found != storages_.end()) {
      storage = found->second;
    }
    resolver = storage_resolver_;
    const auto group = imported_.find(reference.storage->id);
    if (group != imported_.end()) {
      const auto node = group->second.find(reference.id);
      if (node != group->second.end()) {
        mirrored = node->second;
      }
    }
  }
  if (storage != nullptr) {
    return storage->Get(reference.id);
  }
  if (resolver) {
    auto resolved = resolver(*reference.storage);
    if (resolved.ok()) {
      if (*resolved == nullptr) {
        return absl::InternalError("Storage resolver returned null");
      }
      return (*resolved)->Get(reference.id);
    }
    if (!absl::IsNotFound(resolved.status())) {
      return resolved.status();
    }
  }
  if (mirrored != nullptr) {
    return mirrored;
  }
  return absl::NotFoundError("Storage identity is not configured on this host");
}

absl::StatusOr<std::shared_ptr<AsyncNode>> NodeMap::ImportReference(
    const data::NodeRef& reference, const std::shared_ptr<NodeMap>& source) {
  if (source == nullptr) {
    return absl::InvalidArgumentError("Source node map must not be null");
  }
  ABSL_RETURN_IF_ERROR(reference.Validate());
  if (reference.storage.has_value()) {
    auto direct = Resolve(reference);
    if (direct.ok()) {
      return direct;
    }
    if (!absl::IsNotFound(direct.status())) {
      return direct.status();
    }
    auto original = source->Resolve(reference);
    if (!original.ok() && !absl::IsNotFound(original.status())) {
      return original.status();
    }
    if (!original.ok()) {
      original = source->Get(reference.id);
    }
    ABSL_RETURN_IF_ERROR(original.status());
    thread::MutexLock lock(&mu_);
    const auto [found, inserted] =
        imported_[reference.storage->id].emplace(reference.id, *original);
    if (!inserted && found->second != *original) {
      return absl::AlreadyExistsError(
          "A different node has this storage-qualified id");
    }
    return found->second;
  }
  ABSL_ASSIGN_OR_RETURN(auto node, source->Resolve(reference));
  ABSL_RETURN_IF_ERROR(Insert(node));
  return node;
}

absl::Status NodeMap::Insert(const std::shared_ptr<AsyncNode>& node) {
  if (node == nullptr) {
    return absl::InvalidArgumentError("node must not be null");
  }
  ABSL_ASSIGN_OR_RETURN(std::string id, node->GetId());
  thread::MutexLock lock(&mu_);
  const auto [found, inserted] = nodes_.emplace(std::move(id), node);
  if (!inserted && found->second != node) {
    return absl::AlreadyExistsError("A different node has this id");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::shared_ptr<AsyncNode>> NodeMap::Discard(
    std::string_view node_id, const std::shared_ptr<AsyncNode>& expected) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(node_id));
  thread::MutexLock lock(&mu_);
  const auto found = nodes_.find(std::string(node_id));
  if (found == nodes_.end() ||
      (expected != nullptr && found->second != expected)) {
    return std::shared_ptr<AsyncNode>();
  }
  std::shared_ptr<AsyncNode> result = std::move(found->second);
  nodes_.erase(found);
  return result;
}

bool NodeMap::Contains(std::string_view node_id) const {
  thread::MutexLock lock(&mu_);
  return nodes_.find(std::string(node_id)) != nodes_.end();
}

size_t NodeMap::Size() const {
  thread::MutexLock lock(&mu_);
  return nodes_.size();
}

std::vector<std::string> NodeMap::Ids() const {
  std::vector<std::string> ids;
  {
    thread::MutexLock lock(&mu_);
    ids.reserve(nodes_.size());
    for (const auto& [id, node] : nodes_) {
      ids.push_back(id);
    }
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

}  // namespace a11::nodes
