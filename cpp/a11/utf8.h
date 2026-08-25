// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief UTF-8 byte-level primitives, as inline functions over a string_view.
 *
 * Header-only and dependency-free on purpose: `a11::flow_lang` links nothing but
 * Abseil and nlohmann, so the lexer, the diagnostic index and the offset table
 * can only share these if sharing costs no link dependency.
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
 * 1 for ASCII *and* for a malformed lead, so that walking a document that is
 * not quite UTF-8 advances rather than stalling or running off the end. A
 * caller that must reject malformed input wants IsValid instead.
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

/// UTF-16 code units the character at @p lead occupies: 2 above the BMP, else 1.
inline size_t Utf16Units(char lead) {
  return static_cast<unsigned char>(lead) >= 0xF0U ? 2 : 1;
}

/**
 * @brief Whether @p text is well-formed UTF-8.
 *
 * Strict: overlong encodings, surrogate halves and anything above U+10FFFF are
 * rejected, which is the definition Python's `bytes.decode("utf-8")`, Kotlin's
 * `String(bytes)` and JavaScript's `TextDecoder` with `fatal` all enforce. See
 * a11::IsValidUtf8 in `a11/json_codec.h` for why A11 refuses such bytes on the
 * way out rather than letting a peer fail on them.
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
