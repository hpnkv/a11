// Copyright 2026 The A11 Authors.

#include "a11/flow/pattern.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>

namespace a11::flow::pattern {
namespace {

// absl's predicates take an `unsigned char`; the casts are the conversion the
// standard library asks for rather than anything about the character.
bool IsSpace(char letter) {
  return absl::ascii_isspace(static_cast<unsigned char>(letter));
}

bool IsDigit(char letter) {
  return absl::ascii_isdigit(static_cast<unsigned char>(letter));
}

bool IsAlnum(char letter) {
  return absl::ascii_isalnum(static_cast<unsigned char>(letter));
}

/// How much horizontal whitespace the subject has at `at`.
size_t FlatRun(std::string_view subject, size_t at) {
  size_t length = 0;
  while (at + length < subject.size() && IsSpace(subject[at + length]) &&
         subject[at + length] != '\n') {
    ++length;
  }
  return length;
}

/// Match one literal at `at`, or nothing.
///
/// A run of spaces or tabs in the pattern matches any run of them in the
/// subject,
/// which is what makes a hand-written pattern survive the two spaces somebody's
/// log writer used. A **line break in the pattern** matches a line break in the
/// subject, with horizontal space allowed either side: crossing to the next
/// line
/// is something a pattern has to say, or `a={a} b={b}` would quietly match
/// `a=1`
/// on one line and `b=2` on the next. Everything else matches itself, exactly.
std::optional<size_t> MatchLiteral(std::string_view literal,
                                   std::string_view subject, size_t at) {
  size_t index = 0;
  while (index < literal.size()) {
    if (IsSpace(literal[index])) {
      bool breaks = false;
      while (index < literal.size() && IsSpace(literal[index])) {
        if (literal[index] == '\n') {
          breaks = true;
        }
        ++index;
      }
      const size_t before = at;
      at += FlatRun(subject, at);
      if (breaks) {
        if (at >= subject.size() || subject[at] != '\n') {
          return std::nullopt;
        }
        ++at;
        at += FlatRun(subject, at);
      } else if (at == before) {
        return std::nullopt;
      }
      continue;
    }
    if (at >= subject.size() || subject[at] != literal[index]) {
      return std::nullopt;
    }
    ++index;
    ++at;
  }
  return at;
}

/// The end of the line `at` is on.
size_t LineEnd(std::string_view subject, size_t at) {
  const size_t found = subject.find('\n', at);
  return found == std::string_view::npos ? subject.size() : found;
}

/// What a hole's type says about the text it may take.
enum class Shape {
  /// Nothing: bounded by whatever literal follows it.
  kOpen,
  /// Exactly what its type is. Where that does not fit, the hole does not
  /// match here at all -- `{n:int}` against `none` is not a number, and taking
  /// `n` for it would be answering a question nobody asked.
  kExact,
  /// A run it may give some of back, so `{level:word}` can stop before the `:`
  /// that follows it.
  kRun,
};

Shape ShapeOf(HoleType type) {
  switch (type) {
    case HoleType::kInt:
    case HoleType::kNumber:
    case HoleType::kBool:
      return Shape::kExact;
    case HoleType::kWord:
    case HoleType::kLine:
    case HoleType::kRest:
      return Shape::kRun;
    case HoleType::kString:
    case HoleType::kDuration:
    case HoleType::kTime:
    case HoleType::kJson:
      return Shape::kOpen;
  }
  return Shape::kOpen;
}

/// How far a typed hole reaches from `at` on its own, or nothing where its type
/// says nothing about the shape of the text -- or where it says something the
/// text at `at` is not.
std::optional<size_t> TypedRun(HoleType type, std::string_view subject,
                               size_t at) {
  switch (type) {
    case HoleType::kRest:
      return subject.size();
    case HoleType::kLine:
      return LineEnd(subject, at);
    case HoleType::kWord: {
      size_t end = at;
      while (end < subject.size() && !IsSpace(subject[end])) {
        ++end;
      }
      return end;
    }
    case HoleType::kInt:
    case HoleType::kNumber: {
      size_t end = at;
      if (end < subject.size() &&
          (subject[end] == '-' || subject[end] == '+')) {
        ++end;
      }
      const size_t digits = end;
      while (end < subject.size() && IsDigit(subject[end])) {
        ++end;
      }
      if (end == digits) {
        return std::nullopt;
      }
      if (type == HoleType::kNumber && end < subject.size() &&
          subject[end] == '.') {
        const size_t point = end;
        ++end;
        const size_t after = end;
        while (end < subject.size() && IsDigit(subject[end])) {
          ++end;
        }
        // A trailing point is not part of the number: `1.` in "1. two" is the
        // number and the full stop, which is what a reader means by it.
        if (end == after) {
          end = point;
        }
      }
      return end;
    }
    case HoleType::kBool: {
      for (const std::string_view word : {"true", "false"}) {
        if (absl::StartsWithIgnoreCase(subject.substr(at), word)) {
          return at + word.size();
        }
      }
      return std::nullopt;
    }
    case HoleType::kString:
    case HoleType::kDuration:
    case HoleType::kTime:
    case HoleType::kJson:
      // Bounded by what follows rather than by their own shape.
      return std::nullopt;
  }
  return std::nullopt;
}

/// Match the pieces from `piece` onwards, starting at `at`.
///
/// Recursive because a hole's end is only known once what follows it has
/// matched.
/// Which candidate ends are tried, and in what order, is the whole behaviour of
/// the language:
///
/// * an **exact** hole takes what its type is, and if that is not there the
/// hole
///   does not match here;
/// * a **run** hole takes its whole run and then gives characters back one at a
///   time, so `{level:word}` stops before the `:` that follows it;
/// * an **open** hole takes as little as it can, growing until what follows
/// fits
/// -- except as the last piece, where there is nothing to bound it and it takes
///   the rest of its line.
bool MatchFrom(const Pattern& pattern, size_t piece, std::string_view subject,
               size_t at, std::vector<Capture>& captures) {
  if (piece >= pattern.pieces.size()) {
    return true;
  }
  const Pattern::Piece& one = pattern.pieces[piece];

  if (!one.is_hole) {
    const std::optional<size_t> next = MatchLiteral(one.literal, subject, at);
    if (!next.has_value()) {
      return false;
    }
    return MatchFrom(pattern, piece + 1, subject, *next, captures);
  }

  const auto take = [&](size_t end) {
    captures.push_back(Capture{subject.substr(at, end - at)});
    if (MatchFrom(pattern, piece + 1, subject, end, captures)) {
      return true;
    }
    captures.pop_back();
    return false;
  };

  const Shape shape = ShapeOf(one.hole.type);
  const std::optional<size_t> run = TypedRun(one.hole.type, subject, at);
  if (shape == Shape::kExact) {
    if (!run.has_value() || *run <= at) {
      return false;
    }
    return take(*run);
  }
  if (shape == Shape::kRun) {
    if (!run.has_value() || *run <= at) {
      return false;
    }
    // Longest first, giving back one character at a time.
    for (size_t end = *run; end > at; --end) {
      if (take(end)) {
        return true;
      }
    }
    return false;
  }

  // Open: bounded by what comes after it, and by the end of the line -- a hole
  // that swallowed a line break would match half of the next record.
  const size_t stop = LineEnd(subject, at);
  if (piece + 1 >= pattern.pieces.size()) {
    // Nothing follows, so there is nothing to grow towards: the rest of the
    // line
    // is what "and then the value" means.
    return stop > at && take(stop);
  }
  for (size_t end = at + 1; end <= stop; ++end) {
    if (take(end)) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::string_view HoleTypeName(HoleType type) {
  switch (type) {
    case HoleType::kString:
      return "string";
    case HoleType::kInt:
      return "int";
    case HoleType::kNumber:
      return "number";
    case HoleType::kBool:
      return "bool";
    case HoleType::kWord:
      return "word";
    case HoleType::kLine:
      return "line";
    case HoleType::kRest:
      return "rest";
    case HoleType::kDuration:
      return "duration";
    case HoleType::kTime:
      return "time";
    case HoleType::kJson:
      return "json";
  }
  return "string";
}

std::optional<HoleType> HoleTypeFromName(std::string_view name) {
  static constexpr HoleType kAll[] = {
      HoleType::kString, HoleType::kInt,      HoleType::kNumber,
      HoleType::kBool,   HoleType::kWord,     HoleType::kLine,
      HoleType::kRest,   HoleType::kDuration, HoleType::kTime,
      HoleType::kJson,
  };
  for (const HoleType type : kAll) {
    if (HoleTypeName(type) == name) {
      return type;
    }
  }
  return std::nullopt;
}

bool Pattern::AllNamed() const {
  for (const Hole& hole : holes) {
    if (hole.name.empty()) {
      return false;
    }
  }
  return !holes.empty();
}

Compiled Compile(std::string_view text) {
  Compiled out;
  std::string literal;
  absl::flat_hash_set<std::string> named;
  const auto flush = [&] {
    if (literal.empty()) {
      return;
    }
    Pattern::Piece piece;
    piece.literal = std::move(literal);
    literal.clear();
    out.pattern.pieces.push_back(std::move(piece));
  };
  const auto fail = [&](std::string why, size_t column) {
    out.error = std::move(why);
    out.column = column;
    return out;
  };

  size_t at = 0;
  while (at < text.size()) {
    const char letter = text[at];
    if (letter == '{' && at + 1 < text.size() && text[at + 1] == '{') {
      literal.push_back('{');
      at += 2;
      continue;
    }
    if (letter == '}' && at + 1 < text.size() && text[at + 1] == '}') {
      literal.push_back('}');
      at += 2;
      continue;
    }
    if (letter == '}') {
      return fail("A '}' with no '{' before it. Write '}}' for a literal one.",
                  at);
    }
    if (letter != '{') {
      literal.push_back(letter);
      ++at;
      continue;
    }
    const size_t opened = at;
    const size_t closes = text.find('}', at + 1);
    if (closes == std::string_view::npos) {
      return fail("A '{' with no '}' after it. Write '{{' for a literal one.",
                  opened);
    }
    const std::string_view inside = text.substr(at + 1, closes - at - 1);
    at = closes + 1;

    Pattern::Piece piece;
    piece.is_hole = true;
    piece.hole.position = out.pattern.holes.size();
    const size_t colon = inside.find(':');
    const std::string_view name =
        colon == std::string_view::npos ? inside : inside.substr(0, colon);
    if (colon != std::string_view::npos) {
      const std::string_view spelled = inside.substr(colon + 1);
      const std::optional<HoleType> type = HoleTypeFromName(spelled);
      if (!type.has_value()) {
        return fail(
            absl::StrCat("'", spelled,
                         "' is not one of the kinds a hole may be read as: "
                         "string, int, number, bool, word, line, rest, "
                         "duration, time, json."),
            opened);
      }
      piece.hole.type = *type;
    }
    if (!name.empty()) {
      for (const char inner : name) {
        if (!IsAlnum(inner) && inner != '_' && inner != '-') {
          return fail(
              absl::StrCat("'", name,
                           "' is not a field name: a hole is named with "
                           "letters, digits, '_' and '-'."),
              opened);
        }
      }
      if (!named.insert(std::string(name)).second) {
        return fail(absl::StrCat("Two holes are both called '", name,
                                 "', so one would overwrite the other."),
                    opened);
      }
      piece.hole.name = std::string(name);
    }
    flush();
    out.pattern.holes.push_back(piece.hole);
    out.pattern.pieces.push_back(std::move(piece));
  }
  flush();

  if (out.pattern.holes.empty()) {
    return fail(
        "A pattern with no '{...}' captures nothing, so there is nothing to "
        "match it for.",
        0);
  }
  // `rest` runs to the end of the subject, so anything written after it could
  // never match. Saying so beats a pattern that silently never fits.
  for (size_t index = 0; index + 1 < out.pattern.pieces.size(); ++index) {
    const Pattern::Piece& piece = out.pattern.pieces[index];
    if (piece.is_hole && piece.hole.type == HoleType::kRest) {
      return fail(
          "A 'rest' hole takes everything left, so nothing can follow it.", 0);
    }
  }
  return out;
}

std::optional<std::vector<Capture>> Match(const Pattern& pattern,
                                          std::string_view subject) {
  if (pattern.pieces.empty()) {
    return std::nullopt;
  }
  std::vector<Capture> captures;
  // Searching, so the pattern may begin anywhere: the earliest fit wins, which
  // is the one a reader means by "the line has this in it".
  for (size_t start = 0; start <= subject.size(); ++start) {
    captures.clear();
    if (MatchFrom(pattern, 0, subject, start, captures)) {
      return captures;
    }
  }
  return std::nullopt;
}

}  // namespace a11::flow::pattern
