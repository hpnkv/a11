// Copyright 2026 The A11 Authors.

#include "a11/flow/offsets.h"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

#include "a11/utf8.h"

namespace a11::flow {
namespace {

/// The four names a `flow.*` envelope uses for a byte offset into the document.
constexpr std::string_view kOffsetFields[] = {"start", "end", "offset",
                                              "prefix_start"};

bool IsOffsetField(std::string_view name) {
  return std::find(std::begin(kOffsetFields), std::end(kOffsetFields), name) !=
         std::end(kOffsetFields);
}

}  // namespace

TextIndex::TextIndex(std::string text) : text_(std::move(text)) {
  line_starts_.push_back(0);
  line_utf16_starts_.push_back(0);
  size_t units = 0;
  size_t at = 0;
  while (at < text_.size()) {
    const auto [width, size] = Step(at);
    at += width;
    units += size;
    if (text_[at - width] == '\n') {
      line_starts_.push_back(at);
      line_utf16_starts_.push_back(units);
    }
  }
}

std::pair<size_t, size_t> TextIndex::Step(size_t offset) const {
  size_t width = utf8::SequenceWidth(text_[offset]);
  // A truncated sequence advances one byte, so a document that is not quite
  // UTF-8 is still indexed rather than walked off the end of.
  if (offset + width > text_.size()) {
    width = 1;
  }
  return {width, utf8::Utf16Units(text_[offset])};
}

size_t TextIndex::LineEnd(size_t line) const {
  return line + 1 < line_starts_.size() ? line_starts_[line + 1] - 1
                                        : text_.size();
}

size_t TextIndex::Utf16Of(size_t byte_offset) const {
  const size_t wanted = std::min(byte_offset, text_.size());
  // The line whose start is at or before the offset, then a walk along it: the
  // reason for retaining per-line totals.
  const auto found =
      std::upper_bound(line_starts_.begin(), line_starts_.end(), wanted);
  const size_t line = static_cast<size_t>(found - line_starts_.begin()) - 1;
  size_t units = line_utf16_starts_[line];
  size_t at = line_starts_[line];
  while (at < wanted) {
    const auto [width, size] = Step(at);
    at += width;
    units += size;
  }
  return units;
}

size_t TextIndex::ByteOf(size_t utf16_offset) const {
  const auto found = std::upper_bound(line_utf16_starts_.begin(),
                                      line_utf16_starts_.end(), utf16_offset);
  const size_t line =
      static_cast<size_t>(found - line_utf16_starts_.begin()) - 1;
  size_t units = line_utf16_starts_[line];
  size_t at = line_starts_[line];
  while (at < text_.size() && units < utf16_offset) {
    const auto [width, size] = Step(at);
    at += width;
    units += size;
  }
  return at;
}

std::pair<int, int> TextIndex::PositionOf(size_t byte_offset) const {
  const size_t wanted = std::min(byte_offset, text_.size());
  const auto found =
      std::upper_bound(line_starts_.begin(), line_starts_.end(), wanted);
  const size_t line = static_cast<size_t>(found - line_starts_.begin()) - 1;
  size_t units = 0;
  size_t at = line_starts_[line];
  while (at < wanted) {
    const auto [width, size] = Step(at);
    at += width;
    units += size;
  }
  return {static_cast<int>(line), static_cast<int>(units)};
}

size_t TextIndex::ByteOfPosition(int line, int character) const {
  if (line < 0) {
    return 0;
  }
  if (static_cast<size_t>(line) >= line_starts_.size()) {
    return text_.size();
  }
  size_t at = line_starts_[static_cast<size_t>(line)];
  const size_t end = LineEnd(static_cast<size_t>(line));
  size_t units = 0;
  while (at < end && units < static_cast<size_t>(character)) {
    const auto [width, size] = Step(at);
    at += width;
    units += size;
  }
  return at;
}

bool OffsetBasisFromName(std::string_view name, OffsetBasis& basis) {
  if (name.empty() || name == "bytes") {
    basis = OffsetBasis::kBytes;
    return true;
  }
  if (name == "utf16") {
    basis = OffsetBasis::kUtf16;
    return true;
  }
  return false;
}

std::string_view OffsetBasisName(OffsetBasis basis) {
  return basis == OffsetBasis::kUtf16 ? "utf16" : "bytes";
}

void RebaseToUtf16(nlohmann::json& answer, const TextIndex& index) {
  if (answer.is_array()) {
    for (nlohmann::json& item : answer) {
      RebaseToUtf16(item, index);
    }
    return;
  }
  if (!answer.is_object()) {
    return;
  }
  for (auto& [key, value] : answer.items()) {
    if (value.is_number_unsigned() && IsOffsetField(key)) {
      value = index.Utf16Of(value.get<size_t>());
      continue;
    }
    RebaseToUtf16(value, index);
  }
}

}  // namespace a11::flow
