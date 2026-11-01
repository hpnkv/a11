/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/base/thread_annotations.h>
#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/data/types.h"
#include "thread/boost_primitives.h"

namespace a11::stores {
class ChunkStore;
}  // namespace a11::stores

namespace a11::nodes {

class AsyncNode;
class NodeMap;
using StorageResolver = std::function<absl::StatusOr<std::shared_ptr<NodeMap>>(
    data::StorageIdentity)>;

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
      ChunkStoreFactory factory = {},
      std::optional<data::StorageIdentity> storage = {});
  ~NodeMap() = default;

  /**
   * @brief Return the node for an id, creating it if it does not exist.
   * @param node_id The node identifier.
   * @return The node, or an error status on failure.
   */
  // By value: on a miss the id is moved into the map, and callers hand it
  // over rather than lending it.
  // NOLINTNEXTLINE(performance-unnecessary-value-param)
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

  /// Configure a host-accessible namespace and its backend node map.
  absl::Status RegisterStorage(std::string storage_id,
                               std::shared_ptr<NodeMap> storage);

  /// Identity of this map's backend, or absent for session-local storage.
  [[nodiscard]] const std::optional<data::StorageIdentity>& Storage() const {
    return storage_;
  }

  /// Adopt host connection policy; advertised ids must be directly accessible.
  absl::Status SetStorageResolver(StorageResolver resolver,
                                  std::vector<std::string> accessible_ids = {});
  /// Storage identities directly accessible through this host's resolver.
  [[nodiscard]] std::vector<std::string> StorageIds() const;

  /// Permit read resolution to open uncached nodes through this map's factory.
  void SetAllowLazyReadResolution(bool allowed);
  [[nodiscard]] bool AllowsLazyReadResolution() const;

  /// Resolve a whole-node reference; windows require a fragment-window reader.
  absl::StatusOr<std::shared_ptr<AsyncNode>> Resolve(
      const data::NodeRef& reference);
  /// Resolve an existing local target for a value read.
  absl::StatusOr<std::shared_ptr<AsyncNode>> ResolveForRead(
      const data::NodeRef& reference);

  /// Share a referenced node from another scope, preserving its identity.
  absl::StatusOr<std::shared_ptr<AsyncNode>> ImportReference(
      const data::NodeRef& reference, const std::shared_ptr<NodeMap>& source);

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
  explicit NodeMap(ChunkStoreFactory factory,
                   std::optional<data::StorageIdentity> storage);

  absl::Status Insert(const std::shared_ptr<AsyncNode>& node);

  const ChunkStoreFactory factory_;
  const std::optional<data::StorageIdentity> storage_;
  mutable thread::Mutex mu_;
  StorageResolver storage_resolver_ ABSL_GUARDED_BY(mu_);
  std::vector<std::string> accessible_ids_ ABSL_GUARDED_BY(mu_);
  bool allow_lazy_read_resolution_ ABSL_GUARDED_BY(mu_) = false;
  absl::flat_hash_map<std::string, std::shared_ptr<NodeMap>> storages_
      ABSL_GUARDED_BY(mu_);
  absl::flat_hash_map<
      std::string, absl::flat_hash_map<std::string, std::shared_ptr<AsyncNode>>>
      imported_ ABSL_GUARDED_BY(mu_);
  absl::flat_hash_map<std::string, std::shared_ptr<AsyncNode>> nodes_
      ABSL_GUARDED_BY(mu_);
  absl::flat_hash_map<std::string, std::shared_ptr<AsyncNode>> retained_refs_
      ABSL_GUARDED_BY(mu_);
};

}  // namespace a11::nodes

#endif  // A11_NODES_NODE_MAP_H_
