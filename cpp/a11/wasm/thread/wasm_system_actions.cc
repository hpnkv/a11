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

#include "thread/wasm_system_actions.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/escaping.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/concurrency/executor.h"
#include "a11/flow/catalogue_data.h"
#include "a11/json_codec.h"
#include "openssl/rand.h"
#include "sdk/flow/actions/options.h"
#include "sdk/flow/actions/ports.h"

namespace a11::wasm {
namespace {

absl::StatusOr<actions::ActionPortSchema> PortFromCatalogue(
    const nlohmann::json& value) {
  if (!value.is_object()) return absl::DataLossError("catalogue port is not an object");
  const auto name = value.find("name");
  const auto type = value.find("type");
  if (name == value.end() || !name->is_string() ||
      type == value.end() || !type->is_string()) {
    return absl::DataLossError("catalogue port lacks a name or type");
  }
  actions::ActionPortSchema port;
  port.name = name->get<std::string>();
  port.type = type->get<std::string>();
  if (auto field = value.find("description");
      field != value.end() && field->is_string()) {
    port.description = field->get<std::string>();
  }
  if (auto field = value.find("required");
      field != value.end() && field->is_boolean()) {
    port.required = field->get<bool>();
  }
  if (auto field = value.find("unary");
      field != value.end() && field->is_boolean()) {
    port.unary = field->get<bool>();
  }
  return port;
}

absl::StatusOr<std::string> SecureBytes(size_t count) {
  std::string bytes(count, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()),
                 static_cast<int>(count)) != 1) {
    return absl::UnavailableError("secure host entropy is unavailable");
  }
  return bytes;
}

absl::StatusOr<sdk::flow::Options> ReadOptions(
    const std::shared_ptr<actions::Action>& action) {
  ABSL_ASSIGN_OR_RETURN(auto input,
                        sdk::flow::ReadJsonInput(action, "options"));
  return sdk::flow::Options::Parse(input.has_value() ? &*input : nullptr);
}

absl::Status RunEnvironment(const std::shared_ptr<actions::Action>& action,
                            const std::shared_ptr<const Environment>& host) {
  ABSL_ASSIGN_OR_RETURN(auto raw,
                        sdk::flow::ReadJsonInput(action, "names"));
  ABSL_ASSIGN_OR_RETURN(auto options, ReadOptions(action));
  ABSL_ASSIGN_OR_RETURN(sdk::flow::OutputPorts outputs,
                        sdk::flow::OpenOutputs(action, options));
  if (!raw.has_value()) {
    return absl::InvalidArgumentError("env_get needs explicit names");
  }
  std::vector<std::string> names;
  if (raw->is_string()) {
    names.push_back(raw->get<std::string>());
  } else if (raw->is_array()) {
    for (const auto& value : *raw) {
      if (!value.is_string()) {
        return absl::InvalidArgumentError("names must be strings");
      }
      names.push_back(value.get<std::string>());
    }
  } else {
    return absl::InvalidArgumentError("names must be a string or string list");
  }
  nlohmann::json values = nlohmann::json::object();
  nlohmann::json missing = nlohmann::json::array();
  for (const auto& name : names) {
    if (host->allowed_names.find(name) == host->allowed_names.end()) {
      return absl::PermissionDeniedError(
          absl::StrCat("environment name '", name, "' is not allowed"));
    }
    if (auto found = host->values.find(name); found != host->values.end()) {
      values[name] = found->second;
    } else {
      missing.push_back(name);
    }
  }
  ABSL_RETURN_IF_ERROR(outputs["values"].PutOnly(values));
  ABSL_RETURN_IF_ERROR(outputs["missing"].PutOnly(missing));
  return outputs.Finish();
}

absl::Status RunRandom(const std::shared_ptr<actions::Action>& action) {
  ABSL_ASSIGN_OR_RETURN(auto options, ReadOptions(action));
  ABSL_ASSIGN_OR_RETURN(auto count,
                        options.IntInRange("count", 32, 1, 1024 * 1024));
  ABSL_ASSIGN_OR_RETURN(auto format,
                        options.Enum("format", "hex",
                                     {"hex", "base64", "base64url", "raw"}));
  ABSL_ASSIGN_OR_RETURN(sdk::flow::OutputPorts outputs,
                        sdk::flow::OpenOutputs(action, options));
  ABSL_ASSIGN_OR_RETURN(std::string bytes,
                        SecureBytes(static_cast<size_t>(count)));
  ABSL_RETURN_IF_ERROR(outputs["bytes"].Put(
      sdk::flow::BytesChunk(bytes), /*final=*/true));
  ABSL_RETURN_IF_ERROR(outputs["bytes"].Close());
  if (format == "raw") {
    ABSL_RETURN_IF_ERROR(outputs["text"].PutOnly(nlohmann::json()));
  } else {
    const std::string rendered = format == "hex"
        ? absl::BytesToHexString(bytes)
        : format == "base64" ? absl::Base64Escape(bytes)
                               : absl::WebSafeBase64Escape(bytes);
    ABSL_RETURN_IF_ERROR(outputs["text"].PutOnlyText(rendered));
  }
  return outputs.Finish();
}

