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

#ifndef A11_INTERNAL_REGEX_H_
#define A11_INTERNAL_REGEX_H_

#include <memory>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <re2/re2.h>

namespace a11::internal {

/// Compiles an RE2 pattern, reporting invalid syntax without logging it.
inline absl::StatusOr<std::unique_ptr<RE2>> CompileRegex(
    std::string_view pattern) {
  RE2::Options options;
  options.set_log_errors(false);
  auto expression = std::make_unique<RE2>(
      absl::string_view(pattern.data(), pattern.size()), options);
  if (!expression->ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Not a valid RE2 pattern: ", expression->error()));
  }
  return expression;
}

}  // namespace a11::internal

#endif  // A11_INTERNAL_REGEX_H_
