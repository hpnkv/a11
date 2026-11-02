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

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <nlohmann/json.hpp>

#include "a11/actions/stream_reference.h"
#include "a11/data/json.h"
#include "a11/data/types.h"
#include "a11/json_codec.h"
#include "bridge_core.h"

extern "C" {
Result* a11_wasm_stream_reference_to_chunk(const std::uint8_t* data,
                                            size_t size) {
  auto value = a11::ParseJson(View(data, size), "stream reference");
  if (!value.ok()) return Failure(value.status());
  if (!value->is_object()) {
    return Failure(absl::InvalidArgumentError(
        "Stream reference must be an object"));
  }
  const auto producer = value->find("producer_id");
  const auto purpose = value->find("purpose");
  const auto node_value = value->find("node");
  const auto source_value = value->find("source");
  const auto parent = value->find("parent_producer_id");
  if (producer == value->end() || !producer->is_string() ||
      purpose == value->end() || !purpose->is_string() ||
      node_value == value->end() || !node_value->is_object() ||
      source_value == value->end() || !source_value->is_object() ||
      (parent != value->end() && !parent->is_string())) {
    return Failure(absl::InvalidArgumentError(
        "Invalid stream reference fields"));
  }
  auto node = a11::data::NodeRefFromJsonValue(*node_value);
  if (!node.ok()) return Failure(node.status());
  auto source = a11::DumpJson(*source_value, "stream source");
  if (!source.ok()) return Failure(source.status());
  a11::actions::StreamReference reference{
      .producer_id = producer->get<std::string>(),
      .parent_producer_id = parent == value->end() ? std::string{} :
                             parent->get<std::string>(),
      .node = std::move(*node),
      .purpose = purpose->get<std::string>(),
      .source = a11::data::Chunk{
          .metadata = a11::data::ChunkMetadata{
              .mimetype = "application/json"},
          .data = std::move(*source)}};
  auto chunk = a11::actions::StreamReferenceToChunk(reference);
  if (!chunk.ok()) return Failure(chunk.status());
  auto bytes = chunk->ToMsgpack();
  return bytes.ok() ? new Result{0, std::move(*bytes)}
                    : Failure(bytes.status());
}

Result* a11_wasm_stream_reference_from_chunk(const std::uint8_t* data,
                                              size_t size) {
  auto chunk = a11::data::Chunk::FromMsgpack(View(data, size));
  if (!chunk.ok()) return Failure(chunk.status());
  auto reference = a11::actions::StreamReferenceFromChunk(std::move(*chunk));
  if (!reference.ok()) return Failure(reference.status());
  auto node = a11::data::NodeRefToJsonValue(reference->node);
  if (!node.ok()) return Failure(node.status());
  auto source = a11::ParseJson(reference->source.data, "stream source");
  if (!source.ok()) return Failure(source.status());
  nlohmann::json value = {
      {"producer_id", reference->producer_id},
      {"parent_producer_id", reference->parent_producer_id},
      {"node", std::move(*node)},
      {"purpose", reference->purpose},
      {"source", std::move(*source)}};
  auto encoded = a11::DumpJson(value, "stream reference");
  return encoded.ok() ? new Result{0, std::move(*encoded)}
                      : Failure(encoded.status());
}
}  // extern "C"
