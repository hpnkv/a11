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

#include "a11/actions/describe.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <nlohmann/json.hpp>

#include "a11/actions/schema.h"
#include "a11/json_codec.h"

namespace a11::actions {
namespace {
std::string Text(const nlohmann::json& value, std::string_view key) {
  const auto found = value.find(key);
  if (found == value.end() || !found->is_string()) {
    return {};
  }
  return found->get<std::string>();
}

/// A boolean field, or @p fallback where the key is absent or not a boolean.
bool Flag(const nlohmann::json& value, std::string_view key, bool fallback) {
  const auto found = value.find(key);
  if (found == value.end() || !found->is_boolean()) {
    return fallback;
  }
  return found->get<bool>();
}

/// The strings in an array field, skipping anything that is not one.
std::vector<const ActionPortSchema*> SortedPorts(
    const absl::flat_hash_map<std::string, ActionPortSchema>& ports) {
  std::vector<const ActionPortSchema*> sorted;
  sorted.reserve(ports.size());
  for (const auto& [unused, port] : ports) {
    (void)unused;
    sorted.push_back(&port);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const ActionPortSchema* left, const ActionPortSchema* right) {
              return left->name < right->name;
            });
  return sorted;
}

std::vector<const ActionHeaderSchema*> SortedHeaders(
    const absl::flat_hash_map<std::string, ActionHeaderSchema>& headers) {
  std::vector<const ActionHeaderSchema*> sorted;
  sorted.reserve(headers.size());
  for (const auto& [unused, header] : headers) {
    (void)unused;
    sorted.push_back(&header);
  }
  std::sort(
      sorted.begin(), sorted.end(),
      [](const ActionHeaderSchema* left, const ActionHeaderSchema* right) {
        return left->name < right->name;
      });
  return sorted;
}

/// Returns whether a JSON Schema contains no constraints.
///
/// What a port with no stated type gets shown as anyway: an adapter offering a
/// port with no `json_schema` to a model gives it `{"type": "object"}`, so a
/// document carrying that spelled out says exactly what an absent field does,
/// on every port whose type is a bare mapping.
bool SaysNothing(const nlohmann::json& schema) {
  if (!schema.is_object() || schema.size() != 1) {
    return false;
  }
  const auto type = schema.find("type");
  return type != schema.end() && *type == "object";
}

/// A port's `json_schema`, parsed if it parses.
///
/// Carried as a JSON value rather than as the string it is stored as, so a
/// consumer building a tool definition can splice it in rather than re-parse
/// it. Text that is not JSON is dropped: a schema nobody can read is worse
/// than an absent one, which at least reads as "no type information".
void AttachJsonSchema(nlohmann::json* target, const std::string& encoded) {
  if (encoded.empty()) {
    return;
  }
  absl::StatusOr<nlohmann::json> parsed =
      a11::ParseJson(encoded, "port JSON Schema");
  if (!parsed.ok() || SaysNothing(*parsed)) {
    return;
  }
  (*target)["json_schema"] = *std::move(parsed);
}

nlohmann::json PortToJson(const ActionPortSchema& port, bool autofilled) {
  nlohmann::json value{{"name", port.name}, {"type", port.type}};
  if (!port.description.empty()) {
    value["description"] = port.description;
  }
  // Both flags are omitted when false, which is what ActionPortSchema itself
  // defaults them to and so what a reader of this document fills in. See the
  // warning on describe.h for the one place that default is not shared.
  if (port.required) {
    value["required"] = true;
  }
  if (port.unary) {
    value["unary"] = true;
  }
  if (autofilled) {
    value["autofilled"] = true;
  }
  AttachJsonSchema(&value, port.json_schema);
  return value;
}

ActionPortSchema PortFromJson(const nlohmann::json& value) {
  ActionPortSchema port;
  port.name = Text(value, "name");
  port.type = Text(value, "type");
  port.description = Text(value, "description");
  port.required = Flag(value, "required", false);
  // False, which is what the writer omits the field for and what
  // ActionPortSchema itself defaults to: a streaming port is the absent case on
  // both sides of this codec.
  port.unary = Flag(value, "unary", false);
  // A `user_facing` flag from an older client is read and dropped. Narration
  // travels on the reserved log port, which no schema declares, so a port
  // saying it was narration has nothing left to mean.
  const auto schema = value.find("json_schema");
  if (schema != value.end() && !schema->is_null()) {
    port.json_schema = a11::DumpJsonLossy(*schema);
  }
  return port;
}

nlohmann::json HeaderToJson(const ActionHeaderSchema& header) {
  nlohmann::json value{{"name", header.name}};
  if (!header.description.empty()) {
    value["description"] = header.description;
  }
  if (header.default_value.has_value()) {
    value["has_default"] = true;
    // The value itself only where it is text. A header default can be arbitrary
    // bytes, and base64 in a document whose whole purpose is to be read by a
    // person is a worse answer than saying a default exists.
    if (a11::IsValidUtf8(*header.default_value)) {
      value["default"] = *header.default_value;
    }
  }
  return value;
}

ActionHeaderSchema HeaderFromJson(const nlohmann::json& value) {
  ActionHeaderSchema header;
  header.name = Text(value, "name");
  header.description = Text(value, "description");
  const auto found = value.find("default");
  if (found != value.end() && found->is_string()) {
    header.default_value = found->get<std::string>();
  }
  return header;
}

}  // namespace

