// Copyright 2026 The A11 Authors.

#ifndef A11_NODES_NODE_MAP_H_
#define A11_NODES_NODE_MAP_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <absl/base/thread_annotations.h>
#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "thread/boost_primitives.h"

namespace a11::stores {
class ChunkStore;
}  // namespace a11::stores

namespace a11::nodes {

class AsyncNode;

using ChunkStoreFactory =
    std::function<absl::StatusOr<std::shared_ptr<stores::ChunkStore>>(
        std::string node_id)>;

class NodeMap : public std::enable_shared_from_this<NodeMap> {
 public:
  static absl::StatusOr<std::shared_ptr<NodeMap>> Create(
      ChunkStoreFactory factory = {});
  ~NodeMap() = default;

  absl::StatusOr<std::shared_ptr<AsyncNode>> Get(std::string node_id);
  absl::StatusOr<std::shared_ptr<AsyncNode>> GetIfExists(
      std::string_view node_id) const;
  absl::StatusOr<std::shared_ptr<AsyncNode>> Discard(
      std::string_view node_id,
      const std::shared_ptr<AsyncNode>& expected = nullptr);
  [[nodiscard]] bool Contains(std::string_view node_id) const;
  [[nodiscard]] size_t Size() const;

 private:
  explicit NodeMap(ChunkStoreFactory factory) : factory_(std::move(factory)) {}

  const ChunkStoreFactory factory_;
  mutable thread::Mutex mu_;
  absl::flat_hash_map<std::string, std::shared_ptr<AsyncNode>> nodes_
      ABSL_GUARDED_BY(mu_);
};

}  // namespace a11::nodes

#endif  // A11_NODES_NODE_MAP_H_
