// Copyright 2026 The A11 Authors.

#include "a11/actions/schema.h"

#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>

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
  ABSL_RETURN_IF_ERROR(data::ValidateName(name));
  const absl::flat_hash_set<std::string_view> reserved = {
      kActionStatusOutput, kActionDispatchStatusOutput};
  for (const auto* ports : {&inputs, &outputs}) {
    for (const auto& [key, port] : *ports) {
      ABSL_RETURN_IF_ERROR(data::ValidateName(key));
      ABSL_RETURN_IF_ERROR(port.Validate());
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
  // A port's node id is derived from the action id and the port name alone --
  // Action::MakeNodeId does not know the direction -- so an input and an output
  // sharing a name are the *same node*, and writing the output would collide
  // with whatever was fed to the input. Rejecting that here turns a puzzling
  // runtime failure into a schema error.
  for (const auto& key : inputs | std::views::keys) {
    if (outputs.contains(key)) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Action port name '", key,
          "' is used for both an input and an output; the two would share one "
          "node"));
    }
  }
  for (const auto& [key, header] : headers) {
    ABSL_RETURN_IF_ERROR(data::ValidateName(key));
    ABSL_RETURN_IF_ERROR(header.Validate());
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
      ABSL_RETURN_IF_ERROR(data::ValidateName(field));
    }
  }
  size_t whole_values = 0;
  for (const auto& [unused, field] : output_to_json_field) {
    (void)unused;
    if (field == kWholeJson) {
      ++whole_values;
    }
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
  ABSL_RETURN_IF_ERROR(data::ValidateName(output_name));
  if (outputs.find(output_name) == outputs.end()) {
    return absl::NotFoundError(
        absl::StrCat("Output '", output_name, "' is not in the action schema"));
  }
  if (field_name.empty()) {
    field_name = output_name;
  }
  if (field_name == kWholeJson) {
    if (!output_to_json_field.empty() &&
        output_to_json_field != absl::flat_hash_map<std::string, std::string>{
                                    {output_name, std::string(kWholeJson)}}) {
      return absl::FailedPreconditionError(
          "Only one output can map to the complete JSON value");
    }
  } else {
    ABSL_RETURN_IF_ERROR(data::ValidateName(field_name));
  }
  output_to_json_field.insert_or_assign(std::move(output_name),
                                        std::move(field_name));
  return absl::OkStatus();
}

absl::StatusOr<data::Chunk> StatusToChunk(const absl::Status& status) {
  return data::MakeStatusChunk(status);
}

absl::StatusOr<absl::Status> StatusFromChunk(const data::Chunk& chunk) {
  absl::Status validation = chunk.Validate();
  if (!validation.ok()) {
    absl::StatusOr<absl::Status> result;
    result.AssignStatus(std::move(validation));
    return result;
  }
  if (!data::IsStatusChunk(chunk)) {
    absl::StatusOr<absl::Status> result;
    result.AssignStatus(
        absl::InvalidArgumentError("Chunk does not contain an Action status"));
    return result;
  }
  return data::StatusFromStatusChunk(chunk);
}

}  // namespace a11::actions
