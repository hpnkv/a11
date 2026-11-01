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

#ifndef A11_ACTIONS_STREAM_REFERENCE_H_
#define A11_ACTIONS_STREAM_REFERENCE_H_

#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/data/types.h"

namespace a11::actions {

/// Metadata identifying a reference chunk on a scope's reserved output.
inline constexpr std::string_view kStreamReferenceAttribute =
    "a11-stream-reference";

/// Locate an auxiliary producer independently of its stream payload.
struct StreamReference {
  std::string producer_id{};         ///< Stable logical producer identity.
  std::string parent_producer_id{};  ///< Enclosing producer, or empty.
  data::NodeRef node{};              ///< Node readable in the receiving scope.
  std::string purpose{};             ///< Payload protocol, such as llm_usage.
  data::Chunk source{};  ///< Opaque source metadata as a JSON object.

  /// Validate the identifiers independently of source serialization.
  absl::Status Validate() const;

  friend bool operator==(const StreamReference&,
                         const StreamReference&) = default;
};

/// Encode a reference and its source as a plain JSON object chunk.
absl::StatusOr<data::Chunk> StreamReferenceToChunk(
    const StreamReference& reference);

/// Decode a reference, preserving source metadata as an opaque JSON chunk.
absl::StatusOr<StreamReference> StreamReferenceFromChunk(data::Chunk chunk);

}  // namespace a11::actions

#endif  // A11_ACTIONS_STREAM_REFERENCE_H_
