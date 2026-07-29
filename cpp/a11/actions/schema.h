// Copyright 2026 The A11 Authors.

#ifndef A11_ACTIONS_SCHEMA_H_
#define A11_ACTIONS_SCHEMA_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/data/types.h"

namespace a11::actions {

inline constexpr std::string_view kActionStatusMimetype =
    "application/x-a11-status";
inline constexpr std::string_view kActionStatusOutput = "__status__";
inline constexpr std::string_view kActionDispatchStatusOutput =
    "__dispatch_status__";
inline constexpr std::string_view kCancelActionName = "__cancel__";
inline constexpr std::string_view kCancelActionHeader = "__action";
inline constexpr std::string_view kActionHeaderPrefix = "x-a11-";

struct ActionPortSchema {
  std::string name;
  std::string type;
  std::string description;
  bool required = false;
  bool unary = false;
  std::vector<std::optional<data::NodeFragment>> autofills;
  
  void* typeinfo = nullptr;

  absl::Status Validate() const;
  friend bool operator==(const ActionPortSchema&,
                         const ActionPortSchema&) = default;
};

struct ActionHeaderSchema {
  std::string name;
  std::string description;
  std::optional<data::Bytes> default_value;

  absl::Status Validate() const;
  friend bool operator==(const ActionHeaderSchema&,
                         const ActionHeaderSchema&) = default;
};

struct ActionSchema {
  static constexpr std::string_view kWholeJson = "$";

  std::string name;
  std::string description;
  absl::flat_hash_map<std::string, ActionPortSchema> inputs;
  absl::flat_hash_map<std::string, ActionPortSchema> outputs;
  absl::flat_hash_map<std::string, ActionHeaderSchema> headers;
  absl::flat_hash_map<std::string, std::string> output_to_json_field;

  absl::Status Validate() const;
  absl::Status MapOutputToJson(std::string output_name,
                               std::string field_name = {});

  friend bool operator==(const ActionSchema&, const ActionSchema&) = default;
};

struct ActionSettings {
  std::optional<bool> bind_streams_on_inputs_by_default;
  std::optional<bool> bind_streams_on_outputs_by_default;
  bool clear_inputs_after_run = false;
  bool clear_outputs_after_run = false;

  friend bool operator==(const ActionSettings&,
                         const ActionSettings&) = default;
};

absl::StatusOr<data::Chunk> StatusToChunk(const absl::Status& status);
absl::StatusOr<absl::Status> StatusFromChunk(const data::Chunk& chunk);
bool IsStatusChunk(const data::Chunk& chunk);

}  // namespace a11::actions

#endif  // A11_ACTIONS_SCHEMA_H_
