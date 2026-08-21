// Copyright 2026 The A11 Authors.

#include "a11/flow/lexer.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/time/time.h>

#include "a11/flow/diagnostic.h"
#include "a11/flow/token.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow {
namespace {

bool IsDigit(char letter) {
  return letter >= '0' && letter <= '9';
}

bool IsAsciiLetter(char letter) {
  return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z');
}

// A name may start with a letter, `_` or `$`. Bytes at or above 0x80 count too:
// Python's `str.isalpha` accepts any Unicode letter, so a port named in another
// script lexes the same way there, and treating UTF-8 continuation bytes as name
// characters is the same rule for text that is already valid UTF-8.
bool IsNameStart(char letter) {
  return IsAsciiLetter(letter) || letter == '_' || letter == '$' ||
         static_cast<unsigned char>(letter) >= 0x80;
}

bool IsNamePart(char letter) {
  return IsNameStart(letter) || IsDigit(letter);
}

/// `…` -- U+2026, which the language does **not** spell a spread with.
///
/// It is looked for only to say so. Two ways of writing one operator is two
/// ways for a file to differ from another that means the same thing, and the
/// one that survives a copy through a chat window, a terminal without the font,
/// and a keyboard layout without the key is the one made of three dots. So a
/// `…` is a diagnostic with the repair attached rather than a second spelling.
///
/// It has to be looked for before [IsNameStart], which takes every byte at or
/// above 0x80 for a letter so that a port may be named in any script.
constexpr std::string_view kEllipsis = "…";

/// The lexer's state, kept in one place so the rules read in order.
class Lexer {
 public:
  Lexer(std::string_view source, const LexOptions& options)
      : source_(source), options_(options) {}

  LexResult Run() {
    while (index_ < source_.size()) {
      const char letter = source_[index_];
      if (letter == '\n') {
        // A statement ends at the end of its line. The parser steps over these
        // wherever a break cannot mean that -- inside brackets, before a
        // continuing `|` or `->`.
        if (!result_.tokens.empty() &&
            result_.tokens.back().kind != TokenKind::kNewline) {
          Push(TokenKind::kNewline, index_, index_ + 1);
        }
        ++index_;
        ++line_;
        line_start_ = index_;
        continue;
      }
      if (letter == ' ' || letter == '\t' || letter == '\r') {
        ++index_;
        continue;
      }
      if (letter == '#') {
        const size_t start = index_;
        while (index_ < source_.size() && source_[index_] != '\n') {
          ++index_;
        }
        if (options_.keep_comments) {
          Push(TokenKind::kComment, start, index_);
        }
        continue;
      }
      if (letter == '"') {
        ReadString();
        continue;
      }
      if (IsDigit(letter) || (letter == '-' && index_ + 1 < source_.size() &&
                              IsDigit(source_[index_ + 1]))) {
        ReadNumber();
        continue;
      }
      if (source_.compare(index_, kEllipsis.size(), kEllipsis) == 0) {
        const size_t start = index_;
        Report("flow.syntax.unexpected-character",
               "A spread is written '...'; '…' is not one.", start,
               start + kEllipsis.size());
        // Read as the spread it plainly meant, with the repair travelling on the
        // diagnostic: the rest of the statement is worth checking, and an editor
        // can offer the fix.
        Take(TokenKind::kSpread, start, kEllipsis.size());
        result_.diagnostics.back().fixes.push_back(
            Fix{.label = "Write '...'",
                .edits = {Edit{.start = start,
                               .end = start + kEllipsis.size(),
                               .text = "..."}}});
        continue;
      }
      if (IsNameStart(letter)) {
        ReadWord();
        continue;
      }
      ReadPunctuation();
    }

    // A trailing run of line breaks ends nothing, so it is not there.
    while (!result_.tokens.empty() &&
           result_.tokens.back().kind == TokenKind::kNewline) {
      result_.tokens.pop_back();
    }
    Push(TokenKind::kEnd, index_, index_);
    return std::move(result_);
  }

