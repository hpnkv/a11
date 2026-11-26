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

#ifndef A11_FLOW_ESCAPE_H_
#define A11_FLOW_ESCAPE_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace a11::flow {

enum class EscapeProblem { kMalformed, kInvalidUtf8, kNonAsciiByte };

struct EscapeFailure {
  EscapeProblem problem;
  size_t start;
  size_t end;
  std::string message;
};

struct EscapeResult {
  std::string value;
  std::optional<EscapeFailure> failure;
};

/// Decode the body of a Flow literal. Text results must be valid UTF-8.
EscapeResult DecodeLiteralBody(std::string_view body, bool bytes, bool raw);

/// A complete source escape's width, including its backslash.
size_t EscapeWidth(std::string_view body, size_t at);

/// An ASCII literal body safe between either kind of Flow quotes.
std::string EscapeLiteralBody(std::string_view value, bool bytes);

}  // namespace a11::flow

#endif  // A11_FLOW_ESCAPE_H_
