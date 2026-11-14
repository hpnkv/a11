/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef A11_FLOW_SERVICE_H_
#define A11_FLOW_SERVICE_H_

#include <string_view>

#include <absl/types/span.h>
#include <nlohmann/json_fwd.hpp>

#include "a11/flow/module.h"

namespace a11::flow {

/// Handles one language service request and returns its JSON envelope.
/// Requests name a method and source; an optional `id` is echoed in the result.
/// Invalid requests return `ok: false`; source problems return diagnostics.
/// `describe` accepts `snippet: true` for a Flow body fragment.
nlohmann::json Handle(const nlohmann::json& request,
                      ModuleResolver resolver = {},
                      CompilationLimits limits = {});

/// Every method [Handle] knows, in the order `--help` lists them.
absl::Span<const std::string_view> Methods();

/// What a method takes and gives back, one line, for `--help`.
std::string_view MethodSummary(std::string_view method);

}  // namespace a11::flow

#endif  // A11_FLOW_SERVICE_H_
