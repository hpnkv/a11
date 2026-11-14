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

#ifndef A11_FLOW_SCHEMA_H_
#define A11_FLOW_SCHEMA_H_

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "a11/flow/diagnostic.h"
#include "a11/flow/plan.h"

namespace a11::flow {

/// Converts a Flow shape to JSON Schema Draft 2020-12.
/// Referenced shapes use `$defs` and `$ref`, including self-references.
/// `bytes`, `time`, and `duration` use strings with `x-a11-type` metadata.
nlohmann::json DtoToJsonSchema(const DtoPlan& dto, const Program& program);

/// The shapes a JSON Schema describes, and what would not fit.
struct SchemaImport {
  /// Root shape followed by referenced `$defs` entries.
  std::vector<DtoPlan> dtos;
  /// Unsupported schema features reported without Flow source ranges.
  std::vector<Diagnostic> diagnostics;
};

/// Imports shapes from `schema`, using `name` when the root has no name.
/// Unsupported keywords produce diagnostics alongside imported fields.
SchemaImport JsonSchemaToDtos(const nlohmann::json& schema,
                              std::string_view name);

/// Renders one shape as formatted Flow source.
std::string DtoToFlow(const DtoPlan& dto);

/// The `format` field of the schema envelope.
inline constexpr std::string_view kSchemaFormat = "flow.schema/v1";

/// The extension key that says which Flow type a string really is.
inline constexpr std::string_view kFlowTypeKey = "x-a11-type";

/// Extension key preserving a shape's field declaration order.
inline constexpr std::string_view kFlowOrderKey = "x-a11-order";

}  // namespace a11::flow

#endif  // A11_FLOW_SCHEMA_H_
