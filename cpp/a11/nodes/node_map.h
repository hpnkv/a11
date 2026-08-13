// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A registry of AsyncNodes keyed by node id.
 *
 * A `NodeMap` owns a collection of `AsyncNode` instances addressed by
 * string id, lazily creating nodes on demand through a `ChunkStoreFactory`
 * that builds each node's backing store. It is how a session (or an agent)
 * routes streamed data to the right node: fragments dispatched by id land
 * in the node the map holds under that id.
 */

#ifndef A11_NODES_NODE_MAP_H_
#define A11_NODES_NODE_MAP_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

/// Callable invoked to construct the backing chunk store for a new node
/// from its id.
using ChunkStoreFactory =
    std::function<absl::StatusOr<std::shared_ptr<stores::ChunkStore>>(
        std::string node_id)>;

/**
 * @brief A thread-safe registry of AsyncNodes keyed by id.
 *
 * Nodes are created lazily on first access via the map's
 * `ChunkStoreFactory`. Instances are heap-allocated and shared via
 * `Create`.
 */
class NodeMap : public std::enable_shared_from_this<NodeMap> {
 public:
  /**
   * @brief Create a node map.
   * @param factory Factory invoked to build the backing store for each new
   *   node; a default store is used when omitted.
   * @return The new map, or an error status on failure.
   */
  static absl::StatusOr<std::shared_ptr<NodeMap>> Create(
      ChunkStoreFactory factory = {});
  ~NodeMap() = default;

  /**
   * @brief Return the node for an id, creating it if it does not exist.
   * @param node_id The node identifier.
   * @return The node, or an error status on failure.
   */
  absl::StatusOr<std::shared_ptr<AsyncNode>> Get(std::string node_id);

  /**
   * @brief Return the node for an id without creating it.
   * @param node_id The node identifier.
   * @return The node, or nullptr if it does not exist (or an error status).
   */
  absl::StatusOr<std::shared_ptr<AsyncNode>> GetIfExists(
      std::string_view node_id) const;

  /**
   * @brief Remove the node for an id.
   * @param node_id The node identifier.
   * @param expected If non-null, only remove when the stored node matches
   *   this one (compare-and-remove).
   * @return The removed node, or an error status on failure.
   */
  absl::StatusOr<std::shared_ptr<AsyncNode>> Discard(
      std::string_view node_id,
      const std::shared_ptr<AsyncNode>& expected = nullptr);

  /**
   * @brief Report whether a node with the given id exists.
   * @param node_id The node identifier.
   * @return True if the map holds a node under that id.
   */
  [[nodiscard]] bool Contains(std::string_view node_id) const;

  /**
   * @brief Return the number of nodes currently in the map.
   * @return The node count.
   */
  [[nodiscard]] size_t Size() const;

  /**
   * @brief Return the id of every node the map holds, sorted.
   *
   * A snapshot: nodes are created on demand, so the answer is what was there
   * when it was asked for. Sorted rather than in hash order so a caller
   * comparing two snapshots -- a test, a diagnostic dump -- sees a stable list.
   * @return The ids.
   */
  [[nodiscard]] std::vector<std::string> Ids() const;

 private:
  explicit NodeMap(ChunkStoreFactory factory) : factory_(std::move(factory)) {}

  const ChunkStoreFactory factory_;
  mutable thread::Mutex mu_;
  absl::flat_hash_map<std::string, std::shared_ptr<AsyncNode>> nodes_
      ABSL_GUARDED_BY(mu_);
};

}  // namespace a11::nodes

#endif  // A11_NODES_NODE_MAP_H_
