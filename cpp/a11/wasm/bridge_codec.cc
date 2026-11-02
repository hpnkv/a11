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
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/data/json.h"
#include "a11/data/msgpack.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/flow/service.h"
#include "a11/json_codec.h"
#include "a11/net/byte_chunking.h"
#include "a11/status.h"
#include "bridge_core.h"
#include "runtime.h"

namespace {
template <typename T>
Result* EncodeData(const nlohmann::json& value,
                   absl::StatusOr<T> (*decode)(const nlohmann::json&)) {
  auto parsed = decode(value);
  if (!parsed.ok()) return Failure(parsed.status());
  auto bytes = parsed->ToMsgpack();
  return bytes.ok() ? new Result{0, std::move(*bytes)}
                    : Failure(bytes.status());
}

template <typename T>
Result* DecodeData(std::string_view bytes,
                   absl::StatusOr<nlohmann::json> (*encode)(const T&)) {
  auto parsed = T::FromMsgpack(bytes);
  if (!parsed.ok()) return Failure(parsed.status());
  auto value = encode(*parsed);
  if (!value.ok()) return Failure(value.status());
  auto json = a11::DumpJson(*value, "A11 data JSON");
  return json.ok() ? new Result{0, std::move(*json)}
                   : Failure(json.status());
}
}  // namespace

