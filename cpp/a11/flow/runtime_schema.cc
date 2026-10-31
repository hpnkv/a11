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

#include "a11/flow/internal/runtime_common.h"

namespace a11::flow {

namespace {

/// What a port's declared type is called in an [actions::ActionSchema].
///
/// A schema's `type` is the host-visible payload name. Built-in types use their
/// Python names so generated Flow schemas match hand-written action schemas.
/// Tags and mimetypes are preserved as written.
std::string SchemaType(std::string_view declared) {
  static const auto* const kNames =
      new absl::flat_hash_map<std::string_view, std::string_view>{
          {"string", "str"},
          {"text", "str"},
          {"number", "float"},
          {"integer", "int"},
          {"int", "int"},
          {"bool", "bool"},
          {"boolean", "bool"},
          {"object", "dict"},
          {"json", "dict"},
          {"list", "list"},
          {"array", "list"},
          {"bytes", "bytes"},
          {"duration", "a11.Duration"},
          {"time", "a11.Time"},
          {"status", "a11.Status"},
          {"any", "application/json"},
  };
  const auto found = kNames->find(declared);
  return std::string(found == kNames->end() ? declared : found->second);
}

}  // namespace

absl::StatusOr<actions::ActionSchema> FlowSchema(const FlowPlan& plan,
                                                 const Program* shapes) {
  actions::ActionSchema schema;
  schema.name = plan.binding_name.empty() ? plan.name : plan.binding_name;
  schema.name = syntax::RuntimeActionName(schema.name);
  schema.description = plan.description;
  for (const PortPlan& port : plan.ports) {
    actions::ActionPortSchema entry;
    entry.name = port.name;
    entry.type = SchemaType(port.type);
    entry.description = port.description;
    entry.required = port.required;
    entry.unary = port.unary;
    if (shapes != nullptr) {
      if (const auto* shape = shapes->Dto(port.type); shape != nullptr) {
        entry.json_schema = DtoToJsonSchema(*shape, *shapes).dump();
      }
    }
    if (port.direction == syntax::PortDirection::kInput) {
      schema.inputs.emplace(port.name, std::move(entry));
    } else {
      schema.outputs.emplace(port.name, std::move(entry));
    }
  }
  for (const HeaderPlan& header : plan.headers) {
    actions::ActionHeaderSchema entry;
    entry.name = header.name;
    entry.description = header.description;
    if (header.has_default) {
      entry.default_value = HeaderBytes(header.default_value);
    }
    schema.headers.emplace(header.name, std::move(entry));
  }
  ABSL_RETURN_IF_ERROR(schema.Validate());
  return schema;
}

ActionDeclaration DeclareAction(const actions::ActionSchema& schema) {
  ActionDeclaration result;
  result.interface.name = schema.name;
  result.interface.description = schema.description;
  result.interface.interface_only = true;
  const auto reject = [&](std::string message) {
    Diagnostic diagnostic;
    diagnostic.code = "flow.schema.unsupported-keyword";
    diagnostic.family = Family::kForm;
    diagnostic.severity = Severity::kError;
    diagnostic.message = std::move(message);
    result.diagnostics.push_back(std::move(diagnostic));
  };
  if (const auto status = schema.Validate(); !status.ok()) {
    reject(std::string(status.message()));
    return result;
  }
  if (!utf8::IsValid(schema.name) || !utf8::IsValid(schema.description)) {
    reject(
        "An action declaration requires valid UTF-8 names and descriptions.");
    return result;
  }
  for (const auto* ports : {&schema.inputs, &schema.outputs}) {
    for (const auto& [name, port] : *ports) {
      if (!utf8::IsValid(name) || !utf8::IsValid(port.type) ||
          !utf8::IsValid(port.description)) {
        reject("Action port metadata requires valid UTF-8 text.");
        return result;
      }
    }
  }
  for (const auto& [name, header] : schema.headers) {
    if (!utf8::IsValid(name) || !utf8::IsValid(header.description) ||
        (header.default_value.has_value() &&
         !utf8::IsValid(*header.default_value))) {
      reject("A header has no lossless UTF-8 Flow spelling.");
      return result;
    }
  }
  const auto encode = [](std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string name;
    for (const char character : value) {
      const auto byte = static_cast<unsigned char>(character);
      name += hex[byte >> 4];
      name += hex[byte & 15];
    }
    return name;
  };

  for (const auto direction :
       {syntax::PortDirection::kInput, syntax::PortDirection::kOutput}) {
    const auto& ports = direction == syntax::PortDirection::kInput
                            ? schema.inputs
                            : schema.outputs;
    std::vector<std::string> names;
    for (const auto& [name, port] : ports) {
      names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
      const auto& port = ports.at(name);
      PortPlan entry;
      entry.name = name;
      entry.direction = direction;
      entry.unary = port.unary;
      entry.required = port.required;
      entry.description = port.description;
      entry.type = port.type;
      entry.declared = port.type;
      if (port.type == "str") {
        entry.declared = "string";
      } else if (port.type == "dict") {
        entry.declared = "object";
      } else if (port.type == "float") {
        entry.declared = "number";
      } else if (port.type == "a11.Time") {
        entry.declared = "time";
      } else if (port.type == "a11.Duration") {
        entry.declared = "duration";
      } else if (port.type.find('/') != std::string::npos) {
        entry.declared = nlohmann::json(port.type).dump();
      }
      if (!port.json_schema.empty()) {
        const auto metadata =
            nlohmann::json::parse(port.json_schema, nullptr, false);
        if (metadata.is_object() && metadata.contains("type") &&
            metadata["type"].is_string() && metadata["type"] == "object" &&
            metadata.contains("properties")) {
          const auto representable = [&](const auto& self,
                                         const nlohmann::json& value,
                                         bool record, int depth) -> bool {
            if (depth > 128 || !value.is_object()) {
              return false;
            }
            for (const auto& item : value.items()) {
              const auto& key = item.key();
              if (key == "title" || key == "description" || key == "$schema" ||
                  key == "x-a11-type" || key == "x-a11-order") {
                continue;
              }
              if (key == "type") {
                if (!item.value().is_string()) {
                  return false;
                }
              } else if (record && key == "properties") {
                if (!item.value().is_object()) {
                  return false;
                }
                for (const auto& field : item.value()) {
                  if (!self(self, field, false, depth + 1)) {
                    return false;
                  }
                }
              } else if (record && key == "$defs") {
                if (!item.value().is_object()) {
                  return false;
                }
                for (const auto& definition : item.value()) {
                  if (!self(self, definition, true, depth + 1)) {
                    return false;
                  }
                }
              } else if (record && key == "required") {
                if (!item.value().is_array()) {
                  return false;
                }
                for (const auto& name : item.value()) {
                  if (!name.is_string() || !value.contains("properties") ||
                      !value["properties"].contains(name.get<std::string>())) {
                    return false;
                  }
                }
              } else if (key == "additionalProperties") {
                if (!item.value().is_boolean()) {
                  return false;
                }
              } else if (!record && key == "items") {
                if (!item.value().is_object()) {
                  return false;
                }
                for (const auto& item_key : item.value().items()) {
                  if (item_key.key() != "type" && item_key.key() != "$ref" &&
                      item_key.key() != "description" &&
                      item_key.key() != "title") {
                    return false;
                  }
                }
                const auto type = item.value().find("type");
                if (type != item.value().end() &&
                    (!type->is_string() || *type == "array")) {
                  return false;
                }
              } else if (!record && (key == "enum" || key == "const")) {
                if (item.value().is_null()) {
                  return false;
                }
                if (key == "enum") {
                  if (!item.value().is_array() || item.value().empty()) {
                    return false;
                  }
                  for (const auto& choice : item.value()) {
                    if (choice.is_null()) {
                      return false;
                    }
                  }
                }
              } else if (!record &&
                         (key == "$ref" || key == "default" ||
                          key == "minimum" || key == "maximum" ||
                          key == "minLength" || key == "maxLength" ||
                          key == "minItems" || key == "maxItems" ||
                          key == "uniqueItems" || key == "pattern")) {
                continue;
              } else if (!record && key == "format") {
                if (item.value() != "date-time" && item.value() != "duration") {
                  return false;
                }
              } else if (!record && key == "contentEncoding") {
                if (item.value() != "base64") {
                  return false;
                }
              } else {
                return false;
              }
            }
            return true;
          };
          if (!representable(representable, metadata, true, 0)) {
            reject(
                "Object metadata contains a constraint without a lossless "
                "Flow declaration.");
            continue;
          }
          const auto prefix = absl::StrCat(
              "Action_", encode(schema.name), "_",
              direction == syntax::PortDirection::kInput ? "in_" : "out_",
              encode(name), "_");
          auto imported = JsonSchemaToDtos(metadata, "Payload");
          absl::flat_hash_map<std::string, std::string> renamed;
          for (const auto& dto : imported.dtos) {
            renamed.emplace(dto.name, prefix + encode(dto.name));
          }
          const auto rename_type = [&](std::string value) {
            const size_t bracket = value.find('[');
            if (bracket != std::string::npos && value.back() == ']') {
              const auto element =
                  value.substr(bracket + 1, value.size() - bracket - 2);
              if (const auto found = renamed.find(element);
                  found != renamed.end()) {
                value.replace(bracket + 1, element.size(), found->second);
              }
            } else if (const auto found = renamed.find(value);
                       found != renamed.end()) {
              value = found->second;
            }
            return value;
          };
          for (auto& dto : imported.dtos) {
            dto.name = renamed.at(dto.name);
            for (auto& field : dto.fields) {
              field.declared = rename_type(field.declared);
              field.type = rename_type(field.type);
              field.dto_name = rename_type(field.dto_name);
              field.element = rename_type(field.element);
              field.element_dto_name = rename_type(field.element_dto_name);
              field.syntax_type.reset();
            }
          }
          for (auto& diagnostic : imported.diagnostics) {
            diagnostic.severity = Severity::kError;
            result.diagnostics.push_back(std::move(diagnostic));
          }
          if (imported.diagnostics.empty() && !imported.dtos.empty()) {
            entry.declared = imported.dtos.front().name;
            for (auto& dto : imported.dtos) {
              result.types.push_back(std::move(dto));
            }
          }
        } else if (metadata.is_object() && metadata.contains("type") &&
                   metadata["type"] == "array") {
          const auto array_type =
              [&](const auto& self, const nlohmann::json& value,
                  int depth) -> absl::StatusOr<std::string> {
            if (depth > 128 || !value.is_object() || !value.contains("type") ||
                !value["type"].is_string()) {
              return absl::InvalidArgumentError(
                  "An array item has no representable Flow type.");
            }
            for (const auto& item : value.items()) {
              if (item.key() != "type" && item.key() != "items" &&
                  item.key() != "description" && item.key() != "title" &&
                  item.key() != "$schema") {
                return absl::InvalidArgumentError(
                    "Array metadata contains a constraint without a Flow port "
                    "spelling.");
              }
            }
            const auto type = value["type"].get<std::string>();
            if (type == "array") {
              if (!value.contains("items")) {
                return std::string("list");
              }
              ABSL_ASSIGN_OR_RETURN(auto element,
                                    self(self, value["items"], depth + 1));
              return absl::StrCat("list[", element, "]");
            }
            if (!vocabulary::TypeNames().contains(type)) {
              return absl::InvalidArgumentError(
                  "An array item type has no Flow spelling.");
            }
            return type;
          };
          const auto type = array_type(array_type, metadata, 0);
          if (type.ok()) {
            entry.declared = *type;
          } else {
            reject(std::string(type.status().message()));
          }
        } else {
          bool representable = metadata.is_object();
          if (representable) {
            for (const auto& item : metadata.items()) {
              if (item.key() != "$schema" && item.key() != "title" &&
                  item.key() != "description" && item.key() != "type") {
                representable = false;
              }
            }
            const auto declared_type = metadata.find("type");
            if (declared_type != metadata.end()) {
              Program evidence;
              if (!declared_type->is_string() ||
                  RefinesType(port.type, declared_type->get<std::string>(),
                              evidence) != Refinement::kYes ||
                  RefinesType(declared_type->get<std::string>(), port.type,
                              evidence) != Refinement::kYes) {
                representable = false;
              }
            }
          }
          if (!representable) {
            Diagnostic diagnostic;
            diagnostic.code = "flow.schema.unsupported-keyword";
            diagnostic.message =
                "This action port's metadata has no lossless Flow declaration.";
            result.diagnostics.push_back(std::move(diagnostic));
          }
        }
      }
      result.interface.ports.push_back(std::move(entry));
    }
  }
  std::vector<std::string> names;
  for (const auto& [name, header] : schema.headers) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  for (const auto& name : names) {
    const auto& header = schema.headers.at(name);
    HeaderPlan entry;
    entry.name = name;
    entry.alias = absl::StrCat("header", result.interface.headers.size());
    entry.description = header.description;
    entry.has_default = header.default_value.has_value();
    if (entry.has_default) {
      entry.default_value = syntax::Constant::String(*header.default_value);
    }
    result.interface.headers.push_back(std::move(entry));
  }
  return result;
}

}  // namespace a11::flow
