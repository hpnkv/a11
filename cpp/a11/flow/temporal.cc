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

#include "a11/flow/temporal.h"

#include <cstdlib>
#include <string>

#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <cmath>

#include "a11/flow/vocabulary.h"

namespace a11::flow {
absl::Duration SecondsDuration(double total) {
  if (std::isinf(total)) {
    return total > 0 ? absl::InfiniteDuration() : -absl::InfiniteDuration();
  }
  return absl::Seconds(total);
}

std::optional<absl::Duration> ParseDuration(std::string_view text) {
  const std::string_view trimmed = absl::StripAsciiWhitespace(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  const std::string lowered = absl::AsciiStrToLower(trimmed);
  if (lowered == "forever" || lowered == "infinite" || lowered == "inf") {
    return absl::InfiniteDuration();
  }
  double total = 0.0;
  double sign = 1.0;
  size_t at = 0;
  int pieces = 0;
  while (at < trimmed.size()) {
    if (trimmed[at] == ' ' || trimmed[at] == '\t') {
      ++at;
      continue;
    }
    const size_t began = at;
    if (trimmed[at] == '+' || trimmed[at] == '-') {
      ++at;
    }
    const size_t whole = at;
    while (at < trimmed.size() &&
           absl::ascii_isdigit(static_cast<unsigned char>(trimmed[at]))) {
      ++at;
    }
    if (at == whole) {
      return std::nullopt;
    }
    if (at < trimmed.size() && trimmed[at] == '.') {
      ++at;
      const size_t fraction = at;
      while (at < trimmed.size() &&
             absl::ascii_isdigit(static_cast<unsigned char>(trimmed[at]))) {
        ++at;
      }
      if (at == fraction) {
        return std::nullopt;
      }
    }
    const std::string number(trimmed.substr(began, at - began));
    while (at < trimmed.size() && (trimmed[at] == ' ' || trimmed[at] == '\t')) {
      ++at;
    }
    const size_t word = at;
    while (at < trimmed.size() &&
           absl::ascii_isalpha(static_cast<unsigned char>(trimmed[at]))) {
      ++at;
    }
    const std::string_view unit = trimmed.substr(word, at - word);
    double scale = 1.0;
    if (!unit.empty()) {
      const auto seconds = vocabulary::DurationUnitSeconds(unit);
      if (!seconds.has_value()) {
        return std::nullopt;
      }
      scale = *seconds;
    }
    // `-1m30s` is a minute and a half, backwards, not a minute back and half a
    // second forwards: the sign belongs to the whole.
    if (pieces == 0 && absl::StartsWith(number, "-")) {
      sign = -1.0;
    }
    total += std::abs(std::strtod(number.c_str(), nullptr)) * scale;
    ++pieces;
  }
  if (pieces == 0) {
    return std::nullopt;
  }
  return SecondsDuration(sign * total);
}

std::optional<absl::Time> ParseTime(std::string_view text) {
  const std::string_view trimmed = absl::StripAsciiWhitespace(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  // The spellings RFC 3339 allows and `TimeText` writes, and the one a
  // timestamp with no zone arrives as: that one is UTC here, which is the zone
  // every instant this language writes is in.
  static constexpr std::string_view kPatterns[] = {
      "%Y-%m-%dT%H:%M:%E*S%Ez",
      "%Y-%m-%dT%H:%M:%E*S",
      "%Y-%m-%d %H:%M:%E*S%Ez",
      "%Y-%m-%d %H:%M:%E*S",
      "%Y-%m-%d",
  };
  for (const std::string_view pattern : kPatterns) {
    absl::Time when;
    std::string error;
    if (absl::ParseTime(pattern, trimmed, absl::UTCTimeZone(), &when, &error)) {
      return when;
    }
  }
  return std::nullopt;
}

}  // namespace a11::flow
