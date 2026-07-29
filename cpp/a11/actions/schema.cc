// Copyright 2026 The A11 Authors.

#include "a11/actions/schema.h"

#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>

#include "a11/data/msgpack.h"
#include "a11/data/types.h"

namespace a11::actions {

absl::Status ActionPortSchema::Validate() const {
  ABSL_RETURN_IF_ERROR(data::ValidateName(name));
  if (type.empty()) {
    return absl::InvalidArgumentError("Action port type must not be empty");
  }
  for (const std::optional<data::NodeFragment>& fragment : autofills) {
    if (!fragment) {
      continue;
    }
    ABSL_RETURN_IF_ERROR(fragment->Validate());
  }
  return absl::OkStatus();
}

absl::Status ActionHeaderSchema::Validate() const {
  return data::ValidateName(name);
}

absl::Status ActionSchema::Validate() const {
  absl::Status status = data::ValidateName(name);
  if (!status.ok())
    return status;
  const absl::flat_hash_set<std::string_view> reserved = {
      kActionStatusOutput, kActionDispatchStatusOutput};
  for (const auto* ports : {&inputs, &outputs}) {
    for (const auto& [key, port] : *ports) {
      status = data::ValidateName(key);
      if (!status.ok())
        return status;
      status = port.Validate();
      if (!status.ok())
        return status;
      if (key != port.name) {
        return absl::InvalidArgumentError(
            absl::StrCat("Action port key '", key,
                         "' does not match port name '", port.name, "'"));
      }
      if (reserved.find(key) != reserved.end()) {
        return absl::InvalidArgumentError(
            absl::StrCat("Action port name '", key, "' is reserved"));
      }
    }
  }
  for (const auto& [key, header] : headers) {
    status = data::ValidateName(key);
    if (!status.ok())
      return status;
    status = header.Validate();
    if (!status.ok())
      return status;
    if (key != header.name) {
      return absl::InvalidArgumentError(
          absl::StrCat("Action header key '", key,
                       "' does not match header name '", header.name, "'"));
    }
  }
  for (const auto& [output, field] : output_to_json_field) {
    if (outputs.find(output) == outputs.end()) {
      return absl::NotFoundError(
          absl::StrCat("Output '", output, "' is not in the action schema"));
    }
    if (field != kWholeJson) {
      status = data::ValidateName(field);
      if (!status.ok())
        return status;
    }
  }
  size_t whole_values = 0;
  for (const auto& [unused, field] : output_to_json_field) {
    (void)unused;
    if (field == kWholeJson)
      ++whole_values;
  }
  if (whole_values > 1 ||
      (whole_values == 1 && output_to_json_field.size() != 1)) {
    return absl::FailedPreconditionError(
        "Only one output can map to the complete JSON value");
  }
  return absl::OkStatus();
}

absl::Status ActionSchema::MapOutputToJson(std::string output_name,
                                           std::string field_name) {
  absl::Status status = data::ValidateName(output_name);
  if (!status.ok())
    return status;
  if (outputs.find(output_name) == outputs.end()) {
    return absl::NotFoundError(
        absl::StrCat("Output '", output_name, "' is not in the action schema"));
  }
  if (field_name.empty())
    field_name = output_name;
  if (field_name == kWholeJson) {
    if (!output_to_json_field.empty() &&
        output_to_json_field != absl::flat_hash_map<std::string, std::string>{
                                    {output_name, std::string(kWholeJson)}}) {
      return absl::FailedPreconditionError(
          "Only one output can map to the complete JSON value");
    }
  } else {
    status = data::ValidateName(field_name);
    if (!status.ok())
      return status;
  }
  output_to_json_field.insert_or_assign(std::move(output_name),
                                        std::move(field_name));
  return absl::OkStatus();
}

absl::StatusOr<data::Chunk> StatusToChunk(const absl::Status& status) {
  absl::StatusOr<std::string> bytes = data::PackStatus(status);
  if (!bytes.ok())
    return bytes.status();
  return data::Chunk{
      .metadata =
          data::ChunkMetadata{.mimetype = std::string(kActionStatusMimetype)},
      .data = std::move(*bytes),
  };
}

absl::StatusOr<absl::Status> StatusFromChunk(const data::Chunk& chunk) {
  absl::Status validation = chunk.Validate();
  if (!validation.ok()) {
    absl::StatusOr<absl::Status> result;
    result.AssignStatus(std::move(validation));
    return result;
  }
  if (!IsStatusChunk(chunk)) {
    absl::StatusOr<absl::Status> result;
    result.AssignStatus(
        absl::InvalidArgumentError("Chunk does not contain an Action status"));
    return result;
  }
  return data::UnpackStatus(chunk.data);
}

bool IsStatusChunk(const data::Chunk& chunk) {
  return chunk.GetMimetype() == kActionStatusMimetype;
}

}  // namespace a11::actions
