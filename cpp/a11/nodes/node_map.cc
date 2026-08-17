// Copyright 2026 The A11 Authors.

#include "a11/nodes/node_map.h"

#include "a11/nodes/internal/exception_guarded_factory.h"

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
#include "a11/stores/chunk_store.h"
#include "a11/stores/local_chunk_store.h"
#include "thread/boost_primitives.h"

namespace a11::nodes {

NodeMap::NodeMap(ChunkStoreFactory factory)
    // A factory may cross a language boundary, so it is guarded once here
    // rather than at every call. See
    // nodes/internal/exception_guarded_factory.h.
    : factory_(internal::GuardFactory(std::move(factory))) {}

absl::StatusOr<std::shared_ptr<NodeMap>> NodeMap::Create(
    ChunkStoreFactory factory) {
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
    explicit MakeSharedEnabler(ChunkStoreFactory factory)
        : NodeMap(std::move(factory)) {}
  };

  return std::make_shared<MakeSharedEnabler>(std::move(factory));
}

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
    for (const auto& [id, node] : nodes_) ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

}  // namespace a11::nodes
