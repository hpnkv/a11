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

#ifndef A11_FLOW_HIGHLIGHT_H_
#define A11_FLOW_HIGHLIGHT_H_

#include <string_view>
#include <vector>

#include <absl/types/span.h>

#include "a11/flow/token.h"

namespace a11::flow {

/// What a token *means* where it stands, which is what a reader colours by.
///
/// Finer-grained than the grammar needs: a word is significant or not to a
/// parser, but a reader wants to tell a port type from a pipeline stage from a
/// status code, because that is the distinction they are making. These are the
/// names the output format uses and the names an editor maps to its own
/// palette,
/// so they are part of the contract.
enum class SemanticKind {
  kComment,
  kString,
  kStringEscape,
  kNumber,
  kDuration,
  /// `flow`, `in`, `out`, `header`, `node`, `nodes`, `as`, `default`.
  kDeclarationKeyword,
  /// `run`, `call`, `try`, `wait`, `for`, `if`, `fail`, and the rest.
  kStatementKeyword,
  /// `tee`, `via`, `timeout`, `after`, `with`, `id`, `forward headers`.
  kModifierKeyword,
  /// A stage, after a `|` or as a bare `then`/`where`.
  kStage,
  /// One of the fixed functions, where it is being called.
  kBuiltin,
  /// A port type: `string`, `list`, `a11.sdk.AudioBuffer`.
  kType,
  /// A standard type, using the primitive-type keyword palette.
  kBuiltinType,
  /// A canonical status code: `not_found`, `NOT_FOUND`.
  kStatusCode,
  /// A log level a `log` or `logf` named: `warning`, `DEBUG`.
  kLogLevel,
  /// `true`, `false`, `null`, `it`.
  kConstant,
  /// `and`, `or`, `not`.
  kWordOperator,
  /// The name a `flow` declaration gives.
  kFlowName,
  /// The action a `run`/`call` names.
  kActionName,
  /// The name a `nodes` declaration, or a `via`, gives a node map.
  kNodeMapName,
  /// A module declaration, alias, or qualifier before `::`.
  kNamespaceName,
  /// What follows a `.`: a port, a field, a node's id.
  kMember,
  /// A port of the flow: its declaration, and every mention of it.
  ///
  /// Told apart from every other name a flow binds because a port is the one
  /// thing that crosses the flow's boundary -- it is the interface, and a
  /// reader following where data comes from and goes wants to see which names
  /// are the outside world and which are local plumbing. Deciding it needs the
  /// resolver, so it is [RefinePorts] rather than [Highlight] that says so.
  kPortName,
  /// Any other word: a node, a step, a loop variable, a `value` value.
  kIdentifier,
  /// `->`, `<-`, `|`: where a stream is going.
  kFlowOperator,
  /// `=`, `==`, `<`, `+`, and the rest.
  kOperator,
  kBrace,
  kParenthesis,
  kBracket,
  /// `.`, `:`, `,`.
  kPunctuation,
  /// Something the language has no meaning for.
  kBad,
};

/// One nonoverlapping semantic span; string escapes have their own spans.
struct SemanticToken {
  SemanticKind kind = SemanticKind::kIdentifier;
  size_t start = 0;
  size_t end = 0;
  int line = 1;
  int column = 1;
};

/// The name of a semantic kind in the output formats, in kebab case.
std::string_view SemanticKindName(SemanticKind kind);

/// The kind a name refers to, or `kIdentifier` if it is not one.
SemanticKind SemanticKindFromName(std::string_view name);

/// Classify a token stream.
///
/// The rules are the ones the grammar itself uses, which is why this reads a
/// stream rather than one token at a time: a word is only a stage directly
/// after a `|`, only a function where it is called, only a type past a port's
/// `:`, and whatever follows a `.` is a member however it is spelled. Two of
/// them need a look ahead rather than behind -- a bare `then`/`where` is a
/// stage only with an operand after it, and `node` is the keyword only where
/// its parentheses open -- and having the whole stream is what makes those
/// cheap and exact.
///
/// The tokens are expected to include comments (`LexOptions::keep_comments`),
/// and each string token is split around its escape ranges. Other tokens keep
/// one entry each, except the final `end`.
std::vector<SemanticToken> Highlight(absl::Span<const Token> tokens);

/// Refine lexical tokens using parsed type positions and resolved bindings.
///
/// Ports include declarations, references, argument labels and called ports.
/// Resolved locals retain identifier styling when shadowing a port or keyword.
/// Parsing recovers from incomplete source, without reading imported files.
void RefinePorts(std::string_view source, std::vector<SemanticToken>& semantic);

}  // namespace a11::flow

#endif  // A11_FLOW_HIGHLIGHT_H_
