// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Adoption guard for a NodeMap's chunk-store factory.
 *
 * Implemented in a11/nodes/boundary.cc, which is compiled with exceptions for
 * this purpose. See a11/exception_guard.h.
 */

#ifndef A11_NODES_INTERNAL_EXCEPTION_GUARDED_FACTORY_H_
#define A11_NODES_INTERNAL_EXCEPTION_GUARDED_FACTORY_H_

#include "a11/nodes/node_map.h"

namespace a11::nodes::internal {

// / Wraps a factory so a raised exception becomes the error status NodeMap
// reads.
[[nodiscard]] ChunkStoreFactory GuardFactory(ChunkStoreFactory factory);

}  // namespace a11::nodes::internal

#endif  // A11_NODES_INTERNAL_EXCEPTION_GUARDED_FACTORY_H_
