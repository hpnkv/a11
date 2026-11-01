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

#include "a11/internal/regex.h"

#include <utility>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>
#include <re2/re2.h>

namespace a11::internal {

Regex::Regex(std::unique_ptr<re2::RE2> expression)
    : expression_(std::move(expression)) {}

Regex::Regex(Regex&&) noexcept = default;
Regex& Regex::operator=(Regex&&) noexcept = default;
Regex::~Regex() = default;

bool Regex::FullMatch(std::string_view text) const {
  return RE2::FullMatch(absl::string_view(text.data(), text.size()),
                        *expression_);
}

bool Regex::PartialMatch(std::string_view text) const {
  return RE2::PartialMatch(absl::string_view(text.data(), text.size()),
                           *expression_);
}

absl::StatusOr<Regex> CompileRegex(std::string_view pattern) {
  RE2::Options options;
  options.set_log_errors(false);
  auto expression = std::make_unique<re2::RE2>(
      absl::string_view(pattern.data(), pattern.size()), options);
  if (!expression->ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Not a valid RE2 pattern: ", expression->error()));
  }
  return Regex(std::move(expression));
}

}  // namespace a11::internal