 private:
  /// The 1-based column of a byte offset, counted in characters.
  ///
  /// Not in bytes: `a11.flow.lexer` reports columns over a Python string, which
  /// is code points, and a flow that holds a `§` in a prompt would otherwise have
  /// every column after it disagree between the two. Continuation bytes
  /// (`10xxxxxx`) are the middles of characters and do not count.
  ///
  /// Tokens are emitted in order, so the scan resumes from the last answer rather
  /// than starting at the line: the common case costs the characters since the
  /// previous token.
  int Column(size_t at) const {
    if (at < counted_at_ || counted_at_ < line_start_) {
      counted_at_ = line_start_;
      counted_column_ = 1;
    }
    while (counted_at_ < at) {
      if ((static_cast<unsigned char>(source_[counted_at_]) & 0xC0) != 0x80) {
        // Count the character being left behind, not the one arrived at.
        ++counted_column_;
      }
      ++counted_at_;
    }
    // A continuation byte is inside a character, so it shares its column.
    return counted_column_;
  }

  Token& Push(TokenKind kind, size_t start, size_t end) {
    Token token;
    token.kind = kind;
    token.start = start;
    token.end = end;
    token.text = source_.substr(start, end - start);
    token.line = line_;
    token.column = Column(start);
    result_.tokens.push_back(std::move(token));
    return result_.tokens.back();
  }

  void Report(std::string_view code, std::string message, size_t start,
              size_t end, Family family = Family::kSyntax) {
    Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = Severity::kError;
    diagnostic.family = family;
    diagnostic.message = std::move(message);
    diagnostic.range.start = Position{start, line_, Column(start)};
    diagnostic.range.end = Position{end, line_, Column(end)};
    result_.diagnostics.push_back(std::move(diagnostic));
  }

  /// The value of a `"""..."""` string: the text between the delimiters, with
  /// the indentation the source put in front of it taken back off.
  ///
  /// The rule, in the order it applies: a first line that is blank goes away, a
  /// last line that is only whitespace goes away with the break above it, and the
  /// indentation every remaining line shares is removed from all of them. That is
  /// what lets a long description sit at the indentation of the flow it describes
  /// and still read as prose -- which is the whole reason to have the form.
  ///
  /// Escapes are resolved afterwards, so a `\n` written by hand is a line break in
  /// the value and never an indented line to be dedented.
  static std::string Dedent(std::string_view inner) {
    std::vector<std::string_view> lines;
    size_t at = 0;
    while (at <= inner.size()) {
      const size_t next = inner.find('\n', at);
      if (next == std::string_view::npos) {
        lines.push_back(inner.substr(at));
        break;
      }
      lines.push_back(inner.substr(at, next - at));
      at = next + 1;
    }
    const auto blank = [](std::string_view line) {
      return line.find_first_not_of(" \t\r") == std::string_view::npos;
    };
    if (lines.size() > 1 && blank(lines.front())) {
      lines.erase(lines.begin());
    }
    if (lines.size() > 1 && blank(lines.back())) {
      lines.pop_back();
    }

    size_t common = std::string_view::npos;
    for (const std::string_view line : lines) {
      if (blank(line)) {
        continue;
      }
      common = std::min(common, line.find_first_not_of(" \t"));
    }
    if (common == std::string_view::npos) {
      common = 0;
    }

    std::string value;
    for (size_t index = 0; index < lines.size(); ++index) {
      if (index != 0) {
        value.push_back('\n');
      }
      const std::string_view line = lines[index];
      value.append(line.size() > common ? line.substr(common) : "");
    }
    return value;
  }

  /// Escapes resolved, the way a single-line string resolves them.
  static std::string Unescape(std::string_view text) {
    std::string value;
    value.reserve(text.size());
    for (size_t at = 0; at < text.size(); ++at) {
      if (text[at] != '\\' || at + 1 >= text.size()) {
        value.push_back(text[at]);
        continue;
      }
      switch (const char escaped = text[++at]) {
        case 'n':
          value.push_back('\n');
          break;
        case 't':
          value.push_back('\t');
          break;
        case 'r':
          value.push_back('\r');
          break;
        default:
          value.push_back(escaped);
          break;
      }
    }
    return value;
  }

