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

#ifndef A11_FLOW_REGEX_LITERAL_H_
#define A11_FLOW_REGEX_LITERAL_H_

#include <cstddef>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

namespace a11::flow {

/// Converts a slash-delimited Flow literal to an RE2 expression.
inline absl::StatusOr<std::string> RegexExpression(std::string_view literal) {
  if (literal.size() < 2 || literal.front() != '/') {
    return absl::InvalidArgumentError("Expected a regex literal.");
  }
  const size_t close = literal.rfind('/');
  if (close == 0) {
    return absl::InvalidArgumentError("Unterminated regex literal.");
  }
  const std::string_view body = literal.substr(1, close - 1);
  const std::string_view flags = literal.substr(close + 1);
  bool seen_i = false;
  bool seen_m = false;
  bool seen_s = false;
  for (const char flag : flags) {
    bool* seen = flag == 'i'   ? &seen_i
                 : flag == 'm' ? &seen_m
                 : flag == 's' ? &seen_s
                               : nullptr;
    if (seen == nullptr || *seen) {
      return absl::InvalidArgumentError(
          "Regex flags must be unique i, m or s.");
    }
    *seen = true;
  }
  std::string expression;
  expression.reserve(body.size() + flags.size() + 4);
  if (!flags.empty()) {
    expression.append("(?");
    expression.append(flags);
    expression.push_back(')');
  }
  expression.append(body);
  return expression;
}

}  // namespace a11::flow

#endif  // A11_FLOW_REGEX_LITERAL_H_
