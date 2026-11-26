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

#include "a11/flow/escape.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include "a11/percent.h"
#include "a11/utf8.h"

namespace a11::flow {
namespace {

bool Octal(char one) {
  return one >= '0' && one <= '7';
}

void AppendScalar(std::string& value, std::uint32_t point) {
  if (point < 0x80U) {
    value.push_back(static_cast<char>(point));
  } else if (point < 0x800U) {
    value.push_back(static_cast<char>(0xC0U | (point >> 6U)));
    value.push_back(static_cast<char>(0x80U | (point & 0x3FU)));
  } else if (point < 0x10000U) {
    value.push_back(static_cast<char>(0xE0U | (point >> 12U)));
    value.push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3FU)));
    value.push_back(static_cast<char>(0x80U | (point & 0x3FU)));
  } else {
    value.push_back(static_cast<char>(0xF0U | (point >> 18U)));
    value.push_back(static_cast<char>(0x80U | ((point >> 12U) & 0x3FU)));
    value.push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3FU)));
    value.push_back(static_cast<char>(0x80U | (point & 0x3FU)));
  }
}

bool Scalar(std::uint32_t point) {
  return point <= 0x10FFFFU && (point < 0xD800U || point > 0xDFFFU);
}

}  // namespace

size_t EscapeWidth(std::string_view body, size_t at) {
  if (at + 1 >= body.size()) {
    return 1;
  }
  const char kind = body[at + 1];
  if (Octal(kind)) {
    size_t end = at + 2;
    while (end < body.size() && end < at + 4 && Octal(body[end])) {
      ++end;
    }
    return end - at;
  }
  if (kind == 'x' || kind == 'u' || kind == 'U') {
    if (kind == 'u' && at + 2 < body.size() && body[at + 2] == '{') {
      const size_t close = body.find('}', at + 3);
      return close == std::string_view::npos ? body.size() - at
                                             : close + 1 - at;
    }
    const size_t digits = kind == 'x' ? 2 : kind == 'u' ? 4 : 8;
    return std::min(body.size() - at, digits + 2);
  }
  return 2;
}

