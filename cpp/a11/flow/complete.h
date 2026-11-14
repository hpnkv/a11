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

#ifndef A11_FLOW_COMPLETE_H_
#define A11_FLOW_COMPLETE_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "a11/flow/catalogue.h"
#include "a11/flow/limits.h"

namespace a11::flow {

struct LinkedModule;

/// Completion categories published by `flow.completions/v1`.
enum class ProposalKind {
  /// A word that opens a statement: `run`, `wait`, `for`.
  kStatement,
  /// A word that declares something: `in`, `header`, `describe`.
  kDeclaration,
  /// A word that follows a call: `tee`, `timeout`, `forward headers`.
  kModifier,
  /// A pipeline stage, offered after a `|`.
  kStage,
  /// One of the fixed functions.
  kFunction,
  /// A port type.
  kType,
  /// A canonical status code, as `fail` names one.
  kStatusCode,
  /// A log level, as `log` and `logf` name one.
  kLogLevel,
  /// `true`, `false`, `null`, `it`.
  kConstant,
  /// What a port says about itself: `stream`, `required`.
  kPortModifier,
  /// A flow of this file, offered as a call target.
  kFlow,
  /// A port of this flow, or of a call.
  kPort,
  /// A node of the flow's own.
  kNode,
  /// A node map.
  kNodeMap,
  /// A bound `run`/`call` step.
  kCall,
  /// A bound `wait`/`close`.
  kBarrier,
  /// A loop variable, the `index` a loop binds, or what a `repeat` carries.
  kVariable,
  /// A header, under its alias.
  kHeader,
  /// A field of something: a status's `code`, a node's `id`.
  kField,
};

/// One thing that may be written at an offset.
struct Proposal {
  /// The word, as it would be read back: `truncate`, `x.question`.
  std::string name;
  ProposalKind kind = ProposalKind::kStatement;
  /// Text inserted for this proposal, including required syntax.
  std::string insert;
  /// Text matched while the visible label includes an invocation verb.
  std::string filter;
  /// Caret byte offset in `insert`; `-1` places it at the end.
  int caret = -1;
  /// Optional document offset overriding the common prefix replacement.
  size_t replace_start = std::string::npos;
  /// Grey text an editor shows after the name: what a stage takes, whether a
  /// port is required. Never part of what is inserted.
  std::string tail;
  /// The type of the thing proposed, where it has one: a port's declared type.
  std::string type;
  /// Markdown documentation shared with hover results; empty when unavailable.
  std::string documentation;
};

/// What may be written at an offset, and the word already typed there.
struct CompleteResult {
  std::vector<Proposal> proposals;
  /// Partial word at the caret; proposals remain unfiltered.
  std::string prefix;
  /// Start of the replacement range; equals the caret for an empty prefix.
  size_t prefix_start = 0;
};

/// Returns grammar and scope-aware proposals at `offset` in `source`.
/// Incomplete source produces narrower proposals. Source without a flow is
/// treated as a flow body. `known` supplies available actions and types.
CompleteResult CompleteAt(
    std::string_view source, size_t offset,
    const catalogue::Catalogue& known = catalogue::Catalogue::Builtin(),
    CompilationLimits limits = {},
    CompilationBudget* absl_nullable budget = nullptr,
    const LinkedModule* absl_nullable linked = nullptr);

/// The name of a proposal kind in the output format, in kebab case.
std::string_view ProposalKindName(ProposalKind kind);

/// The `format` field of the completions envelope.
inline constexpr std::string_view kCompletionsFormat = "flow.completions/v1";

}  // namespace a11::flow

#endif  // A11_FLOW_COMPLETE_H_
