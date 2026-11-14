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

#ifndef A11_FLOW_FORMAT_H_
#define A11_FLOW_FORMAT_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "a11/flow/diagnostic.h"
#include "a11/flow/limits.h"

namespace a11::flow {

/// What the formatter is allowed to decide.
///
/// Input and output declarations form separate groups with a blank line.
/// The fixed style keeps formatting consistent across projects.
struct FormatOptions {
  /// Spaces per block level. Repository Flow files use two.
  int indent = 2;
  /// Whether to align consecutive `in` and `out` declaration columns.
  bool align_ports = true;
  /// Maximum columns where syntax permits a break; zero leaves lines unbound.
  size_t line_width = 80;
};

/// Formatted source, edits, and diagnostics.
struct FormatResult {
  /// Complete source; equals the input when parsing fails or no edit is needed.
  std::string formatted;
  bool changed = false;
  /// Minimal edits producing `formatted`; empty when unchanged.
  std::vector<Edit> edits;
  /// Diagnostics that prevented formatting.
  std::vector<Diagnostic> diagnostics;
};

/// Formats Flow source with stable whitespace and equivalent syntax.
/// Parse errors leave the source unchanged and appear in `diagnostics`.
/// Formatting is idempotent; comments and literal values are preserved.
FormatResult Format(std::string_view source, FormatOptions options = {},
                    CompilationLimits limits = {},
                    CompilationBudget* absl_nullable budget = nullptr);

}  // namespace a11::flow

#endif  // A11_FLOW_FORMAT_H_
