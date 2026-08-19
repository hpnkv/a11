// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_PATTERN_H_
#define A11_FLOW_PATTERN_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace a11::flow::pattern {

/// The pattern language `match` reads: literal text and named holes.
///
/// **Why not a regular expression.** A flow is text somebody writes in the
/// moment, often a model, and the thing being written is almost always "this
/// line, with these bits pulled out of it". Regex answers that with
/// `name.*=.*(?<name>[^ ]+)\s+age.*=.*(?<age>\d+)`, which is unreadable at a
/// glance and wrong in three ways before it is right. The same intent here is
/// `name={name} age={age:int}`, and there is nothing to escape.
///
/// | In the pattern | Means |
/// | --- | --- |
/// | literal text | matches itself |
/// | a run of whitespace | matches any run of whitespace, at least one |
/// | `{name}` | captures up to whatever literal follows, as little as it can |
/// | `{name:int}` | and reads it as an integer |
/// | `{}` | captures without a name, read as `it[0]`, `it[1]`, ... |
/// | `{{`, `}}` | a literal brace |
///
/// The pattern **searches**: it matches anywhere in the value, so
/// `match("age={age:int}", line)` works on a longer line without leading and
/// trailing wildcards. A hole captures at least one character and does not cross
/// a line break unless its type says to.
///
/// This half knows nothing about [Value]: it compiles a pattern, says what its
/// holes are called, and hands back the *text* each one took. Reading that text
/// as a number, an instant or a duration is the value layer's job, and keeping
/// the split is what lets the resolver ask a pattern for its field names without
/// the runtime being linked in.

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
  bool AllNamed() const;
};

/// A pattern, or what is wrong with the text it was written as.
struct Compiled {
  Pattern pattern;
  /// Empty when it compiled. Otherwise what is wrong, as a sentence.
  std::string error;
  /// Where the problem is, counting characters from the start of the pattern.
  size_t column = 0;

  bool ok() const { return error.empty(); }
};

/// Compile a pattern, saying what is wrong rather than throwing.
Compiled Compile(std::string_view text);

/// What one hole took: a view into the subject that was matched.
struct Capture {
  std::string_view text;
};

/// Match `pattern` anywhere in `subject`, taking the earliest fit.
///
/// Nothing when it does not fit at all, which is how a stage knows to drop the
/// value and a function knows to answer null.
std::optional<std::vector<Capture>> Match(const Pattern& pattern,
                                          std::string_view subject);

}  // namespace a11::flow::pattern

#endif  // A11_FLOW_PATTERN_H_