  /// `"""..."""`: a string that may hold line breaks.
  ///
  /// Three quotes rather than allowing a break inside a single-quoted string,
  /// because a missing quote then costs one line rather than the rest of the file
  /// -- the same reason Python draws the line in the same place.
  void ReadMultilineString() {
    const size_t start = index_;
    const int start_line = line_;
    const size_t start_line_start = line_start_;
    index_ += 3;
    const size_t inner_start = index_;
    size_t inner_end = 0;
    bool terminated = false;
    while (index_ < source_.size()) {
      if (source_[index_] == '\\' && index_ + 1 < source_.size()) {
        index_ += 2;
        continue;
      }
      if (source_.compare(index_, 3, R"(""")") == 0) {
        inner_end = index_;
        index_ += 3;
        terminated = true;
        break;
      }
      if (source_[index_] == '\n') {
        ++line_;
        line_start_ = index_ + 1;
      }
      ++index_;
    }
    if (!terminated) {
      // Recovery: it ends where the file does. There is nothing further along to
      // mistake for the rest of it.
      inner_end = index_;
      Report("flow.syntax.unterminated-string", R"(Unterminated """ string.)",
             start, index_);
    }
    // The token is reported at the line it *started* on, as every other token is,
    // so the position is wound back for the push and then forward again: what
    // follows the closing quotes is on the line the scan ended up at.
    const int ending_line = line_;
    const size_t ending_line_start = line_start_;
    line_ = start_line;
    line_start_ = start_line_start;
    Token& token = Push(TokenKind::kString, start, index_);
    token.string_value =
        Unescape(Dedent(source_.substr(inner_start, inner_end - inner_start)));
    line_ = ending_line;
    line_start_ = ending_line_start;
  }

  void ReadString() {
    if (source_.compare(index_, 3, R"(""")") == 0) {
      return ReadMultilineString();
    }
    const size_t start = index_;
    ++index_;
    std::string value;
    bool terminated = false;
    while (index_ < source_.size()) {
      const char letter = source_[index_];
      if (letter == '\n') {
        break;
      }
      if (letter == '"') {
        ++index_;
        terminated = true;
        break;
      }
      if (letter == '\\') {
        ++index_;
        if (index_ >= source_.size()) {
          break;
        }
        switch (const char escaped = source_[index_]) {
          case 'n':
            value.push_back('\n');
            break;
          case 't':
            value.push_back('\t');
            break;
          case 'r':
            value.push_back('\r');
            break;
          default:
            // An unknown escape is the character itself, which is what the
            // Python lexer does: `\"` and `\\` come out of this too.
            value.push_back(escaped);
            break;
        }
        ++index_;
        continue;
      }
      value.push_back(letter);
      ++index_;
    }
    if (!terminated) {
      // Recovery: the string ends where its line does. Anything else -- reading
      // to the next quote three lines down -- swallows real statements.
      Report("flow.syntax.unterminated-string", "Unterminated string.", start,
             index_);
    }
    Push(TokenKind::kString, start, index_).string_value = std::move(value);
  }

