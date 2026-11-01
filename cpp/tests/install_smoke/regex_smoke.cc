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

#include <utility>

#include "regex_holder.h"

int main() {
  auto compiled = a11::internal::CompileRegex("needle");
  if (!compiled.ok()) {
    return 1;
  }
  RegexHolder holder{std::move(*compiled)};
  if (!holder.expression.FullMatch("needle") ||
      holder.expression.FullMatch("a needle") ||
      !holder.expression.PartialMatch("a needle in text")) {
    return 1;
  }

  auto other = a11::internal::CompileRegex("(?i)error");
  if (!other.ok()) {
    return 1;
  }
  holder.expression = std::move(*other);
  RegexHolder moved{std::move(holder)};
  if (!moved.expression.PartialMatch("an ERROR occurred")) {
    return 1;
  }

  const auto invalid = a11::internal::CompileRegex("[");
  return invalid.status().code() == absl::StatusCode::kInvalidArgument ? 0 : 1;
}
