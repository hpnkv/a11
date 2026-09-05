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

/**
 * @file
 * @brief The nodes library's exception boundary.
 *
 * Compiled with exceptions (see the exception policy block in
 * cpp/CMakeLists.txt). A NodeMap's factory is the caller's -- often a Python
 * callable that builds a ChunkStore per node id -- and NodeMap invokes it from
 * its own frames, so it is wrapped here where a raised exception can still be
 * caught. a11/exception_guard.h has the reasoning.
 */

#include <memory>
#include <string>
#include <utility>

#include "a11/internal/exception_guard_impl.h"
#include "a11/nodes/internal/exception_guarded_factory.h"
#include "a11/nodes/node_map.h"
#include "a11/stores/chunk_store.h"

namespace a11::nodes::internal {

ChunkStoreFactory GuardFactory(ChunkStoreFactory factory) {
  return exception_guard::Wrap<
      absl::StatusOr<std::shared_ptr<stores::ChunkStore>>, std::string>(
      std::move(factory), "chunk-store factory");
}

}  // namespace a11::nodes::internal
