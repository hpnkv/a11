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

/**
 * @file
 * @brief UTF-8 byte-level primitives, as inline functions over a string_view.
 *
 * Inline functions let `a11::flow_lang` share UTF-8 rules without a link
 * dependency.
 */

#ifndef A11_UTF8_H_
#define A11_UTF8_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace a11::utf8 {

/// Whether @p byte is a continuation byte -- the middle of a character.
inline bool IsContinuation(char byte) {
  return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

/**
 * @brief Bytes in the sequence @p lead starts, from the lead byte alone.
 *
 * Returns a width from the lead byte's range. ASCII and continuation bytes
 * have width 1. Use IsValid() to reject malformed input.
 */
inline size_t SequenceWidth(char lead) {
  const auto byte = static_cast<unsigned char>(lead);
  if (byte >= 0xF0U) {
    return 4;
  }
  if (byte >= 0xE0U) {
    return 3;
  }
  if (byte >= 0xC0U) {
    return 2;
  }
  return 1;
}

/// UTF-16 code units the character at @p lead occupies: 2 above the BMP, else
/// 1.
inline size_t Utf16Units(char lead) {
  return static_cast<unsigned char>(lead) >= 0xF0U ? 2 : 1;
}

/**
 * @brief Whether @p text is well-formed UTF-8.
 *
 * Rejects overlong encodings, surrogate code points and values above U+10FFFF.
 */
inline bool IsValid(std::string_view text) {
  // The smallest code point each width may encode; anything smaller is an
  // overlong form of a shorter sequence.
  static constexpr std::uint32_t kSmallest[] = {0, 0, 0x80, 0x800, 0x10000};
  size_t at = 0;
  while (at < text.size()) {
    const auto lead = static_cast<unsigned char>(text[at]);
    if (lead < 0x80U) {
      ++at;
      continue;
    }
    size_t width = 0;
    std::uint32_t point = 0;
    if ((lead & 0xE0U) == 0xC0U) {
      width = 2;
      point = lead & 0x1FU;
    } else if ((lead & 0xF0U) == 0xE0U) {
      width = 3;
      point = lead & 0x0FU;
    } else if ((lead & 0xF8U) == 0xF0U) {
      width = 4;
      point = lead & 0x07U;
    } else {
      // A continuation byte with no lead, or a 5-byte form UTF-8 has not had
      // since 2003.
      return false;
    }
    if (text.size() - at < width) {
      return false;
    }
    for (size_t index = 1; index < width; ++index) {
      if (!IsContinuation(text[at + index])) {
        return false;
      }
      point = (point << 6U) |
              (static_cast<unsigned char>(text[at + index]) & 0x3FU);
    }
    if (point < kSmallest[width] || point > 0x10FFFFU ||
        (point >= 0xD800U && point <= 0xDFFFU)) {
      return false;
    }
    at += width;
  }
  return true;
}

}  // namespace a11::utf8

#endif  // A11_UTF8_H_
