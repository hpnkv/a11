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
  /// colours a fenced flow in A11's own documentation and in anything else
  /// built
  /// on Pygments (MkDocs, Sphinx, `pygmentize`).
  kPygments,
  /// `editors/vscode/a11flow.tmLanguage.json`: a TextMate grammar in the JSON
  /// dialect VSCode reads.
  ///
  /// Provides lexical highlighting until semantic tokens are available.
  kVsCode,
  /// `editors/vscode/a11flow-injection.tmLanguage.json`: Flow in host strings.
  kVsCodeInjection,
  /// Native vocabulary data for the browser fallback highlighter.
  kBrowser,
};

/// Native document-start pattern used by host-string injectors.
std::string FragmentStartPattern();

/// Every target, for a command that offers a choice of them.
absl::Span<const SyntaxTarget> SyntaxTargets();

/// The name a target is asked for by: `sublime`.
std::string_view SyntaxTargetName(SyntaxTarget target);

/// A target from its name, or `nullopt`.
bool SyntaxTargetFromName(std::string_view name, SyntaxTarget& target);

/// Where the generated file belongs, relative to the repository root.
std::string_view SyntaxTargetPath(SyntaxTarget target);

/// Generates the target grammar from the Flow vocabulary.
/// Output is deterministic and ends with a newline.
std::string GenerateSyntax(SyntaxTarget target);

}  // namespace a11::flow

#endif  // A11_FLOW_GENERATE_H_
