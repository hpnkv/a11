// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The flow language's exception boundary.
 *
 * Compiled with exceptions (see the exception policy block in
 * cpp/CMakeLists.txt) for one reason: std::regex reports a malformed pattern by
 * throwing. See a11/flow/internal/pattern.h.
 */

#include "a11/flow/internal/pattern.h"

#include <regex>
#include <string>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>

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