absl::Status RunUuid(const std::shared_ptr<actions::Action>& action) {
  ABSL_ASSIGN_OR_RETURN(auto options, ReadOptions(action));
  ABSL_ASSIGN_OR_RETURN(auto count,
                        options.IntInRange("count", 1, 1, 100000));
  ABSL_ASSIGN_OR_RETURN(sdk::flow::OutputPorts outputs,
                        sdk::flow::OpenOutputs(action, options));
  for (std::int64_t index = 0; index < count; ++index) {
    ABSL_ASSIGN_OR_RETURN(std::string bytes, SecureBytes(16));
    bytes[6] = static_cast<char>((static_cast<unsigned char>(bytes[6]) & 0x0f) | 0x40);
    bytes[8] = static_cast<char>((static_cast<unsigned char>(bytes[8]) & 0x3f) | 0x80);
    const std::string hex = absl::BytesToHexString(bytes);
    const std::string uuid = absl::StrCat(
        hex.substr(0, 8), "-", hex.substr(8, 4), "-", hex.substr(12, 4), "-",
        hex.substr(16, 4), "-", hex.substr(20));
    ABSL_RETURN_IF_ERROR(outputs["ids"].PutText(uuid, index + 1 == count));
  }
  return outputs.Finish();
}

a11::actions::ActionHandler Plain(
    absl::Status (*run)(const std::shared_ptr<actions::Action>&)) {
  return [run](std::shared_ptr<actions::Action> action) {
    return a11::SubmitTask([run, action = std::move(action)]() {
      return run(action);
    });
  };
}

}  // namespace

absl::StatusOr<actions::ActionSchema> SchemaFromCatalogue(
    std::string_view name) {
  ABSL_ASSIGN_OR_RETURN(
      auto snapshot,
      a11::ParseJson(a11::flow::catalogue::kCatalogueSnapshot,
                     "Flow action catalogue"));
  const auto actions = snapshot.find("actions");
  if (actions == snapshot.end() || !actions->is_array()) {
    return absl::DataLossError("Flow action catalogue has no actions");
  }
  for (const auto& item : *actions) {
    if (!item.is_object()) continue;
    const auto found = item.find("name");
    if (found == item.end() || !found->is_string() ||
        found->get_ref<const std::string&>() != name) continue;
    actions::ActionSchema schema;
    schema.name = std::string(name);
    if (auto description = item.find("description");
        description != item.end() && description->is_string()) {
      schema.description = description->get<std::string>();
    }
    for (auto [key, target] : {
             std::pair{"inputs", &schema.inputs},
             std::pair{"outputs", &schema.outputs}}) {
      auto ports = item.find(key);
      if (ports == item.end() || !ports->is_array()) continue;
      for (const auto& value : *ports) {
        ABSL_ASSIGN_OR_RETURN(auto port, PortFromCatalogue(value));
        target->emplace(port.name, std::move(port));
      }
    }
    if (auto headers = item.find("headers");
        headers != item.end() && headers->is_array()) {
      for (const auto& value : *headers) {
        if (!value.is_object()) continue;
        auto header_name = value.find("name");
        if (header_name == value.end() || !header_name->is_string()) continue;
        actions::ActionHeaderSchema header;
        header.name = header_name->get<std::string>();
        if (auto description = value.find("description");
            description != value.end() && description->is_string()) {
          header.description = description->get<std::string>();
        }
        schema.headers.emplace(header.name, std::move(header));
      }
    }
    ABSL_RETURN_IF_ERROR(schema.Validate());
    return schema;
  }
  return absl::NotFoundError(absl::StrCat("action schema not found: ", name));
}

absl::Status RegisterSystemActions(actions::ActionRegistry& registry,
                                   std::shared_ptr<const Environment> environment,
                                   bool register_environment) {
  if (register_environment) {
    ABSL_ASSIGN_OR_RETURN(auto schema, SchemaFromCatalogue("env_get"));
    ABSL_RETURN_IF_ERROR(registry.Register(
        "env_get", std::move(schema),
        [environment = std::move(environment)](
            std::shared_ptr<actions::Action> action) {
          return a11::SubmitTask(
              [environment, action = std::move(action)]() {
                return RunEnvironment(action, environment);
              });
        }));
  }
  ABSL_ASSIGN_OR_RETURN(auto random_schema,
                        SchemaFromCatalogue("random_bytes"));
  ABSL_RETURN_IF_ERROR(registry.Register(
      "random_bytes", std::move(random_schema), Plain(&RunRandom)));
  ABSL_ASSIGN_OR_RETURN(auto uuid_schema, SchemaFromCatalogue("new_uuid"));
  uuid_schema.description = "Create version 4 UUIDs from secure host entropy.";
  return registry.Register("new_uuid", std::move(uuid_schema), Plain(&RunUuid));
}

}  // namespace a11::wasm
