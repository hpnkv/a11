// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_PARSER_H_
#define A11_FLOW_PARSER_H_

#include <string_view>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/types/span.h>

#include "a11/flow/diagnostic.h"
#include "a11/flow/syntax.h"

namespace a11::flow {

/// The flows a file declares, and everything wrong with it.
struct ParseResult {
  std::vector<syntax::FlowDeclarationPtr> flows;
  std::vector<Diagnostic> diagnostics;

  /// The offset of every `{` that opens a *value* rather than a block, sorted.
  ///
  /// The one thing about the token stream that cannot be worked out from the
  /// tokens: `flow research {` and `a11.sdk.Interaction{` are a word followed by a
  /// brace either way, and only the grammar knows which. The formatter needs it to
  /// know whether a brace opens a line or a literal, so the parser -- which knew
  /// at the time -- writes it down.
  std::vector<size_t> value_braces;

  /// The offsets of the value braces that follow a type tag, sorted.
  ///
  /// A subset of [value_braces], and the other thing only the grammar knows:
  /// `a11.sdk.Interaction{...}` is one thing and `| map {...}` is a stage and a
  /// literal, so the first hugs its brace and the second takes a space.
  std::vector<size_t> tagged_braces;

  bool HasErrors() const;

  /// The first error in source order, or `nullptr`. This is what a strict
  /// caller turns into a refusal: `flow.loads` raises the Python
  /// `FlowSyntaxError` built from exactly this.
  const Diagnostic* absl_nullable FirstError() const;
};

/// Parse Flow source.
///
/// The grammar is `a11/flow/parser.py`'s, one for one: recursive descent, one
/// token of lookahead, and no reserved words -- a word means `skip` or `for` only
/// where it opens a statement and is not immediately followed by something that
/// makes it a name.
///
/// **This never throws and always returns a tree.** The Python reference raises
/// at the first problem, which is right for a compiler and useless for everything
/// else: an inspector wants every problem, a formatter wants to format the rest of
/// the file, and an editor is looking at something half-typed. So a problem here
/// becomes a `Diagnostic`, the parser recovers -- skipping to the end of the
/// statement, or standing in a `syntax::ErrorNode` where a value was wanted -- and
/// parsing carries on. Nothing downstream has to guess: a subtree that could not
/// be read *says* so.
ParseResult Parse(std::string_view source);

/// Parse an already-lexed stream, sharing the lex diagnostics.
///
/// For a frontend that has the tokens in hand -- a formatter, a highlighter that
/// then wants a tree -- so one file is lexed once. Comment tokens are stepped
/// over here, which is what lets the same stream serve both.
ParseResult ParseTokens(std::string_view source, absl::Span<const Token> tokens,
                        std::vector<Diagnostic> diagnostics);

}  // namespace a11::flow

#endif  // A11_FLOW_PARSER_H_
