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

#include "a11/flow/casefold.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

#include <absl/status/status.h>

#include "a11/utf8.h"

namespace a11::flow {
namespace {

struct CasefoldMapping {
  std::uint32_t codepoint;
  std::string_view replacement;
};

// Generated from Unicode 17.0.0 by scripts/generate_flow_casefold.py.
constexpr CasefoldMapping kCasefoldMappings[] = {
#include "a11/flow/casefold_data.inc"
};

std::uint32_t Decode(std::string_view text) {
  const auto lead = static_cast<unsigned char>(text.front());
  if (text.size() == 1) {
    return lead;
  }
  const unsigned mask = text.size() == 2   ? 0x1FU
                        : text.size() == 3 ? 0x0FU
                                           : 0x07U;
  std::uint32_t point = lead & mask;
  for (std::size_t i = 1; i < text.size(); ++i) {
    point = (point << 6U) | (static_cast<unsigned char>(text[i]) & 0x3FU);
  }
  return point;
}

}  // namespace

absl::StatusOr<std::string> CasefoldUtf8(std::string_view input) {
  if (!utf8::IsValid(input)) {
    return absl::InvalidArgumentError("std::casefold requires valid UTF-8");
  }
  std::string output;
  output.reserve(input.size());
  for (std::size_t at = 0; at < input.size();) {
    const auto lead = static_cast<unsigned char>(input[at]);
    if (lead < 0x80U) {
      output.push_back(lead >= 'A' && lead <= 'Z'
                           ? static_cast<char>(lead + ('a' - 'A'))
                           : static_cast<char>(lead));
      ++at;
      continue;
    }
    const std::size_t width = utf8::SequenceWidth(input[at]);
    const std::string_view character = input.substr(at, width);
    const std::uint32_t codepoint = Decode(character);
    const auto mapping = std::lower_bound(
        std::begin(kCasefoldMappings), std::end(kCasefoldMappings), codepoint,
        [](const CasefoldMapping& entry, std::uint32_t point) {
          return entry.codepoint < point;
        });
    if (mapping != std::end(kCasefoldMappings) &&
        mapping->codepoint == codepoint) {
      output.append(mapping->replacement);
    } else {
      output.append(character);
    }
    at += width;
  }
  return output;
}

}  // namespace a11::flow
