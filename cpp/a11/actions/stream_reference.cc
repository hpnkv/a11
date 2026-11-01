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

#include "a11/actions/stream_reference.h"

#include <string>
#include <utility>

#include <absl/status/status_macros.h>
#include <nlohmann/json.hpp>

#include "a11/data/json.h"
#include "a11/json_codec.h"

namespace a11::actions {
namespace {
constexpr size_t kMaxReferenceBytes = 16384;
}  // namespace

absl::Status StreamReference::Validate() const {
  if (producer_id.empty() || purpose.empty()) {
    return absl::InvalidArgumentError(
        "Stream producer and purpose are required");
  }
  return node.Validate();
}

absl::StatusOr<data::Chunk> StreamReferenceToChunk(
    const StreamReference& reference) {
  ABSL_RETURN_IF_ERROR(reference.Validate());
  data::Chunk source = reference.source;
  if (!source.metadata.has_value() ||
      source.metadata->mimetype != "application/json") {
    return absl::InvalidArgumentError(
        "Stream source requires application/json metadata");
  }
  ABSL_RETURN_IF_ERROR(source.Materialize());
  ABSL_ASSIGN_OR_RETURN(auto source_value,
                        ParseJson(source.data, "stream source"));
  if (!source_value.is_object()) {
    return absl::InvalidArgumentError("Stream source must be a JSON object");
  }
  ABSL_ASSIGN_OR_RETURN(auto node, data::NodeRefToJsonValue(reference.node));
  nlohmann::json value = {{"producer_id", reference.producer_id},
                          {"parent_producer_id", reference.parent_producer_id},
                          {"node", std::move(node)},
                          {"purpose", reference.purpose},
                          {"source", std::move(source_value)}};
  ABSL_ASSIGN_OR_RETURN(std::string encoded,
                        DumpJson(value, "stream reference"));
  if (encoded.size() > kMaxReferenceBytes) {
    return absl::ResourceExhaustedError("Stream reference exceeds 16384 bytes");
  }
  return data::Chunk{
      .metadata =
          data::ChunkMetadata{
              .mimetype = "application/json",
              .attributes = {{std::string(kStreamReferenceAttribute), "1"}}},
      .data = std::move(encoded)};
}

absl::StatusOr<StreamReference> StreamReferenceFromChunk(data::Chunk chunk) {
  if (!chunk.metadata.has_value() ||
      chunk.metadata->mimetype != "application/json") {
    return absl::InvalidArgumentError(
        "Stream reference requires application/json metadata");
  }
  ABSL_RETURN_IF_ERROR(chunk.Materialize());
  if (chunk.data.size() > kMaxReferenceBytes) {
    return absl::ResourceExhaustedError("Stream reference exceeds 16384 bytes");
  }
  ABSL_ASSIGN_OR_RETURN(auto value, ParseJson(chunk.data, "stream reference"));
  if (!value.is_object()) {
    return absl::InvalidArgumentError("Stream reference must be an object");
  }
  for (const auto key : {"producer_id", "purpose"}) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_string()) {
      return absl::InvalidArgumentError(
          "Stream reference requires string identifiers");
    }
  }
  const auto node_value = value.find("node");
  if (node_value == value.end() || !node_value->is_object()) {
    return absl::InvalidArgumentError("Stream reference requires a NodeRef");
  }
  ABSL_ASSIGN_OR_RETURN(auto node, data::NodeRefFromJsonValue(*node_value));
  const auto parent = value.find("parent_producer_id");
  if (parent != value.end() && !parent->is_string()) {
    return absl::InvalidArgumentError("Parent producer id must be a string");
  }
  const auto source = value.find("source");
  if (source == value.end() || !source->is_object()) {
    return absl::InvalidArgumentError("Stream source must be an object");
  }
  ABSL_ASSIGN_OR_RETURN(std::string encoded_source,
                        DumpJson(*source, "stream source"));
  StreamReference reference{
      .producer_id = value["producer_id"].get<std::string>(),
      .parent_producer_id =
          parent == value.end() ? "" : parent->get<std::string>(),
      .node = std::move(node),
      .purpose = value["purpose"].get<std::string>(),
      .source = data::Chunk{
          .metadata = data::ChunkMetadata{.mimetype = "application/json"},
          .data = std::move(encoded_source)}};
  ABSL_RETURN_IF_ERROR(reference.Validate());
  return reference;
}

}  // namespace a11::actions
