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

/// The formatted text, and what it took to get there.
struct FormatResult {
  /// The whole file as it should be. Equal to the input when nothing changed --
  /// and equal to the input, unchanged, when the file does not parse.
  std::string formatted;
  bool changed = false;
  /// One edit that turns the input into `formatted`, trimmed to the part that
  /// actually differs, so an editor applying it does not move the cursor or
  /// lose a selection over a file that only changed at the bottom. Empty when
  /// nothing changed.
  std::vector<Edit> edits;
  /// Why the file was left alone, where it was.
  std::vector<Diagnostic> diagnostics;
};

/// Format Flow source.
///
/// **What it decides:** indentation, the spaces between tokens, how far a
/// continued line is indented, how many blank lines are allowed and where, the
/// columns of a run of port declarations, trailing whitespace, and the newline
/// at
/// the end of the file.
///
/// Equivalent whitespace has one layout, preserving parsed statement boundaries.
/// Groups stay compact when they fit, or expand with indented contents and
/// dedented closers. Trailing commas request expanded item lists. Module bodies
/// have no extra indentation. Declaration modifiers follow canonical order.
/// Ordinary strings split into adjacent literals at spaces or between complete
/// characters, preserving escapes. Triple-quoted strings and comments remain
/// intact.
///
/// **It refuses a file it cannot read.** With an error diagnostic, `formatted`
/// is the input and `diagnostics` says why: half-formatting a file somebody is
/// in the middle of typing is how a formatter loses somebody's work.
///
/// Two invariants, both tested over every flow in the repository:
///
/// * **Idempotent.** Formatting formatted text changes nothing.
/// * **Meaning-preserving.** Syntax stays equivalent, including literal values.
///   Declaration modifiers may move; comment text remains.
FormatResult Format(std::string_view source, FormatOptions options = {},
                    CompilationLimits limits = {},
                    CompilationBudget* absl_nullable budget = nullptr);

}  // namespace a11::flow

#endif  // A11_FLOW_FORMAT_H_
