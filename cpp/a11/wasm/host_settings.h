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

#ifndef A11_WASM_HOST_SETTINGS_H_
#define A11_WASM_HOST_SETTINGS_H_

#include <memory>

#include <absl/status/statusor.h>
#include <nlohmann/json_fwd.hpp>

#include "thread/wasm_system_actions.h"

namespace a11::wasm {

struct HostSettings {
  std::shared_ptr<Environment> environment =
      std::make_shared<Environment>();
  bool register_environment = true;
  bool register_make_http_request = true;
  bool register_web_fetch = true;
};

absl::StatusOr<HostSettings> ParseHostSettings(const nlohmann::json& value);

}  // namespace a11::wasm

#endif  // A11_WASM_HOST_SETTINGS_H_
