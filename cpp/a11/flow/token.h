// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_TOKEN_H_
#define A11_FLOW_TOKEN_H_

#include <cstddef>
#include <string>
#include <string_view>

#include <absl/time/time.h>

namespace a11::flow {

/// What kind of thing a token is.
///
/// The same set `a11.flow.lexer` produces, under the same names -- [KindName]
/// returns exactly the strings the Python lexer uses -- with one addition: a
/// comment is a token here. The Python lexer drops comments because a parser has
/// no use for them; a highlighter and a formatter do, and one lexer that keeps
/// them serves all three.
enum class TokenKind {
  /// The end of a statement. One per run of blank lines, never leading.
  kNewline,
  /// `# to the end of the line`.
  kComment,
  /// A quoted string. `string_value` holds it with escapes resolved.
  kString,
  /// A number, integral or not. `number` holds it.
  kNumber,
  /// A number with a duration unit: `250ms`. `duration` holds it.
  kDuration,
  /// A bare word. What it *means* is the grammar's business, not the lexer's.
  kWord,

  kDot,
  /// `..` -- the range between two bounds, either of which may be left out.
  kRange,
  /// `...` or `...` -- everything the thing after it holds, spread in here.
  kSpread,
  kArrow,
  kCarry,
  kEqual,
  kEqualEqual,
  kBangEqual,
  kLess,
  kLessEqual,
  kGreater,
  kGreaterEqual,
  kPlus,
  kMinus,
  kPipe,
  kColon,
  kComma,
  kLeftBrace,
  kRightBrace,
  kLeftParen,
  kRightParen,
  kLeftBracket,
  kRightBracket,

  /// A character the language has no meaning for. Carried rather than thrown so
  /// the rest of the file is still read.
  kBad,
  /// One past the last token, so lookahead never runs off the end.
  kEnd,
};

/// One token: what it is, where it is, and what it holds.
///
/// `text` borrows the source, so a token lives no longer than the text it was
/// lexed from -- which is how every caller uses it: lex, use, discard.
struct Token {
  TokenKind kind = TokenKind::kEnd;
  /// The source slice, exactly as written.
  std::string_view text;
  size_t start = 0;
  size_t end = 0;
  /// 1-based, as `a11.flow.lexer` has always reported them.
  int line = 1;
  int column = 1;

  /// A `kString`'s value, with escapes resolved.
  std::string string_value;
  /// A `kNumber`'s value. `is_integer` says whether it was written without a
  /// fractional part, which is what the counted forms of the grammar require.
  double number = 0.0;
  bool is_integer = false;
  /// A `kDuration`'s value.
  absl::Duration duration;

  [[nodiscard]] bool IsWord() const { return kind == TokenKind::kWord; }

  /// Whether a line break, a `}` or the end of the file is here: the three ways
  /// a statement can end.
  [[nodiscard]] bool EndsStatement() const {
    return kind == TokenKind::kNewline || kind == TokenKind::kRightBrace ||
           kind == TokenKind::kEnd;
  }
};

/// The name of a kind, as `a11.flow.lexer` spells it.
///
/// Punctuation is named by itself (`"->"`, `"{"`), which is what makes a token
/// dump comparable between the two implementations token for token.
std::string_view KindName(TokenKind kind);

/// The kind a name from [KindName] refers to, or `kBad` if it is not one.
TokenKind KindFromName(std::string_view name);

}  // namespace a11::flow

#endif  // A11_FLOW_TOKEN_H_
