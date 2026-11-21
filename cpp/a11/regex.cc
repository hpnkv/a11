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

#include <limits>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>
#include <re2/re2.h>

namespace a11::internal {

Regex::Regex(std::unique_ptr<re2::RE2> expression)
    : expression_(std::move(expression)) {
  const auto& named = expression_->NamedCapturingGroups();
  if (!named.empty()) {
    names_.resize(static_cast<size_t>(expression_->NumberOfCapturingGroups()) +
                  1);
  }
  for (const auto& [name, group] : named) {
    names_[static_cast<size_t>(group)] = name;
  }
}

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

size_t Regex::CaptureCount() const {
  return static_cast<size_t>(expression_->NumberOfCapturingGroups());
}

std::string_view Regex::CaptureName(size_t group) const {
  return group < names_.size() ? names_[group] : std::string_view{};
}

absl::StatusOr<std::optional<RegexMatch>> Regex::Find(std::string_view text,
                                                      size_t start) const {
  if (CaptureCount() > 32) {
    return absl::InvalidArgumentError("A regex may capture at most 32 groups.");
  }
  if (start > text.size()) {
    return std::nullopt;
  }
  if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return absl::ResourceExhaustedError("Regex input exceeds the RE2 limit.");
  }
  std::vector<absl::string_view> groups(CaptureCount() + 1);
  const absl::string_view input(text.data() == nullptr ? "" : text.data(),
                                text.size());
  if (!expression_->Match(input, start, text.size(), re2::RE2::UNANCHORED,
                          groups.data(), static_cast<int>(groups.size()))) {
    return std::nullopt;
  }
  RegexMatch found;
  found.start = groups[0].data() == nullptr
                    ? start
                    : static_cast<size_t>(groups[0].data() - input.data());
  found.end = found.start + groups[0].size();
  found.groups.reserve(groups.size());
  for (size_t index = 0; index < groups.size(); ++index) {
    const auto group = groups[index];
    if (group.data() == nullptr && index == 0) {
      found.groups.push_back({std::string_view{}, found.start, 0});
    } else if (group.data() == nullptr) {
      found.groups.push_back({});
    } else {
      found.groups.push_back({std::string_view(group.data(), group.size()),
                              static_cast<size_t>(group.data() - input.data()),
                              group.size()});
    }
  }
  return found;
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
