// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_LEXER_H_
#define A11_FLOW_LEXER_H_

#include <string_view>
#include <vector>

#include "a11/flow/diagnostic.h"
#include "a11/flow/token.h"

namespace a11::flow {

/// What to keep while lexing.
struct LexOptions {
  /// Whether comments are tokens. On for a highlighter and a formatter, off for
  /// a parser, which has no use for them.
  ///
  /// With them off the stream is exactly what `a11.flow.lexer` produces, line
  /// breaks included -- which means a line holding only a comment ends nothing,
  /// because with the comment gone there was no statement on it.
  bool keep_comments = true;
};

/// Tokens, and what could not be read.
struct LexResult {
  std::vector<Token> tokens;
  std::vector<Diagnostic> diagnostics;

  /// Whether anything is an error, which is what the strict entry points check.
  bool HasErrors() const;
};

/// Turn Flow source into tokens, ending with a single `end` token.
///
/// The rules are `a11/flow/lexer.py`'s: `#` to the end of the line is a comment, a
/// string cannot span a line, a number may carry a duration unit, and a dash
/// continues a name only between word characters -- which is what keeps `-3` a
/// number, `starts-with` one name, and `a -> b` a pipe. A line break is a token,
/// because the grammar is one statement per line; a run of them is one token, and
/// a leading one is none.
///
/// **This never fails.** The Python lexer raises on the first unterminated string
/// or unknown character, because a program that cannot be read cannot run. An
/// editor is looking at a file somebody is in the middle of typing, so a problem
/// here becomes a diagnostic and lexing carries on: an unterminated string ends at
/// its line, an unknown character is one `kBad` token, and the tokens after it are
/// still there. `Compile` in `parser.h` is where a first error becomes a refusal.
LexResult Lex(std::string_view source, LexOptions options = {});

}  // namespace a11::flow

#endif  // A11_FLOW_LEXER_H_
