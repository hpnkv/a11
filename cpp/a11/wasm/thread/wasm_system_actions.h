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

#ifndef A11_WASM_SYSTEM_ACTIONS_H_
#define A11_WASM_SYSTEM_ACTIONS_H_

#include <memory>
#include <string>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/actions/registry.h"
#include "a11/actions/schema.h"

namespace a11::wasm {

struct Environment {
  absl::flat_hash_map<std::string, std::string> values;
  absl::flat_hash_set<std::string> allowed_names;
};

absl::StatusOr<actions::ActionSchema> SchemaFromCatalogue(
    std::string_view name);
absl::Status RegisterSystemActions(actions::ActionRegistry& registry,
                                   std::shared_ptr<const Environment> environment,
                                   bool register_environment);

}  // namespace a11::wasm

#endif  // A11_WASM_SYSTEM_ACTIONS_H_
