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
