// Copyright 2026 The A11 Authors.

#include "a11/data/json.h"

#include "a11/json_codec.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/escaping.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/data/types.h"

namespace a11::data {
namespace {

using Json = nlohmann::json;

std::string EncodeBytes(std::string_view bytes) {
  return absl::Base64Escape(bytes);
}

absl::StatusOr<std::string> DecodeBytes(const Json& value,
                                        std::string_view field) {
  if (!value.is_string()) {
    return absl::InvalidArgumentError(
        absl::StrCat(field, " must be a base64 string"));
  }
  std::string decoded;
  if (!absl::Base64Unescape(value.get_ref<const std::string&>(), &decoded)) {
    return absl::InvalidArgumentError(
        absl::StrCat(field, " is not valid base64"));
  }
  return decoded;
}

absl::StatusOr<const Json*> GetRequired(const Json& object,
                                        std::string_view key,
                                        std::string_view context) {
  if (!object.is_object()) {
    return absl::InvalidArgumentError(
        absl::StrCat(context, " must be a JSON object"));
  }
  const auto iterator = object.find(key);
  if (iterator == object.end()) {
    return absl::InvalidArgumentError(
        absl::StrCat(context, " is missing field ", key));
  }
  return &*iterator;
}

const Json* GetOptional(const Json& object, std::string_view key) {
  if (!object.is_object()) {
    return nullptr;
  }
  const auto iterator = object.find(key);
  return iterator == object.end() ? nullptr : &*iterator;
}

absl::StatusOr<std::string> GetString(const Json& object, std::string_view key,
                                      std::string_view default_value,
                                      std::string_view context) {
  const Json* value = GetOptional(object, key);
  if (value == nullptr) {
    return std::string(default_value);
  }
  if (!value->is_string()) {
    return absl::InvalidArgumentError(
        absl::StrCat(context, ".", key, " must be a string"));
  }
  return value->get<std::string>();
}

absl::StatusOr<std::uint64_t> GetUnsigned(const Json& object,
                                          std::string_view key,
                                          std::uint64_t default_value,
                                          std::uint64_t maximum,
                                          std::string_view context) {
  const Json* value = GetOptional(object, key);
  if (value == nullptr) {
    return default_value;
  }
  if (!value->is_number_unsigned() && !value->is_number_integer()) {
    return absl::InvalidArgumentError(
        absl::StrCat(context, ".", key, " must be an unsigned integer"));
  }
  if (value->is_number_integer() && value->get<std::int64_t>() < 0) {
    return absl::OutOfRangeError(
        absl::StrCat(context, ".", key, " must not be negative"));
  }
  const std::uint64_t result = value->get<std::uint64_t>();
  if (result > maximum) {
    return absl::OutOfRangeError(
        absl::StrCat(context, ".", key, " exceeds its supported range"));
  }
  return result;
}

Json EncodeByteMap(const ByteMap& values) {
  Json result = Json::object();
  for (const auto& [key, value] : values) {
    result[key] = EncodeBytes(value);
  }
  return result;
}

absl::StatusOr<ByteMap> DecodeByteMap(const Json* value,
                                      std::string_view context) {
  ByteMap result;
  if (value == nullptr) {
    return result;
  }
  if (!value->is_object()) {
    return absl::InvalidArgumentError(
        absl::StrCat(context, " must be an object"));
  }
  for (auto iterator = value->begin(); iterator != value->end(); ++iterator) {
    ABSL_RETURN_IF_ERROR(ValidateName(iterator.key()));
    ABSL_ASSIGN_OR_RETURN(
        std::string decoded,
        DecodeBytes(iterator.value(),
                    absl::StrCat(context, ".", iterator.key())));
    result.emplace(iterator.key(), std::move(decoded));
  }
  return result;
}

Json EncodeMetadata(const ChunkMetadata& metadata) {
  Json value = Json::object();
  value["mimetype"] = metadata.mimetype;
  if (metadata.timestamp.has_value()) {
    value["timestamp"] = absl::FormatTime(
        "%Y-%m-%dT%H:%M:%E*S%Ez", *metadata.timestamp, absl::UTCTimeZone());
  }
  if (!metadata.attributes.empty()) {
    value["attributes"] = EncodeByteMap(metadata.attributes);
  }
  return value;
}

absl::StatusOr<ChunkMetadata> DecodeMetadata(const Json& value) {
  ABSL_ASSIGN_OR_RETURN(std::string mimetype,
                        GetString(value, "mimetype", "", "ChunkMetadata"));
  std::optional<absl::Time> timestamp;
  if (const Json* timestamp_value = GetOptional(value, "timestamp");
      timestamp_value != nullptr && !timestamp_value->is_null()) {
    if (!timestamp_value->is_string()) {
      return absl::InvalidArgumentError(
          "ChunkMetadata.timestamp must be an RFC 3339 string or null");
    }
    absl::Time parsed;
    std::string error;
    if (!absl::ParseTime(absl::RFC3339_full,
                         timestamp_value->get_ref<const std::string&>(),
                         &parsed, &error)) {
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid ChunkMetadata.timestamp: ", error));
    }
    timestamp = parsed;
  }
  ABSL_ASSIGN_OR_RETURN(ByteMap attributes,
                        DecodeByteMap(GetOptional(value, "attributes"),
                                      "ChunkMetadata.attributes"));
  ChunkMetadata result{.mimetype = std::move(mimetype),
                       .timestamp = timestamp,
                       .attributes = std::move(attributes)};
  ABSL_RETURN_IF_ERROR(result.Validate());
  return result;
}

Json EncodeChunk(const Chunk& chunk) {
  Json value = Json::object();
  if (chunk.metadata.has_value()) {
    value["metadata"] = EncodeMetadata(*chunk.metadata);
  }
  if (!chunk.ref.empty()) {
    value["ref"] = chunk.ref;
  }
  // Emitting data even when empty makes the Chunk/NodeRef union unambiguous.
  value["data"] = EncodeBytes(chunk.data);
  return value;
}

absl::StatusOr<Chunk> DecodeChunk(const Json& value) {
  if (!value.is_object()) {
    return absl::InvalidArgumentError("Chunk must be a JSON object");
  }
  std::optional<ChunkMetadata> metadata;
  if (const Json* metadata_value = GetOptional(value, "metadata");
      metadata_value != nullptr && !metadata_value->is_null()) {
    ABSL_ASSIGN_OR_RETURN(ChunkMetadata decoded,
                          DecodeMetadata(*metadata_value));
    metadata = std::move(decoded);
  }
  ABSL_ASSIGN_OR_RETURN(std::string ref, GetString(value, "ref", "", "Chunk"));
  std::string data;
  if (const Json* data_value = GetOptional(value, "data");
      data_value != nullptr) {
    ABSL_ASSIGN_OR_RETURN(std::string decoded,
                          DecodeBytes(*data_value, "Chunk.data"));
    data = std::move(decoded);
  }
  Chunk result{.metadata = std::move(metadata),
               .ref = std::move(ref),
               .data = std::move(data)};
  ABSL_RETURN_IF_ERROR(result.Validate());
  return result;
}

Json EncodeNodeRef(const NodeRef& node_ref) {
  Json value = Json::object({{"id", node_ref.id}});
  if (node_ref.offset != 0) {
    value["offset"] = node_ref.offset;
  }
  if (node_ref.length.has_value()) {
    value["length"] = *node_ref.length;
  }
  return value;
}

absl::StatusOr<NodeRef> DecodeNodeRef(const Json& value) {
  ABSL_ASSIGN_OR_RETURN(std::string id, GetString(value, "id", "", "NodeRef"));
  ABSL_ASSIGN_OR_RETURN(
      std::uint64_t offset,
      GetUnsigned(value, "offset", 0, std::numeric_limits<std::uint32_t>::max(),
                  "NodeRef"));
  std::optional<std::uint64_t> length;
  if (const Json* length_value = GetOptional(value, "length");
      length_value != nullptr && !length_value->is_null()) {
    ABSL_ASSIGN_OR_RETURN(
        std::uint64_t decoded,
        GetUnsigned(value, "length", 0,
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::uint32_t>::max()) +
                        1,
                    "NodeRef"));
    length = decoded;
  }
  NodeRef result{.id = std::move(id),
                 .offset = static_cast<std::uint32_t>(offset),
                 .length = length};
  ABSL_RETURN_IF_ERROR(result.Validate());
  return result;
}

Json EncodeFragment(const NodeFragment& fragment) {
  Json value = Json::object();
  if (!fragment.id.empty()) {
    value["id"] = fragment.id;
  }
  if (const Chunk* chunk = std::get_if<Chunk>(&fragment.data)) {
    value["data"] = EncodeChunk(*chunk);
  } else {
    value["data"] = EncodeNodeRef(std::get<NodeRef>(fragment.data));
  }
  if (fragment.seq.has_value()) {
    value["seq"] = *fragment.seq;
  }
  if (fragment.continued) {
    value["continued"] = true;
  }
  return value;
}

absl::StatusOr<NodeFragment> DecodeFragment(const Json& value) {
  ABSL_ASSIGN_OR_RETURN(std::string id,
                        GetString(value, "id", "", "NodeFragment"));
  ABSL_ASSIGN_OR_RETURN(const Json* data_value,
                        GetRequired(value, "data", "NodeFragment"));
  if (!data_value->is_object()) {
    return absl::InvalidArgumentError(
        "NodeFragment.data must be a JSON object");
  }
  std::variant<Chunk, NodeRef> data;
  if (GetOptional(*data_value, "id") != nullptr &&
      GetOptional(*data_value, "data") == nullptr &&
      GetOptional(*data_value, "ref") == nullptr &&
      GetOptional(*data_value, "metadata") == nullptr) {
    ABSL_ASSIGN_OR_RETURN(NodeRef decoded, DecodeNodeRef(*data_value));
    data = std::move(decoded);
  } else {
    ABSL_ASSIGN_OR_RETURN(Chunk decoded, DecodeChunk(*data_value));
    data = std::move(decoded);
  }
  std::optional<std::uint32_t> seq;
  if (const Json* seq_value = GetOptional(value, "seq");
      seq_value != nullptr && !seq_value->is_null()) {
    ABSL_ASSIGN_OR_RETURN(
        std::uint64_t decoded,
        GetUnsigned(value, "seq", 0, std::numeric_limits<std::uint32_t>::max(),
                    "NodeFragment"));
    seq = static_cast<std::uint32_t>(decoded);
  }
  bool continued = false;
  if (const Json* continued_value = GetOptional(value, "continued");
      continued_value != nullptr) {
    if (!continued_value->is_boolean()) {
      return absl::InvalidArgumentError(
          "NodeFragment.continued must be a boolean");
    }
    continued = continued_value->get<bool>();
  }
  NodeFragment result{.id = std::move(id),
                      .data = std::move(data),
                      .seq = seq,
                      .continued = continued};
  ABSL_RETURN_IF_ERROR(result.Validate());
  return result;
}

Json EncodePort(const Port& port) {
  Json value = Json::object();
  if (!port.name.empty()) {
    value["name"] = port.name;
  }
  if (!port.id.empty()) {
    value["id"] = port.id;
  }
  return value;
}

absl::StatusOr<Port> DecodePort(const Json& value) {
  ABSL_ASSIGN_OR_RETURN(std::string name, GetString(value, "name", "", "Port"));
  ABSL_ASSIGN_OR_RETURN(std::string id, GetString(value, "id", "", "Port"));
  Port result{.name = std::move(name), .id = std::move(id)};
  ABSL_RETURN_IF_ERROR(result.Validate());
  return result;
}

Json EncodeAction(const ActionMessage& action) {
  Json value = Json::object({{"id", action.id}, {"name", action.name}});
  if (!action.inputs.empty()) {
    value["inputs"] = Json::array();
    for (const Port& port : action.inputs) {
      value["inputs"].push_back(EncodePort(port));
    }
  }
  if (!action.outputs.empty()) {
    value["outputs"] = Json::array();
    for (const Port& port : action.outputs) {
      value["outputs"].push_back(EncodePort(port));
    }
  }
  if (!action.headers.empty()) {
    value["headers"] = EncodeByteMap(action.headers);
  }
  return value;
}

absl::StatusOr<std::vector<Port>> DecodePorts(const Json* value,
                                              std::string_view context) {
  std::vector<Port> result;
  if (value == nullptr) {
    return result;
  }
  if (!value->is_array()) {
    return absl::InvalidArgumentError(
        absl::StrCat(context, " must be an array"));
  }
  result.reserve(value->size());
  for (const Json& item : *value) {
    ABSL_ASSIGN_OR_RETURN(Port port, DecodePort(item));
    result.push_back(std::move(port));
  }
  return result;
}

absl::StatusOr<ActionMessage> DecodeAction(const Json& value) {
  ABSL_ASSIGN_OR_RETURN(std::string id,
                        GetString(value, "id", "", "ActionMessage"));
  ABSL_ASSIGN_OR_RETURN(std::string name,
                        GetString(value, "name", "", "ActionMessage"));
  ABSL_ASSIGN_OR_RETURN(
      std::vector<Port> inputs,
      DecodePorts(GetOptional(value, "inputs"), "ActionMessage.inputs"));
  ABSL_ASSIGN_OR_RETURN(
      std::vector<Port> outputs,
      DecodePorts(GetOptional(value, "outputs"), "ActionMessage.outputs"));
  ABSL_ASSIGN_OR_RETURN(
      ByteMap headers,
      DecodeByteMap(GetOptional(value, "headers"), "ActionMessage.headers"));
  ActionMessage result{.id = std::move(id),
                       .name = std::move(name),
                       .inputs = std::move(inputs),
                       .outputs = std::move(outputs),
                       .headers = std::move(headers)};
  ABSL_RETURN_IF_ERROR(result.Validate());
  return result;
}

}  // namespace