  void ReadNumber() {
    const size_t start = index_;
    ++index_;
    bool fractional = false;
    while (index_ < source_.size() &&
           (IsDigit(source_[index_]) || source_[index_] == '.')) {
      if (source_[index_] == '.') {
        // `1..200` is a range of two whole numbers, not a number with two
        // decimal points in it. A `.` only continues the number when it is not
        // the first of a `..`.
        if (index_ + 1 < source_.size() && source_[index_ + 1] == '.') {
          break;
        }
        fractional = true;
      }
      ++index_;
    }
    const size_t number_end = index_;
    const std::string_view number_text =
        source_.substr(start, number_end - start);

    const size_t unit_start = index_;
    while (index_ < source_.size() && IsAsciiLetter(source_[index_])) {
      ++index_;
    }
    const std::string_view unit =
        source_.substr(unit_start, index_ - unit_start);

    double value = 0.0;
    if (!absl::SimpleAtod(number_text, &value)) {
      Report("flow.form.bad-number",
             absl::StrCat("Bad number '", number_text, "'."), start, number_end,
             Family::kForm);
    }

    if (unit.empty()) {
      Token& token = Push(TokenKind::kNumber, start, number_end);
      token.number = value;
      token.is_integer = !fractional;
      return;
    }

    const std::optional<double> seconds = vocabulary::DurationUnitSeconds(unit);
    if (!seconds.has_value()) {
      Report(
          "flow.form.duration-unit",
          absl::StrCat("Unknown duration unit '", unit, "' (use ",
                       absl::StrJoin(vocabulary::DurationUnits(), ", "), ")."),
          unit_start, index_, Family::kForm);
      // Recovery: one bad token over the whole thing. Splitting it into a number
      // and a name would read as two things the author did not write.
      Push(TokenKind::kBad, start, index_);
      return;
    }
    Token& token = Push(TokenKind::kDuration, start, index_);
    token.number = value;
    token.is_integer = !fractional;
    token.duration = absl::Seconds(value * *seconds);
  }

  void ReadWord() {
    const size_t start = index_;
    ++index_;
    while (index_ < source_.size()) {
      if (IsNamePart(source_[index_])) {
        ++index_;
        continue;
      }
      // A dash continues a name only when a word follows it, which is what keeps
      // `starts-with` one name and `a -> b` a pipe.
      if (source_[index_] == '-' && index_ + 1 < source_.size() &&
          IsNamePart(source_[index_ + 1])) {
        index_ += 2;
        continue;
      }
      break;
    }
    Push(TokenKind::kWord, start, index_);
  }

  void ReadPunctuation() {
    const size_t start = index_;
    const char letter = source_[index_];
    const char next = index_ + 1 < source_.size() ? source_[index_ + 1] : '\0';

    // Longest first, so `...` wins over `..` wins over `.`, `->` over `-` and
    // `<=` over `<`.
    if (letter == '.' && next == '.') {
      const bool third =
          index_ + 2 < source_.size() && source_[index_ + 2] == '.';
      return Take(third ? TokenKind::kSpread : TokenKind::kRange, start,
                  third ? 3 : 2);
    }
    if (letter == '-' && next == '>') {
      return Take(TokenKind::kArrow, start, 2);
    }
    if (letter == '<' && next == '-') {
      return Take(TokenKind::kCarry, start, 2);
    }
    if (letter == '=' && next == '=') {
      return Take(TokenKind::kEqualEqual, start, 2);
    }
    if (letter == '!' && next == '=') {
      return Take(TokenKind::kBangEqual, start, 2);
    }
    if (letter == '<' && next == '=') {
      return Take(TokenKind::kLessEqual, start, 2);
    }
    if (letter == '>' && next == '=') {
      return Take(TokenKind::kGreaterEqual, start, 2);
    }

    switch (letter) {
      case '.':
        return Take(TokenKind::kDot, start, 1);
      case '{':
        return Take(TokenKind::kLeftBrace, start, 1);
      case '}':
        return Take(TokenKind::kRightBrace, start, 1);
      case '(':
        return Take(TokenKind::kLeftParen, start, 1);
      case ')':
        return Take(TokenKind::kRightParen, start, 1);
      case '[':
        return Take(TokenKind::kLeftBracket, start, 1);
      case ']':
        return Take(TokenKind::kRightBracket, start, 1);
      case ':':
        return Take(TokenKind::kColon, start, 1);
      case ',':
        return Take(TokenKind::kComma, start, 1);
      case '|':
        return Take(TokenKind::kPipe, start, 1);
      case '=':
        return Take(TokenKind::kEqual, start, 1);
      case '<':
        return Take(TokenKind::kLess, start, 1);
      case '>':
        return Take(TokenKind::kGreater, start, 1);
      case '+':
        return Take(TokenKind::kPlus, start, 1);
      case '-':
        return Take(TokenKind::kMinus, start, 1);
      default:
        break;
    }
    Report("flow.syntax.unexpected-character",
           absl::StrCat("Unexpected character '", std::string(1, letter), "'."),
           start, start + 1);
    Take(TokenKind::kBad, start, 1);
  }