EscapeResult DecodeLiteralBody(std::string_view body, bool bytes, bool raw) {
  EscapeResult result;
  result.value.reserve(body.size());
  for (size_t at = 0; at < body.size();) {
    const auto one = static_cast<unsigned char>(body[at]);
    if (bytes && one >= 0x80U) {
      size_t width = std::min(utf8::SequenceWidth(body[at]), body.size() - at);
      if (!utf8::IsValid(body.substr(at, width))) {
        width = 1;
      }
      result.failure = EscapeFailure{
          EscapeProblem::kNonAsciiByte, at, at + width,
          "A byte literal needs ASCII source; use an escape for this byte."};
      return result;
    }
    if (raw || one != '\\') {
      result.value.push_back(body[at++]);
      continue;
    }
    if (at + 1 >= body.size()) {
      result.failure =
          EscapeFailure{EscapeProblem::kMalformed, at, at + 1,
                        "A backslash needs a following character."};
      return result;
    }
    const size_t start = at;
    const char kind = body[at + 1];
    at += 2;
    if (kind == '\n') {
      continue;
    }
    if (Octal(kind)) {
      unsigned value = static_cast<unsigned>(kind - '0');
      for (int count = 1; count < 3 && at < body.size() && Octal(body[at]);
           ++count) {
        value = value * 8U + static_cast<unsigned>(body[at++] - '0');
      }
      if (value > 255U) {
        result.failure =
            EscapeFailure{EscapeProblem::kMalformed, start, at,
                          "An octal byte escape must be at most 377."};
        return result;
      }
      result.value.push_back(static_cast<char>(value));
      continue;
    }
    if (kind == 'x' || kind == 'u' || kind == 'U') {
      size_t digits = kind == 'x' ? 2 : kind == 'u' ? 4 : 8;
      const bool braced = kind == 'u' && at < body.size() && body[at] == '{';
      if (braced) {
        ++at;
        digits = 0;
        while (at + digits < body.size() &&
               percent::HexDigit(body[at + digits]) >= 0 && digits < 6) {
          ++digits;
        }
      }
      std::uint32_t point = 0;
      bool valid = digits != 0 && at + digits <= body.size();
      for (size_t digit = 0; valid && digit < digits; ++digit) {
        const int hex = percent::HexDigit(body[at + digit]);
        valid = hex >= 0;
        if (valid) {
          point = (point << 4U) | static_cast<unsigned>(hex);
        }
      }
      at += valid ? digits : 0;
      valid = valid && (!braced || (at < body.size() && body[at] == '}'));
      if (!valid || (kind != 'x' && !Scalar(point))) {
        result.failure = EscapeFailure{
            EscapeProblem::kMalformed, start,
            std::min(body.size(), start + EscapeWidth(body, start)),
            "A hexadecimal escape needs complete digits and a Unicode scalar "
            "value."};
        return result;
      }
      if (braced) {
        ++at;
      }
      if (kind == 'x') {
        result.value.push_back(static_cast<char>(point));
      } else {
        AppendScalar(result.value, point);
      }
      continue;
    }
    switch (kind) {
      case 'a':
        result.value.push_back('\a');
        break;
      case 'b':
        result.value.push_back('\b');
        break;
      case 'e':
        result.value.push_back('\x1b');
        break;
      case 'f':
        result.value.push_back('\f');
        break;
      case 'n':
        result.value.push_back('\n');
        break;
      case 'r':
        result.value.push_back('\r');
        break;
      case 't':
        result.value.push_back('\t');
        break;
      case 'v':
        result.value.push_back('\v');
        break;
      default:
        result.value.push_back(kind);
        break;
    }
  }
  if (!bytes && !utf8::IsValid(result.value)) {
    result.failure =
        EscapeFailure{EscapeProblem::kInvalidUtf8, 0, body.size(),
                      "A string literal must decode to valid UTF-8."};
  }
  return result;
}

std::string EscapeLiteralBody(std::string_view value, bool bytes) {
  static constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string out;
  for (size_t at = 0; at < value.size();) {
    const auto one = static_cast<unsigned char>(value[at]);
    if (one == '\\' || one == '\'' || one == '"') {
      out.push_back('\\');
      out.push_back(static_cast<char>(one));
      ++at;
    } else if (one == '\n' || one == '\r' || one == '\t' || one == '\a' ||
               one == '\b' || one == '\f' || one == '\v') {
      const char escaped = one == '\n'   ? 'n'
                           : one == '\r' ? 'r'
                           : one == '\t' ? 't'
                           : one == '\a' ? 'a'
                           : one == '\b' ? 'b'
                           : one == '\f' ? 'f'
                                         : 'v';
      out.push_back('\\');
      out.push_back(escaped);
      ++at;
    } else if (one >= 0x20U && one < 0x7FU) {
      out.push_back(static_cast<char>(one));
      ++at;
    } else if (bytes || one < 0x80U) {
      out += "\\x";
      out.push_back(kHex[one >> 4U]);
      out.push_back(kHex[one & 0x0FU]);
      ++at;
    } else {
      std::uint32_t point = one;
      const size_t width = utf8::SequenceWidth(static_cast<char>(one));
      point &= width == 2 ? 0x1FU : width == 3 ? 0x0FU : 0x07U;
      for (size_t index = 1; index < width; ++index) {
        point = (point << 6U) |
                (static_cast<unsigned char>(value[at + index]) & 0x3FU);
      }
      out += point <= 0xFFFFU ? "\\u" : "\\U";
      const int digits = point <= 0xFFFFU ? 4 : 8;
      for (int digit = digits - 1; digit >= 0; --digit) {
        out.push_back(kHex[(point >> (digit * 4)) & 0x0FU]);
      }
      at += width;
    }
  }
  return out;
}

}  // namespace a11::flow
