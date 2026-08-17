// Copyright 2026 The A11 Authors.

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

#include "a11/nodes/internal/exception_guarded_factory.h"

#include <memory>
#include <string>
#include <utility>

#include "a11/internal/exception_guard_impl.h"
#include "a11/nodes/node_map.h"
#include "a11/stores/chunk_store.h"

namespace a11::nodes::internal {

ChunkStoreFactory GuardFactory(ChunkStoreFactory factory) {
  return exception_guard::Wrap<
      absl::StatusOr<std::shared_ptr<stores::ChunkStore>>, std::string>(
      std::move(factory), "chunk-store factory");
}

}  // namespace a11::nodes::internal