extern "C" {
Result* a11_wasm_name_validate(const char* data, size_t size) {
  const absl::Status status = a11::data::ValidateName(View(data, size));
  return status.ok() ? new Result{0, {}} : Failure(status);
}

Result* a11_wasm_data_encode(const char* kind_data, size_t kind_size,
                             const std::uint8_t* data, size_t size) {
  const std::string_view kind = View(kind_data, kind_size);
  auto value = a11::ParseJson(View(data, size), "A11 data JSON");
  if (!value.ok()) return Failure(value.status());
  if (kind == "chunk_metadata") {
    return EncodeData(*value, a11::data::ChunkMetadataFromJsonValue);
  }
  if (kind == "chunk") return EncodeData(*value, a11::data::ChunkFromJsonValue);
  if (kind == "node_ref") {
    return EncodeData(*value, a11::data::NodeRefFromJsonValue);
  }
  if (kind == "node_fragment") {
    return EncodeData(*value, a11::data::NodeFragmentFromJsonValue);
  }
  if (kind == "port") return EncodeData(*value, a11::data::PortFromJsonValue);
  if (kind == "action_message") {
    return EncodeData(*value, a11::data::ActionMessageFromJsonValue);
  }
  if (kind == "wire_message") {
    return EncodeData(*value, a11::data::WireMessageFromJsonValue);
  }
  return Failure(absl::InvalidArgumentError("Unknown A11 data kind"));
}

Result* a11_wasm_data_decode(const char* kind_data, size_t kind_size,
                             const std::uint8_t* data, size_t size) {
  const std::string_view kind = View(kind_data, kind_size);
  const std::string_view bytes = View(data, size);
  if (kind == "chunk_metadata") {
    return DecodeData<a11::data::ChunkMetadata>(
        bytes, a11::data::ChunkMetadataToJsonValue);
  }
  if (kind == "chunk") {
    return DecodeData<a11::data::Chunk>(bytes, a11::data::ChunkToJsonValue);
  }
  if (kind == "node_ref") {
    return DecodeData<a11::data::NodeRef>(bytes, a11::data::NodeRefToJsonValue);
  }
  if (kind == "node_fragment") {
    return DecodeData<a11::data::NodeFragment>(
        bytes, a11::data::NodeFragmentToJsonValue);
  }
  if (kind == "port") {
    return DecodeData<a11::data::Port>(bytes, a11::data::PortToJsonValue);
  }
  if (kind == "action_message") {
    return DecodeData<a11::data::ActionMessage>(
        bytes, a11::data::ActionMessageToJsonValue);
  }
  if (kind == "wire_message") {
    return DecodeData<a11::data::WireMessage>(
        bytes, a11::data::WireMessageToJsonValue);
  }
  return Failure(absl::InvalidArgumentError("Unknown A11 data kind"));
}

Result* a11_wasm_json_to_chunk(const std::uint8_t* data, size_t size,
                               const char* mimetype_data,
                               size_t mimetype_size) {
  auto value = a11::ParseJson(View(data, size), "JSON value");
  if (!value.ok()) return Failure(value.status());
  auto chunk = a11::data::GlobalSerializationRegistry().ToChunk(
      *value, View(mimetype_data, mimetype_size));
  if (!chunk.ok()) return Failure(chunk.status());
  auto bytes = chunk->ToMsgpack();
  return bytes.ok() ? new Result{0, std::move(*bytes)}
                    : Failure(bytes.status());
}

Result* a11_wasm_json_from_chunk(const std::uint8_t* data, size_t size,
                                 const char* patterns_data,
                                 size_t patterns_size) {
  auto chunk = a11::data::Chunk::FromMsgpack(View(data, size));
  if (!chunk.ok()) return Failure(chunk.status());
  std::vector<std::string> patterns;
  if (patterns_size != 0) {
    auto parsed = a11::ParseJson(View(patterns_data, patterns_size),
                                 "mimetype selectors");
    if (!parsed.ok()) return Failure(parsed.status());
    if (!parsed->is_array()) {
      return Failure(absl::InvalidArgumentError(
          "Mimetype selectors must be an array"));
    }
    for (const auto& pattern : *parsed) {
      if (!pattern.is_string()) {
        return Failure(absl::InvalidArgumentError(
            "Mimetype selector must be a string"));
      }
      patterns.push_back(pattern.get<std::string>());
    }
  }
  auto value = a11::data::GlobalSerializationRegistry()
                   .FromChunk<nlohmann::json>(*chunk, patterns);
  if (!value.ok()) return Failure(value.status());
  auto encoded = a11::DumpJson(*value, "JSON value");
  return encoded.ok() ? new Result{0, std::move(*encoded)}
                      : Failure(encoded.status());
}

Result* a11_wasm_bytes_to_chunk(const std::uint8_t* data, size_t size,
                                const char* mimetype_data,
                                size_t mimetype_size) {
  auto chunk = a11::data::GlobalSerializationRegistry().ToChunk(
      std::string(View(data, size)), View(mimetype_data, mimetype_size));
  if (!chunk.ok()) return Failure(chunk.status());
  auto encoded = chunk->ToMsgpack();
  return encoded.ok() ? new Result{0, std::move(*encoded)}
                      : Failure(encoded.status());
}

Result* a11_wasm_bytes_from_chunk(const std::uint8_t* data, size_t size) {
  auto chunk = a11::data::Chunk::FromMsgpack(View(data, size));
  if (!chunk.ok()) return Failure(chunk.status());
  auto value = a11::data::GlobalSerializationRegistry()
                   .FromChunk<std::string>(*chunk);
  return value.ok() ? new Result{0, std::move(*value)}
                    : Failure(value.status());
}

Result* a11_wasm_status_pack(const std::uint8_t* data, size_t size) {
  auto value = a11::ParseJson(View(data, size), "Status JSON");
  if (!value.ok()) return Failure(value.status());
  auto status = a11::StatusFromJson(*value);
  if (!status.ok()) return Failure(status.status());
  auto bytes = a11::data::PackStatus(*status);
  return bytes.ok() ? new Result{0, std::move(*bytes)}
                    : Failure(bytes.status());
}

Result* a11_wasm_status_unpack(const std::uint8_t* data, size_t size) {
  auto status = a11::data::UnpackStatus(View(data, size));
  if (!status.ok()) return Failure(status.status());
  auto encoded = a11::DumpJson(a11::StatusToJsonOrEmptyDetails(*status),
                               "Status JSON");
  return encoded.ok() ? new Result{0, std::move(*encoded)}
                      : Failure(encoded.status());
}

Result* a11_wasm_status_chunk(const std::uint8_t* data, size_t size,
                              int closing) {
  auto value = a11::ParseJson(View(data, size), "Status JSON");
  if (!value.ok()) return Failure(value.status());
  auto status = a11::StatusFromJson(*value);
  if (!status.ok()) return Failure(status.status());
  auto chunk = a11::data::MakeStatusChunk(*status, closing != 0);
  if (!chunk.ok()) return Failure(chunk.status());
  auto encoded = chunk->ToMsgpack();
  return encoded.ok() ? new Result{0, std::move(*encoded)}
                      : Failure(encoded.status());
}

Result* a11_wasm_status_from_chunk(const std::uint8_t* data, size_t size) {
  auto chunk = a11::data::Chunk::FromMsgpack(View(data, size));
  if (!chunk.ok()) return Failure(chunk.status());
  auto status = a11::data::StatusFromStatusChunk(*chunk);
  if (!status.ok()) return Failure(status.status());
  auto json = a11::DumpJson(a11::StatusToJsonOrEmptyDetails(*status),
                            "Status JSON");
  return json.ok() ? new Result{0, std::move(*json)}
                   : Failure(json.status());
}

Result* a11_wasm_wire_encode(const std::uint8_t* data, size_t size) {
  auto message = a11::data::WireMessageFromJson(View(data, size));
  if (!message.ok()) {
    return Failure(message.status());
  }
  auto bytes = message->ToMsgpack();
  if (!bytes.ok()) {
    return Failure(bytes.status());
  }
  return new Result{0, std::move(*bytes)};
}

Result* a11_wasm_wire_decode(const std::uint8_t* data, size_t size) {
  auto message = a11::data::WireMessage::FromMsgpack(View(data, size));
  if (!message.ok()) {
    return Failure(message.status());
  }
  auto encoded = a11::data::WireMessageToJson(*message);
  if (!encoded.ok()) {
    return Failure(encoded.status());
  }
  return new Result{0, std::move(*encoded)};
}

Result* a11_wasm_fragment_decode(const std::uint8_t* data, size_t size) {
  auto fragment = a11::data::NodeFragment::FromMsgpack(View(data, size));
  if (!fragment.ok()) {
    return Failure(fragment.status());
  }
  auto json = a11::data::NodeFragmentToJsonValue(*fragment);
  if (!json.ok()) {
    return Failure(json.status());
  }
  auto encoded = a11::DumpJson(*json, "fragment");
  if (!encoded.ok()) {
    return Failure(encoded.status());
  }
  return new Result{0, std::move(*encoded)};
}

}  // extern "C"
