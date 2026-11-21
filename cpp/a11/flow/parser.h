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

#ifndef A11_FLOW_PARSER_H_
#define A11_FLOW_PARSER_H_

#include <string_view>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/status/statusor.h>
#include <absl/types/span.h>

#include "a11/flow/diagnostic.h"
#include "a11/flow/limits.h"
#include "a11/flow/syntax.h"

namespace a11::flow {

/// The flows a file declares, and everything wrong with it.
struct ParseResult {
  std::vector<syntax::ModuleDeclaration> modules;
  std::vector<syntax::ExportDeclaration> exports;
  std::vector<syntax::Import> imports;
  std::vector<syntax::ConstDeclaration> constants;
  std::vector<syntax::FlowDeclarationPtr> flows;
  /// Top-level shapes in declaration order.
  std::vector<syntax::DtoDeclarationPtr> dtos;
  std::vector<Diagnostic> diagnostics;

  /// Sorted offsets of braces that open values, for formatter spacing.
  std::vector<size_t> value_braces;

  /// Sorted subset of value braces that follow a type tag.
  std::vector<size_t> tagged_braces;

  [[nodiscard]] bool HasErrors() const;

  /// The first error in source order, or `nullptr`. This is what a strict
  /// caller turns into a refusal: `flow.loads` raises the Python
  /// `FlowSyntaxError` built from exactly this.
  [[nodiscard]] const Diagnostic* absl_nullable FirstError() const;
};

/// Parses Flow source into a recoverable tree and diagnostics.
/// Recovery skips to statement ends or inserts `syntax::ErrorNode` for values.
ParseResult Parse(std::string_view source, CompilationLimits limits = {},
                  CompilationBudget* absl_nullable budget = nullptr);

/// Parse a standalone type with the same grammar and limits as a declaration.
absl::StatusOr<syntax::TypeExpression> ParseTypeExpression(
    std::string_view source, CompilationLimits limits = {},
    CompilationBudget* absl_nullable budget = nullptr);

/// Parse an already-lexed stream, sharing the lex diagnostics.
///
/// For a frontend that has the tokens in hand -- a formatter, a highlighter
/// that then wants a tree -- so one file is lexed once. Comment tokens are
/// stepped over here, which is what lets the same stream serve both.
ParseResult ParseTokens(std::string_view source, absl::Span<const Token> tokens,
                        std::vector<Diagnostic> diagnostics,
                        CompilationLimits limits = {},
                        CompilationBudget* absl_nullable budget = nullptr);

}  // namespace a11::flow

#endif  // A11_FLOW_PARSER_H_
