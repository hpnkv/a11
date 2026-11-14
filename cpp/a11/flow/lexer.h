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

#ifndef A11_FLOW_LEXER_H_
#define A11_FLOW_LEXER_H_

#include <string_view>
#include <vector>

#include "a11/flow/diagnostic.h"
#include "a11/flow/limits.h"
#include "a11/flow/token.h"

namespace a11::flow {

/// What to keep while lexing.
struct LexOptions {
  /// Whether comments appear as tokens; parser callers may omit them.
  bool keep_comments = true;
  CompilationLimits limits;
  CompilationBudget* budget = nullptr;
};

/// Tokens, and what could not be read.
struct LexResult {
  std::vector<Token> tokens;
  std::vector<Diagnostic> diagnostics;

  /// Whether anything is an error, which is what the strict entry points check.
  [[nodiscard]] bool HasErrors() const;
};

/// Lexes Flow source, ending with one `end` token.
/// Newlines delimit statements; malformed tokens produce diagnostics while
/// lexing continues. Strict compilation checks the reported errors.
LexResult Lex(std::string_view source, LexOptions options = {});

}  // namespace a11::flow

#endif  // A11_FLOW_LEXER_H_
