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
#include "a11/json_codec.h"
#include "jvm/bridge.h"

namespace a11::jvm {
namespace {
template <typename T, typename Decode>
absl::StatusOr<absl::flat_hash_map<std::string, T>> ReadMap(
    const nlohmann::json& fields, std::string_view name, Decode decode) {
  ABSL_ASSIGN_OR_RETURN(
      auto source,
      DefaultArg<nlohmann::json>(fields, name, nlohmann::json::object()));
  if (!source.is_object()) {
    return absl::InvalidArgumentError("Expected field map");
  }
  absl::flat_hash_map<std::string, T> result;
  for (const auto& [key, item] : source.items()) {
    ABSL_ASSIGN_OR_RETURN(auto value, decode(item));
    result.emplace(key, std::move(value));
  }
  return result;
}

absl::StatusOr<actions::ActionPortSchema> ReadPort(
    const nlohmann::json& fields) {
  actions::ActionPortSchema value;
  ABSL_ASSIGN_OR_RETURN(value.name, Arg<std::string>(fields, "name"));
  ABSL_ASSIGN_OR_RETURN(value.type, Arg<std::string>(fields, "type"));
  ABSL_ASSIGN_OR_RETURN(value.description,
                        DefaultArg<std::string>(fields, "description", ""));
  ABSL_ASSIGN_OR_RETURN(value.required,
                        DefaultArg<bool>(fields, "required", value.required));
  ABSL_ASSIGN_OR_RETURN(value.unary,
                        DefaultArg<bool>(fields, "unary", value.unary));
  ABSL_ASSIGN_OR_RETURN(value.json_schema,
                        DefaultArg<std::string>(fields, "json_schema", ""));
  ABSL_ASSIGN_OR_RETURN(
      auto defaults,
      DefaultArg<nlohmann::json>(fields, "autofills", nlohmann::json::array()));
  if (!defaults.is_array()) {
    return absl::InvalidArgumentError("Expected autofill array");
  }
  for (const auto& item : defaults) {
    if (item.is_null()) {
      value.autofills.emplace_back(std::nullopt);
    } else {
      ABSL_ASSIGN_OR_RETURN(auto fragment,
                            data::NodeFragmentFromJsonValue(item));
      value.autofills.emplace_back(std::move(fragment));
    }
  }
  return value;
}

absl::StatusOr<actions::ActionHeaderSchema> ReadHeader(
    const nlohmann::json& fields) {
  actions::ActionHeaderSchema value;
  ABSL_ASSIGN_OR_RETURN(value.name, Arg<std::string>(fields, "name"));
  ABSL_ASSIGN_OR_RETURN(value.description,
                        DefaultArg<std::string>(fields, "description", ""));
  const auto found = fields.find("default_value");
  if (found != fields.end() && !found->is_null()) {
    ABSL_ASSIGN_OR_RETURN(value.default_value,
                          Arg<std::string>(fields, "default_value"));
  }
  return value;
}

absl::StatusOr<nlohmann::json> PortFields(
    const actions::ActionPortSchema& port) {
  nlohmann::json defaults = nlohmann::json::array();
  for (const auto& fragment : port.autofills) {
    if (!fragment.has_value()) {
      defaults.push_back(nullptr);
      continue;
    }
    ABSL_ASSIGN_OR_RETURN(auto value, data::NodeFragmentToJsonValue(*fragment));
    defaults.push_back(std::move(value));
  }
  return nlohmann::json{{"name", port.name},
                        {"type", port.type},
                        {"description", port.description},
                        {"required", port.required},
                        {"unary", port.unary},
                        {"autofills", std::move(defaults)},
                        {"json_schema", port.json_schema}};
}

nlohmann::json HeaderFields(const actions::ActionHeaderSchema& header) {
  return nlohmann::json{{"name", header.name},
                        {"description", header.description},
                        {"default_value", header.default_value.has_value()
                                              ? Binary(*header.default_value)
                                              : Null()}};
}
}  // namespace

absl::StatusOr<actions::ActionSchema> ReadSchema(const nlohmann::json& fields) {
  actions::ActionSchema value;
  ABSL_ASSIGN_OR_RETURN(value.name, Arg<std::string>(fields, "name"));
  ABSL_ASSIGN_OR_RETURN(value.description,
                        DefaultArg<std::string>(fields, "description", ""));
  ABSL_ASSIGN_OR_RETURN(value.inputs, ReadMap<actions::ActionPortSchema>(
                                          fields, "inputs", ReadPort));
  ABSL_ASSIGN_OR_RETURN(value.outputs, ReadMap<actions::ActionPortSchema>(
                                           fields, "outputs", ReadPort));
  ABSL_ASSIGN_OR_RETURN(value.headers, ReadMap<actions::ActionHeaderSchema>(
                                           fields, "headers", ReadHeader));
  ABSL_ASSIGN_OR_RETURN(
      auto mapping, DefaultArg<nlohmann::json>(fields, "output_to_json_field",
                                               nlohmann::json::object()));
  if (!mapping.is_object()) {
    return absl::InvalidArgumentError("Expected output field mapping");
  }
  for (const auto& [key, item] : mapping.items()) {
    ABSL_ASSIGN_OR_RETURN(auto target, Arg<std::string>(mapping, key));
    value.output_to_json_field.emplace(key, std::move(target));
  }
  return value;
}

absl::StatusOr<nlohmann::json> SchemaFields(
    const actions::ActionSchema& schema) {
  nlohmann::json fields{{"name", schema.name},
                        {"description", schema.description},
                        {"inputs", nlohmann::json::object()},
                        {"outputs", nlohmann::json::object()},
                        {"headers", nlohmann::json::object()},
                        {"output_to_json_field", schema.output_to_json_field}};
  for (const auto& [key, port] : schema.inputs) {
    ABSL_ASSIGN_OR_RETURN(fields["inputs"][key], PortFields(port));
  }
  for (const auto& [key, port] : schema.outputs) {
    ABSL_ASSIGN_OR_RETURN(fields["outputs"][key], PortFields(port));
  }
  for (const auto& [key, header] : schema.headers) {
    fields["headers"][key] = HeaderFields(header);
  }
  return fields;
}

void BindSchemas(absl::flat_hash_map<std::string, Operation>* operations) {
  (*operations)["schema.validatePort"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto fields, Arg<nlohmann::json>(args, "fields"));
    ABSL_ASSIGN_OR_RETURN(auto value, ReadPort(fields));
    return Result(value.Validate());
  };
  (*operations)["schema.validateHeader"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto fields, Arg<nlohmann::json>(args, "fields"));
    ABSL_ASSIGN_OR_RETURN(auto value, ReadHeader(fields));
    return Result(value.Validate());
  };
  (*operations)["schema.validate"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto fields, Arg<nlohmann::json>(args, "fields"));
    ABSL_ASSIGN_OR_RETURN(auto value, ReadSchema(fields));
    return Result(value.Validate());
  };
  (*operations)["schema.mapOutput"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto fields, Arg<nlohmann::json>(args, "fields"));
    ABSL_ASSIGN_OR_RETURN(auto value, ReadSchema(fields));
    ABSL_ASSIGN_OR_RETURN(auto output, Arg<std::string>(args, "output"));
    ABSL_ASSIGN_OR_RETURN(auto target, Arg<std::string>(args, "field"));
    ABSL_RETURN_IF_ERROR(
        value.MapOutputToJson(std::move(output), std::move(target)));
    return nlohmann::json(value.output_to_json_field);
  };
  (*operations)["schema.toJson"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto fields, Arg<nlohmann::json>(args, "fields"));
    ABSL_ASSIGN_OR_RETURN(auto value, ReadSchema(fields));
    ABSL_ASSIGN_OR_RETURN(auto runnable,
                          DefaultArg<bool>(args, "runnable", true));
    ABSL_ASSIGN_OR_RETURN(auto all, DefaultArg<bool>(args, "all", false));
    return actions::SchemaToJson(
        value, runnable,
        all ? actions::PortView::kAll : actions::PortView::kCallable);
  };
  (*operations)["schema.fromJson"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto entry, Arg<nlohmann::json>(args, "entry"));
    ABSL_ASSIGN_OR_RETURN(auto value, actions::SchemaFromJson(entry));
    return SchemaFields(value);
  };
  (*operations)["schema.entries"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto document, Arg<nlohmann::json>(args, "document"));
    ABSL_ASSIGN_OR_RETURN(auto entries, actions::SchemasInDocument(document));
    return nlohmann::json(std::move(entries));
  };
  (*operations)["schema.query"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto source, Arg<std::string>(args, "source"));
    ABSL_ASSIGN_OR_RETURN(auto query, actions::ParseSchemaQuery(source));
    auto fields = Save(std::make_shared<actions::SchemaQuery>(query));
    fields["all"] = query.ports == actions::PortView::kAll;
    fields["runnableOnly"] = query.runnable_only;
    return fields;
  };
  (*operations)["schema.accepts"] =
      [](std::int64_t self,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto query, Object<actions::SchemaQuery>(self));
    ABSL_ASSIGN_OR_RETURN(auto name, Arg<std::string>(args, "name"));
    return nlohmann::json(actions::SchemaQueryAccepts(*query, name));
  };
}
}  // namespace a11::jvm
