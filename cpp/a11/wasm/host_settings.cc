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

#include "host_settings.h"

#include <string>
#include <utility>

#include <absl/status/status.h>
#include <nlohmann/json.hpp>

namespace a11::wasm {

absl::StatusOr<HostSettings> ParseHostSettings(const nlohmann::json& value) {
  if (!value.is_object()) {
    return absl::InvalidArgumentError("host settings must be an object");
  }
  HostSettings settings;
  if (const auto actions = value.find("actions"); actions != value.end()) {
    if (!actions->is_object()) {
      return absl::InvalidArgumentError("host actions must be an object");
    }
    for (const auto& [name, target] :
         {std::pair{"env_get", &settings.register_environment},
          std::pair{"make_http_request", &settings.register_make_http_request},
          std::pair{"web-fetch", &settings.register_web_fetch}}) {
      if (const auto enabled = actions->find(name); enabled != actions->end()) {
        if (!enabled->is_boolean()) {
          return absl::InvalidArgumentError(
              std::string(name) + " registration must be boolean");
        }
        *target = enabled->get<bool>();
      }
    }
  }
  if (const auto names = value.find("allowedEnvironmentNames");
      names != value.end()) {
    if (!names->is_array()) {
      return absl::InvalidArgumentError(
          "allowedEnvironmentNames must be an array");
    }
    for (const auto& name : *names) {
      if (!name.is_string()) {
        return absl::InvalidArgumentError(
            "allowedEnvironmentNames must contain strings");
      }
      settings.environment->allowed_names.insert(name.get<std::string>());
    }
  }
  if (const auto values = value.find("environment"); values != value.end()) {
    if (!values->is_object()) {
      return absl::InvalidArgumentError("environment must be an object");
    }
    for (const auto& item : values->items()) {
      if (!item.value().is_string()) {
        return absl::InvalidArgumentError(
            "environment values must be strings");
      }
      settings.environment->values.emplace(item.key(),
                                           item.value().get<std::string>());
    }
  }
  return settings;
}

}  // namespace a11::wasm
