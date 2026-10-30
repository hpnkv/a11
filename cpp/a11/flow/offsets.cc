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

#include "a11/flow/offsets.h"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

#include "a11/utf8.h"

namespace a11::flow {
namespace {

/// The four names a `flow.*` envelope uses for a byte offset into the document.
constexpr std::string_view kOffsetFields[] = {"start", "end", "offset",
                                              "prefix_start", "replace_start"};

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

size_t TextIndex::ByteOfSourcePosition(int line, int column) const {
  const size_t row = static_cast<size_t>(line > 0 ? line - 1 : 0);
  if (row >= line_starts_.size()) {
    return text_.size();
  }
  size_t offset = line_starts_[row];
  const size_t end = LineEnd(row);
  for (int character = 1; character < column && offset < end; ++character) {
    offset += Step(offset).first;
  }
  return std::min(offset, end);
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
  RebaseToUtf16(answer, index, {});
}

void RebaseToUtf16(
    nlohmann::json& answer, const TextIndex& inherited,
    const absl::flat_hash_map<std::string, TextIndex>& documents) {
  const TextIndex* index = &inherited;
  if (answer.is_object()) {
    if (const auto source = answer.find("source");
        source != answer.end() && source->is_string()) {
      if (const auto found = documents.find(source->get<std::string>());
          found != documents.end()) {
        index = &found->second;
      }
    }
  }
  if (answer.is_object() && answer.contains("file") &&
      answer["file"].is_string()) {
    if (const auto found = documents.find(answer["file"].get<std::string>());
        found != documents.end()) {
      index = &found->second;
      if (answer.contains("line") && answer["line"].is_number_integer() &&
          answer.contains("column") && answer["column"].is_number_integer()) {
        const auto offset = index->ByteOfSourcePosition(
            answer["line"].get<int>(), answer["column"].get<int>());
        answer["utf16_column"] = index->PositionOf(offset).second + 1;
      }
    }
  }
  if (answer.is_array()) {
    for (auto& item : answer) {
      RebaseToUtf16(item, *index, documents);
    }
    return;
  }
  if (!answer.is_object()) {
    return;
  }
  if (auto offset = answer.find("offset");
      offset != answer.end() && offset->is_number_unsigned() &&
      answer.contains("line") && answer.contains("column")) {
    const auto position = index->PositionOf(offset->get<size_t>());
    answer["utf16_column"] = position.second + 1;
  }
  for (auto& [key, value] : answer.items()) {
    if (value.is_number_unsigned() && IsOffsetField(key)) {
      value = index->Utf16Of(value.get<size_t>());
    } else {
      RebaseToUtf16(value, *index, documents);
    }
  }
}

}  // namespace a11::flow
