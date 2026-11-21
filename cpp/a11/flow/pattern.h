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

#ifndef A11_FLOW_PATTERN_H_
#define A11_FLOW_PATTERN_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace a11::flow::pattern {

/// Literal text and named holes used by `match`, such as `age={age:int}`.
/// Matches search the input. Holes capture nonempty text and stay on one line
/// unless their type permits line breaks. Captures retain text for decoding.

/// What a hole reads its text as, and how much of it a hole may take.
enum class HoleType {
  /// Anything up to the next literal, on one line. The default.
  kString,
  /// A whole number, with an optional sign.
  kInt,
  /// A number, with an optional sign and fraction.
  kNumber,
  /// `true` or `false`, in either case.
  kBool,
  /// A run of anything that is not whitespace.
  kWord,
  /// The rest of the line.
  kLine,
  /// Everything left, line breaks and all. Only as the last piece.
  kRest,
  /// Text to be read as a duration: `250ms`, `1m30s`.
  kDuration,
  /// Text to be read as an instant: RFC 3339, or a bare date.
  kTime,
  /// Text to be read as JSON.
  kJson,
};

std::string_view HoleTypeName(HoleType type);

/// The type that name spells, or nothing where it is not one of them.
std::optional<HoleType> HoleTypeFromName(std::string_view name);

/// One capture position in a pattern.
struct Hole {
  /// The field it fills. Empty for `{}`, which is read by position.
  std::string name;
  HoleType type = HoleType::kString;
  /// Which hole this is, counting from zero, which is what `{}` is read by.
  size_t position = 0;
};

/// A compiled pattern: literals and holes, in the order they were written.
struct Pattern {
  struct Piece {
    /// A hole when true, and otherwise a literal to match.
    bool is_hole = false;
    /// The literal's text. A run of whitespace in it matches any run.
    std::string literal;
    Hole hole;
  };

  std::vector<Piece> pieces;
  /// Every hole, in order. What a shape is built from.
  std::vector<Hole> holes;

  /// Whether every hole has a name, which is what makes the result a record
  /// with fields rather than one read by position.
  [[nodiscard]] bool AllNamed() const;
};

/// A pattern, or what is wrong with the text it was written as.
struct Compiled {
  Pattern pattern;
  /// Empty when it compiled. Otherwise what is wrong, as a sentence.
  std::string error;
  /// Where the problem is, counting characters from the start of the pattern.
  size_t column = 0;

  [[nodiscard]] bool ok() const { return error.empty(); }
};

/// Compile a pattern, saying what is wrong rather than throwing.
Compiled Compile(std::string_view text);

/// What one hole took: a view into the subject that was matched.
struct Capture {
  std::string_view text;
  size_t start = 0;
  size_t end = 0;
};

struct MatchResult {
  size_t start = 0;
  size_t end = 0;
  std::vector<Capture> captures;
};

/// Match `pattern` anywhere in `subject`, taking the earliest fit.
///
/// Nothing when it does not fit at all, which is how a stage knows to drop the
/// value and a function knows to answer null.
std::optional<MatchResult> MatchWithSpans(const Pattern& pattern,
                                          std::string_view subject);
std::optional<std::vector<Capture>> Match(const Pattern& pattern,
                                          std::string_view subject);

}  // namespace a11::flow::pattern

#endif  // A11_FLOW_PATTERN_H_