  void Take(TokenKind kind, size_t start, size_t length) {
    index_ = start + length;
    Push(kind, start, index_);
  }

  std::string_view source_;
  LexOptions options_;
  LexResult result_;
  size_t index_ = 0;
  size_t line_start_ = 0;
  int line_ = 1;
  /// Where [Column]'s scan got to, and what it counted: see the comment there.
  mutable size_t counted_at_ = 0;
  mutable int counted_column_ = 1;
};

}  // namespace

bool LexResult::HasErrors() const {
  for (const Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == Severity::kError) {
      return true;
    }
  }
  return false;
}

LexResult Lex(std::string_view source, LexOptions options) {
  return Lexer(source, options).Run();
}

std::string_view KindName(TokenKind kind) {
  switch (kind) {
    case TokenKind::kNewline:
      return "newline";
    case TokenKind::kComment:
      return "comment";
    case TokenKind::kString:
      return "string";
    case TokenKind::kNumber:
      return "number";
    case TokenKind::kDuration:
      return "duration";
    case TokenKind::kWord:
      return "word";
    case TokenKind::kDot:
      return ".";
    case TokenKind::kRange:
      return "..";
    case TokenKind::kSpread:
      return "...";
    case TokenKind::kArrow:
      return "->";
    case TokenKind::kCarry:
      return "<-";
    case TokenKind::kEqual:
      return "=";
    case TokenKind::kEqualEqual:
      return "==";
    case TokenKind::kBangEqual:
      return "!=";
    case TokenKind::kLess:
      return "<";
    case TokenKind::kLessEqual:
      return "<=";
    case TokenKind::kGreater:
      return ">";
    case TokenKind::kGreaterEqual:
      return ">=";
    case TokenKind::kPlus:
      return "+";
    case TokenKind::kMinus:
      return "-";
    case TokenKind::kPipe:
      return "|";
    case TokenKind::kColon:
      return ":";
    case TokenKind::kComma:
      return ",";
    case TokenKind::kLeftBrace:
      return "{";
    case TokenKind::kRightBrace:
      return "}";
    case TokenKind::kLeftParen:
      return "(";
    case TokenKind::kRightParen:
      return ")";
    case TokenKind::kLeftBracket:
      return "[";
    case TokenKind::kRightBracket:
      return "]";
    case TokenKind::kBad:
      return "bad";
    case TokenKind::kEnd:
      return "end";
  }
  return "bad";
}

TokenKind KindFromName(std::string_view name) {
  static constexpr TokenKind kAll[] = {
      TokenKind::kNewline,      TokenKind::kComment,
      TokenKind::kString,       TokenKind::kNumber,
      TokenKind::kDuration,     TokenKind::kWord,
      TokenKind::kDot,          TokenKind::kRange,
      TokenKind::kSpread,       TokenKind::kArrow,
      TokenKind::kCarry,        TokenKind::kEqual,
      TokenKind::kEqualEqual,   TokenKind::kBangEqual,
      TokenKind::kLess,         TokenKind::kLessEqual,
      TokenKind::kGreater,      TokenKind::kGreaterEqual,
      TokenKind::kPlus,         TokenKind::kMinus,
      TokenKind::kPipe,         TokenKind::kColon,
      TokenKind::kComma,        TokenKind::kLeftBrace,
      TokenKind::kRightBrace,   TokenKind::kLeftParen,
      TokenKind::kRightParen,   TokenKind::kLeftBracket,
      TokenKind::kRightBracket, TokenKind::kEnd,
  };
  for (const TokenKind kind : kAll) {
    if (KindName(kind) == name) {
      return kind;
    }
  }
  return TokenKind::kBad;
}

}  // namespace a11::flow
