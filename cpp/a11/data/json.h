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

#ifndef A11_DATA_JSON_H_
#define A11_DATA_JSON_H_

#include <string>
#include <string_view>

#include <absl/status/statusor.h>
#include <nlohmann/json_fwd.hpp>

#include "a11/data/types.h"

namespace a11::data {

/// Encode a ChunkMetadata using the native wire JSON representation.
absl::StatusOr<nlohmann::json> ChunkMetadataToJsonValue(
    const ChunkMetadata& value);
/// Decode and validate a ChunkMetadata from native wire JSON.
absl::StatusOr<ChunkMetadata> ChunkMetadataFromJsonValue(
    const nlohmann::json& value);

/// Encode a Chunk using the native wire JSON representation.
absl::StatusOr<nlohmann::json> ChunkToJsonValue(const Chunk& value);
/// Decode and validate a Chunk from native wire JSON.
absl::StatusOr<Chunk> ChunkFromJsonValue(const nlohmann::json& value);

/// Encode a NodeRef using the native wire JSON representation.
absl::StatusOr<nlohmann::json> NodeRefToJsonValue(const NodeRef& value);
/// Decode and validate a NodeRef from native wire JSON.
absl::StatusOr<NodeRef> NodeRefFromJsonValue(const nlohmann::json& value);

/// Encode a NodeFragment using the native wire JSON representation.
absl::StatusOr<nlohmann::json> NodeFragmentToJsonValue(
    const NodeFragment& value);
/// Decode and validate a NodeFragment from native wire JSON.
absl::StatusOr<NodeFragment> NodeFragmentFromJsonValue(
    const nlohmann::json& value);

/// Encode a Port using the native wire JSON representation.
absl::StatusOr<nlohmann::json> PortToJsonValue(const Port& value);
/// Decode and validate a Port from native wire JSON.
absl::StatusOr<Port> PortFromJsonValue(const nlohmann::json& value);

/// Encode a ActionMessage using the native wire JSON representation.
absl::StatusOr<nlohmann::json> ActionMessageToJsonValue(
    const ActionMessage& value);
/// Decode and validate a ActionMessage from native wire JSON.
absl::StatusOr<ActionMessage> ActionMessageFromJsonValue(
    const nlohmann::json& value);

// JSON encoding used by A11's HTTP/SSE transports.
absl::StatusOr<nlohmann::json> WireMessageToJsonValue(
    const WireMessage& message);
absl::StatusOr<std::string> WireMessageToJson(const WireMessage& message);
absl::StatusOr<WireMessage> WireMessageFromJsonValue(
    const nlohmann::json& value);
absl::StatusOr<WireMessage> WireMessageFromJson(std::string_view encoded);

}  // namespace a11::data

#endif  // A11_DATA_JSON_H_
