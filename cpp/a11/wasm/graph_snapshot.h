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

#ifndef A11_WASM_GRAPH_SNAPSHOT_H_
#define A11_WASM_GRAPH_SNAPSHOT_H_

#include <memory>
#include <string>
#include <string_view>

#include <absl/container/flat_hash_map.h>
#include <absl/status/statusor.h>

#include "thread/boost_primitives.h"

namespace a11::nodes {
class NodeMap;
}  // namespace a11::nodes

namespace a11::stores {
class LocalChunkStore;
}  // namespace a11::stores

namespace a11::wasm {

struct StoreRegistry {
  thread::Mutex mu;
  absl::flat_hash_map<std::string, std::shared_ptr<stores::LocalChunkStore>>
      stores ABSL_GUARDED_BY(mu);
};

struct RestoredStores {
  std::shared_ptr<nodes::NodeMap> nodes;
  std::shared_ptr<StoreRegistry> registry;
};

absl::StatusOr<std::shared_ptr<nodes::NodeMap>> MakeTrackedNodeMap(
    const std::shared_ptr<StoreRegistry>& registry,
    std::string storage_id);
absl::StatusOr<std::string> ExportGraphSnapshot(StoreRegistry& registry,
                                                const std::string& storage_id);
absl::StatusOr<RestoredStores> RestoreGraphSnapshot(
    std::string_view bytes, const std::string& storage_id);

}  // namespace a11::wasm

#endif  // A11_WASM_GRAPH_SNAPSHOT_H_
