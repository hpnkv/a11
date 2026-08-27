// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Percent-encoding, as inline functions over a string_view.
 *
 * Header-only so that the three unrelated libraries needing it -- a Redis URL,
 * a W3C baggage header, an action-discovery query string -- share one
 * implementation without any of them taking a link dependency on the others.
 */

#ifndef A11_PERCENT_H_
#define A11_PERCENT_H_

#include <cstddef>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>

namespace a11::percent {

/// The value 0-15 of a hex digit, either case, or -1 if @p one is not one.
inline int HexDigit(char one) {
  if (one >= '0' && one <= '9') {
    return one - '0';
  }
  if (one >= 'a' && one <= 'f') {
    return one - 'a' + 10;
  }
  if (one >= 'A' && one <= 'F') {
    return one - 'A' + 10;
  }
  return -1;
}

/**
 * @brief Percent-decodes @p value, leaving anything malformed as it stands.
 *
 * The lenient posture: an undecodable `%` is passed through as itself rather
 * than dropped, so a malformed query gives a "no such thing" answer instead of
 * a silently different one, and a propagator keeps a baggage value it cannot
 * read rather than losing it.
 *
 * @param value Text to decode.
 * @param plus_is_space Map `+` to a space, as
 * `application/x-www-form-urlencoded`
 *   does. Off for a URL path or a baggage value, where `+` is literal.
 */
inline std::string Decode(std::string_view value, bool plus_is_space = false) {
  std::string decoded;
  decoded.reserve(value.size());
  for (size_t index = 0; index < value.size(); ++index) {
    const char one = value[index];
    if (plus_is_space && one == '+') {
      decoded.push_back(' ');
      continue;
    }
    if (one == '%' && index + 2 < value.size()) {
      const int high = HexDigit(value[index + 1]);
      const int low = HexDigit(value[index + 2]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2;
        continue;
      }
    }
    decoded.push_back(one);
  }
  return decoded;
}

/**
 * @brief Percent-decodes @p value, or says why it cannot be decoded.
 *
 * The strict posture, for text where a malformed escape means the caller got
 * the input wrong and should hear about it rather than connect somewhere
 * slightly different -- a URL carrying credentials, for instance.
 *
 * @param value Text to decode.
 * @param what Names the input in the error, e.g. "Redis URL".
 */
inline absl::StatusOr<std::string> DecodeStrict(std::string_view value,
                                                std::string_view what) {
  std::string decoded;
  decoded.reserve(value.size());
  for (size_t index = 0; index < value.size(); ++index) {
    if (value[index] != '%') {
      decoded.push_back(value[index]);
      continue;
    }
    if (index + 2 >= value.size()) {
      return absl::InvalidArgumentError(
          absl::StrCat("Truncated percent escape in ", what));
    }
    const int high = HexDigit(value[index + 1]);
    const int low = HexDigit(value[index + 2]);
    if (high < 0 || low < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid percent escape in ", what));
    }
    decoded.push_back(static_cast<char>((high << 4) | low));
    index += 2;
  }
  return decoded;
}

/// Percent-encodes everything outside RFC 3986's unreserved set.
inline std::string Encode(std::string_view value) {
  static constexpr std::string_view kLowerHex = "0123456789abcdef";
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    if (absl::ascii_isalnum(byte) || byte == '-' || byte == '.' ||
        byte == '_' || byte == '~') {
      out.push_back(ch);
      continue;
    }
    out.push_back('%');
    out.push_back(kLowerHex[byte >> 4U]);
    out.push_back(kLowerHex[byte & 0x0FU]);
  }
  return out;
}

}  // namespace a11::percent

#endif  // A11_PERCENT_H_