nlohmann::json SchemaToJson(const ActionSchema& schema, bool runnable,
                            PortView ports) {
  nlohmann::json entry{{"name", schema.name}};
  if (!schema.description.empty()) {
    entry["description"] = schema.description;
  }
  entry["runnable"] = runnable;

  nlohmann::json inputs = nlohmann::json::array();
  for (const ActionPortSchema* port : SortedPorts(schema.inputs)) {
    const bool autofilled = !port->autofills.empty();
    // A caller cannot write an autofilled input: the runtime requires it empty
    // before applying the receiver's default, so offering it invites a failure.
    if (autofilled && ports == PortView::kCallable) {
      continue;
    }
    inputs.push_back(PortToJson(*port, autofilled));
  }
  nlohmann::json outputs = nlohmann::json::array();
  for (const ActionPortSchema* port : SortedPorts(schema.outputs)) {
    outputs.push_back(PortToJson(*port, false));
  }
  if (!inputs.empty()) {
    entry["inputs"] = std::move(inputs);
  }
  if (!outputs.empty()) {
    entry["outputs"] = std::move(outputs);
  }

  nlohmann::json headers = nlohmann::json::array();
  for (const ActionHeaderSchema* header : SortedHeaders(schema.headers)) {
    headers.push_back(HeaderToJson(*header));
  }
  if (!headers.empty()) {
    entry["headers"] = std::move(headers);
  }

  if (!schema.output_to_json_field.empty()) {
    nlohmann::json mapping = nlohmann::json::object();
    // Sorted for the same reason the ports are.
    std::vector<std::pair<std::string, std::string>> pairs(
        schema.output_to_json_field.begin(), schema.output_to_json_field.end());
    std::sort(pairs.begin(), pairs.end());
    for (const auto& [output, field] : pairs) {
      mapping[output] = field;
    }
    entry["output_to_json_field"] = std::move(mapping);
  }
  return entry;
}

absl::StatusOr<std::string> SchemaToJsonText(const ActionSchema& schema,
                                             bool runnable, PortView ports) {
  nlohmann::json actions = nlohmann::json::array();
  actions.push_back(SchemaToJson(schema, runnable, ports));
  return a11::DumpJson(
      nlohmann::json{{"format", std::string(kSchemaDocumentFormat)},
                     {"actions", std::move(actions)}},
      "action descriptor");
}

absl::StatusOr<ActionSchema> SchemaFromJson(const nlohmann::json& entry) {
  if (!entry.is_object()) {
    return absl::InvalidArgumentError("A entry action must be an object");
  }
  ActionSchema schema;
  schema.name = Text(entry, "name");
  if (schema.name.empty()) {
    return absl::InvalidArgumentError("A entry action must have a name");
  }
  schema.description = Text(entry, "description");

  const auto add_ports =
      [&entry](std::string_view key,
               absl::flat_hash_map<std::string, ActionPortSchema>* into) {
        const auto found = entry.find(key);
        if (found == entry.end() || !found->is_array()) {
          return;
        }
        for (const nlohmann::json& one : *found) {
          if (!one.is_object()) {
            continue;
          }
          ActionPortSchema port = PortFromJson(one);
          if (port.name.empty()) {
            continue;
          }
          into->insert_or_assign(port.name, std::move(port));
        }
      };
  add_ports("inputs", &schema.inputs);
  add_ports("outputs", &schema.outputs);

  const auto headers = entry.find("headers");
  if (headers != entry.end() && headers->is_array()) {
    for (const nlohmann::json& one : *headers) {
      if (!one.is_object()) {
        continue;
      }
      ActionHeaderSchema header = HeaderFromJson(one);
      if (header.name.empty()) {
        continue;
      }
      schema.headers.insert_or_assign(header.name, std::move(header));
    }
  }

  const auto mapping = entry.find("output_to_json_field");
  if (mapping != entry.end() && mapping->is_object()) {
    for (const auto& [output, field] : mapping->items()) {
      if (!field.is_string()) {
        continue;
      }
      if (!schema.outputs.contains(output)) {
        continue;
      }
      ABSL_RETURN_IF_ERROR(
          schema.MapOutputToJson(output, field.get<std::string>()));
    }
  }

  ABSL_RETURN_IF_ERROR(schema.Validate());
  return schema;
}

absl::StatusOr<ActionSchema> SchemaFromJsonText(std::string_view encoded) {
  ABSL_ASSIGN_OR_RETURN(const nlohmann::json value,
                        a11::ParseJson(encoded, "action descriptor"));
  return SchemaFromJson(value);
}

absl::StatusOr<std::vector<nlohmann::json>> SchemasInDocument(
    const nlohmann::json& envelope) {
  std::vector<nlohmann::json> entry;
  if (envelope.is_array()) {
    for (const nlohmann::json& one : envelope) {
      entry.push_back(one);
    }
    return entry;
  }
  if (!envelope.is_object()) {
    return absl::InvalidArgumentError(
        "Action descriptors must be an object or an array");
  }
  const auto actions = envelope.find("actions");
  if (actions == envelope.end() || !actions->is_array()) {
    return absl::InvalidArgumentError(
        "Action descriptors are missing their 'actions' array");
  }
  for (const nlohmann::json& one : *actions) {
    entry.push_back(one);
  }
  return entry;
}

}  // namespace a11::actions
