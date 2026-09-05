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

/**
 * @file
 * @brief The flow language's exception boundary.
 *
 * Compiled with exceptions (see the exception policy block in
 * cpp/CMakeLists.txt) for one reason: std::regex reports a malformed pattern by
 * throwing. See a11/flow/internal/pattern.h.
 */

#include <regex>
#include <string>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>

#include "a11/flow/internal/pattern.h"

namespace a11::flow::internal {

absl::StatusOr<std::regex> CompilePattern(const std::string& pattern) {
  try {
    return std::regex(pattern, std::regex::ECMAScript);
  } catch (const std::regex_error& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Not a regular expression: ", error.what()));
  }
}

}  // namespace a11::flow::internal
