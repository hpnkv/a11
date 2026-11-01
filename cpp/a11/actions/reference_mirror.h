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

#ifndef A11_ACTIONS_REFERENCE_MIRROR_H_
#define A11_ACTIONS_REFERENCE_MIRROR_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/statusor.h>

#include "a11/data/types.h"

namespace a11::net {
class WireStream;
}  // namespace a11::net

namespace a11::nodes {
class NodeMap;
}  // namespace a11::nodes

namespace a11::actions {

/// Reserved node-id prefix for transport-created reference targets.
inline constexpr std::string_view kReferenceMirrorPrefix = "a11_mirror_";

/// Rewrites inaccessible references to ordered, receiver-owned mirror nodes.
class ReferenceMirrorRegistry {
 public:
  /// Configure one peer's transport and directly readable storage namespaces.
  ReferenceMirrorRegistry(std::shared_ptr<net::WireStream> stream,
                          std::vector<std::string> direct_storage_ids);

  /// Start or reuse a mirror and return the peer-readable reference.
  absl::StatusOr<data::NodeRef> Rewrite(
      const data::NodeRef& reference,
      const std::shared_ptr<nodes::NodeMap>& source_map);

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace a11::actions

#endif  // A11_ACTIONS_REFERENCE_MIRROR_H_
