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
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/escaping.h>
#include <nlohmann/json.hpp>

#include "a11/actions/schema.h"
#include "a11/actions/describe.h"
#include "a11/data/json.h"
#include "a11/json_codec.h"
#include "bridge_core.h"

namespace {
absl::StatusOr<std::string> Text(const nlohmann::json& value,
                                 const char* key, bool required = false) {
  const auto found = value.find(key);
  if (found == value.end()) {
    if (required) return absl::InvalidArgumentError(
        std::string("Missing schema field: ") + key);
    return std::string{};
  }
  if (!found->is_string()) {
    return absl::InvalidArgumentError(
        std::string("Schema field must be a string: ") + key);
  }
  return found->get<std::string>();
}

absl::StatusOr<bool> Flag(const nlohmann::json& value, const char* key) {
  const auto found = value.find(key);
  if (found == value.end()) return false;
  if (!found->is_boolean()) {
    return absl::InvalidArgumentError(
        std::string("Schema field must be a boolean: ") + key);
  }
  return found->get<bool>();
}

absl::StatusOr<a11::actions::ActionSchema> ReadSchema(
    const nlohmann::json& value) {
  if (!value.is_object()) {
    return absl::InvalidArgumentError("Action schema must be an object");
  }
  a11::actions::ActionSchema schema;
  ABSL_ASSIGN_OR_RETURN(schema.name, Text(value, "name", true));
  ABSL_ASSIGN_OR_RETURN(schema.description, Text(value, "description"));
  for (const char* direction : {"inputs", "outputs"}) {
    const auto entries = value.find(direction);
    if (entries == value.end()) continue;
    if (!entries->is_array()) {
      return absl::InvalidArgumentError("Schema ports must be an array");
    }
    auto& ports = std::string_view(direction) == "inputs"
                      ? schema.inputs : schema.outputs;
    for (const auto& entry : *entries) {
      if (!entry.is_object()) {
        return absl::InvalidArgumentError("Schema port must be an object");
      }
      a11::actions::ActionPortSchema port;
      ABSL_ASSIGN_OR_RETURN(port.name, Text(entry, "name", true));
      ABSL_ASSIGN_OR_RETURN(port.type, Text(entry, "type", true));
      ABSL_ASSIGN_OR_RETURN(port.description, Text(entry, "description"));
      ABSL_ASSIGN_OR_RETURN(port.required, Flag(entry, "required"));
      ABSL_ASSIGN_OR_RETURN(port.unary, Flag(entry, "unary"));
      if (const auto json_schema = entry.find("json_schema");
          json_schema != entry.end() && !json_schema->is_null()) {
        ABSL_ASSIGN_OR_RETURN(port.json_schema,
                              a11::DumpJson(*json_schema, "port JSON Schema"));
      }
      if (const auto autofills = entry.find("autofills");
          autofills != entry.end()) {
        if (!autofills->is_array()) {
          return absl::InvalidArgumentError("input autofills must be an array");
        }
        for (const auto& fragment : *autofills) {
          if (fragment.is_null()) {
            port.autofills.emplace_back(std::nullopt);
          } else {
            ABSL_ASSIGN_OR_RETURN(
                auto decoded,
                a11::data::NodeFragmentFromJsonValue(fragment));
            port.autofills.emplace_back(std::move(decoded));
          }
        }
      }
      if (!ports.emplace(port.name, std::move(port)).second) {
        return absl::InvalidArgumentError("Duplicate schema port");
      }
    }
  }
  if (const auto entries = value.find("headers"); entries != value.end()) {
    if (!entries->is_array()) {
      return absl::InvalidArgumentError("Schema headers must be an array");
    }
    for (const auto& entry : *entries) {
      if (!entry.is_object()) {
        return absl::InvalidArgumentError("Schema header must be an object");
      }
      a11::actions::ActionHeaderSchema header;
      ABSL_ASSIGN_OR_RETURN(header.name, Text(entry, "name", true));
      ABSL_ASSIGN_OR_RETURN(header.description, Text(entry, "description"));
      if (const auto encoded = entry.find("default_base64");
          encoded != entry.end()) {
        if (!encoded->is_string()) {
          return absl::InvalidArgumentError(
              "header default_base64 must be a string");
        }
        std::string decoded;
        if (!absl::Base64Unescape(encoded->get<std::string>(), &decoded)) {
          return absl::InvalidArgumentError(
              "header default_base64 is invalid");
        }
        header.default_value = std::move(decoded);
      }
      if (!schema.headers.emplace(header.name, std::move(header)).second) {
        return absl::InvalidArgumentError("Duplicate schema header");
      }
    }
  }
  if (const auto mapping = value.find("output_to_json_field");
      mapping != value.end()) {
    if (!mapping->is_object()) {
      return absl::InvalidArgumentError("Output mapping must be an object");
    }
    for (const auto& [output, field] : mapping->items()) {
      if (!field.is_string()) {
        return absl::InvalidArgumentError("Output mapping must be a string");
      }
      ABSL_RETURN_IF_ERROR(schema.MapOutputToJson(output,
                                                  field.get<std::string>()));
    }
  }
  ABSL_RETURN_IF_ERROR(schema.Validate());
  return schema;
}
}  // namespace

extern "C" {
Result* a11_wasm_schema_validate(const std::uint8_t* data, size_t size) {
  auto value = a11::ParseJson(View(data, size), "action schema");
  if (!value.ok()) return Failure(value.status());
  auto schema = ReadSchema(*value);
  return schema.ok() ? new Result{0, {}} : Failure(schema.status());
}

Result* a11_wasm_schema_describe(const std::uint8_t* data, size_t size,
                                  int runnable, int all_ports) {
  auto value = a11::ParseJson(View(data, size), "action schema");
  if (!value.ok()) return Failure(value.status());
  auto schema = ReadSchema(*value);
  if (!schema.ok()) return Failure(schema.status());
  auto described = a11::actions::SchemaToJson(
      *schema, runnable != 0,
      all_ports != 0 ? a11::actions::PortView::kAll
                     : a11::actions::PortView::kCallable);
  auto encoded = a11::DumpJson(described, "action descriptor");
  return encoded.ok() ? new Result{0, std::move(*encoded)}
                      : Failure(encoded.status());
}

Result* a11_wasm_schema_parse(const std::uint8_t* data, size_t size) {
  auto value = a11::ParseJson(View(data, size), "action descriptor");
  if (!value.ok()) return Failure(value.status());
  auto schema = a11::actions::SchemaFromJson(*value);
  if (!schema.ok()) return Failure(schema.status());
  auto normalized = a11::actions::SchemaToJson(
      *schema, false, a11::actions::PortView::kAll);
  auto encoded = a11::DumpJson(normalized, "action descriptor");
  return encoded.ok() ? new Result{0, std::move(*encoded)}
                      : Failure(encoded.status());
}

Result* a11_wasm_schema_entries(const std::uint8_t* data, size_t size) {
  auto value = a11::ParseJson(View(data, size), "action descriptor");
  if (!value.ok()) return Failure(value.status());
  auto entries = a11::actions::SchemasInDocument(*value);
  if (!entries.ok()) return Failure(entries.status());
  auto encoded = a11::DumpJson(*entries, "action descriptor");
  return encoded.ok() ? new Result{0, std::move(*encoded)}
                      : Failure(encoded.status());
}
}  // extern "C"
