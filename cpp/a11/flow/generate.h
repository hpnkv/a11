// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_GENERATE_H_
#define A11_FLOW_GENERATE_H_

#include <string>
#include <string_view>

#include <absl/types/span.h>

namespace a11::flow {

/// An editor definition the language can write for itself.
enum class SyntaxTarget {
  /// `editors/sublime-text/A11 Flow.sublime-syntax`: a Sublime/TextMate-family
  /// YAML grammar, which is also what Zed and a few others read.
  kSublime,
  /// `editors/pygments/a11flow_lexer.py`: a Pygments lexer, which is what
  /// colours a fenced flow in A11's own documentation and in anything else built
  /// on Pygments (MkDocs, Sphinx, `pygmentize`).
  kPygments,
  /// `editors/vscode/a11flow.tmLanguage.json`: a TextMate grammar in the JSON
  /// dialect VSCode reads.
  ///
  /// The *fallback* rather than the whole story, and generated for the same
  /// reason the others are. A VSCode extension with a language server gets its
  /// real colours from semantic tokens, which are the language's own judgement
  /// about every token; this is what colours a `.flow` before the server has
  /// answered, and what colours one with no server at all. So it is a grammar of
  /// words, strings and marks, and it does not try to be a parser.
  kVsCode,
  /// `editors/vscode/a11flow-injection.tmLanguage.json`: the same words, as an
  /// injection into the string literals of a host language.
  ///
  /// Where most flows actually live. A separate file because VSCode injects by
  /// `injectTo` on a grammar of its own rather than by a rule inside another
  /// one, and because the two answer different questions: this one has to decide
  /// *whether* a string is a flow before colouring any of it.
  kVsCodeInjection,
};

/// Every target, for a command that offers a choice of them.
absl::Span<const SyntaxTarget> SyntaxTargets();

/// The name a target is asked for by: `sublime`.
std::string_view SyntaxTargetName(SyntaxTarget target);

/// A target from its name, or `nullopt`.
bool SyntaxTargetFromName(std::string_view name, SyntaxTarget& target);

/// Where the generated file belongs, relative to the repository root.
std::string_view SyntaxTargetPath(SyntaxTarget target);

/// The whole definition, generated.
///
/// **Why generate it.** A static grammar file is a copy of the language's word
/// lists, and a copy is a thing that falls behind: `then` added as a stage is a
/// stage the editor does not colour until somebody remembers this file. So the
/// structure -- which contexts there are, what pushes what -- is written here
/// once, and every list of words in it comes from `vocabulary`. A word added to
/// the language reaches the editor by running the generator, and CI notices when
/// nobody has.
///
/// The output ends with a newline and is byte-stable: the same vocabulary
/// generates the same file, which is what makes `--check` a diff rather than a
/// judgement.
std::string GenerateSyntax(SyntaxTarget target);

}  // namespace a11::flow

#endif  // A11_FLOW_GENERATE_H_