absl::StatusOr<Json> WireMessageToJsonValue(const WireMessage& message) {
  ABSL_RETURN_IF_ERROR(message.Validate());
  // Encoding cannot fail: every value put here is built from a typed field, and
  // nlohmann only raises on a type mismatch. Byte strings are base64-encoded on
  // the way in (see EncodeChunk), which is what keeps a chunk of arbitrary
  // bytes out of a JSON string field.
  Json value = Json::object();
  if (!message.node_fragments.empty()) {
    value["node_fragments"] = Json::array();
    for (const NodeFragment& fragment : message.node_fragments) {
      value["node_fragments"].push_back(EncodeFragment(fragment));
    }
  }
  if (!message.actions.empty()) {
    value["actions"] = Json::array();
    for (const ActionMessage& action : message.actions) {
      value["actions"].push_back(EncodeAction(action));
    }
  }
  if (!message.headers.empty()) {
    value["headers"] = EncodeByteMap(message.headers);
  }
  return value;
}

absl::StatusOr<std::string> WireMessageToJson(const WireMessage& message) {
  ABSL_ASSIGN_OR_RETURN(Json value, WireMessageToJsonValue(message));
  return DumpJson(value, "WireMessage JSON");
}

absl::StatusOr<WireMessage> WireMessageFromJsonValue(const Json& value) {
  if (!value.is_object()) {
    return absl::InvalidArgumentError(
        "A WireMessage JSON value must be an object");
  }
  WireMessage result;
  if (const Json* fragments = GetOptional(value, "node_fragments");
      fragments != nullptr) {
    if (!fragments->is_array()) {
      return absl::InvalidArgumentError(
          "WireMessage.node_fragments must be an array");
    }
    result.node_fragments.reserve(fragments->size());
    for (const Json& item : *fragments) {
      ABSL_ASSIGN_OR_RETURN(NodeFragment fragment, DecodeFragment(item));
      result.node_fragments.push_back(std::move(fragment));
    }
  }
  if (const Json* actions = GetOptional(value, "actions");
      actions != nullptr) {
    if (!actions->is_array()) {
      return absl::InvalidArgumentError(
          "WireMessage.actions must be an array");
    }
    result.actions.reserve(actions->size());
    for (const Json& item : *actions) {
      ABSL_ASSIGN_OR_RETURN(ActionMessage action, DecodeAction(item));
      result.actions.push_back(std::move(action));
    }
  }
  ABSL_ASSIGN_OR_RETURN(
      ByteMap headers,
      DecodeByteMap(GetOptional(value, "headers"), "WireMessage.headers"));
  result.headers = std::move(headers);
  ABSL_RETURN_IF_ERROR(result.Validate());
  return result;
}

absl::StatusOr<WireMessage> WireMessageFromJson(std::string_view encoded) {
  ABSL_ASSIGN_OR_RETURN(const Json value,
                        ParseJson(encoded, "WireMessage JSON"));
  return WireMessageFromJsonValue(value);
}

}  // namespace a11::data
