// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_COMPLETE_H_
#define A11_FLOW_COMPLETE_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace a11::flow {

/// What a proposal *is*, which is what an editor turns into an icon.
///
/// Finer than "keyword or not": a reader choosing between a port and a stage is
/// making a different decision than one choosing between two ports, and the
/// icons are how an editor says which list they are looking at. These names are
/// part of the `flow.completions/v1` contract.
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
  /// A bound `wait`/`drain`.
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
  /// The text to insert, which is `name` unless taking it writes more: a
  /// function takes its parentheses, an argument takes the colon that has to
  /// follow it.
  std::string insert;
  /// Where the caret lands inside `insert`, in bytes. `-1` is the end of it,
  /// which is what almost everything wants; a function wants it between the
  /// parentheses it just wrote.
  int caret = -1;
  /// Grey text an editor shows after the name: what a stage takes, whether a
  /// port is required. Never part of what is inserted.
  std::string tail;
  /// The type of the thing proposed, where it has one: a port's declared type.
  std::string type;
};

/// What may be written at an offset, and the word already typed there.
struct CompleteResult {
  std::vector<Proposal> proposals;
  /// The partial word the caret is in, if any. Not filtered on here: every
  /// frontend that consumes this filters and sorts by its own rules, and one
  /// that filtered twice would drop what a fuzzy matcher would have kept.
  std::string prefix;
  /// Where that word starts, so an editor knows what a proposal replaces.
  size_t prefix_start = 0;
};

/// What may be written at `offset` in `source`.
///
/// **The one implementation of the judgement.** After a `|` only a stage can
/// follow; past a port's `:` only a type; after a `->` only somewhere writable;
/// after `x.` only what `x` actually has. Each of those is a fact about the
/// grammar and the names in scope, and a frontend that worked it out for itself
/// would be a second, worse copy of the language -- which is what the plugin's
/// Kotlin completion was.
///
/// **It never fails and never throws.** The document is being typed: the
/// statement the caret is in is usually half-written and the braces are usually
/// unbalanced. Parsing recovers, resolution recovers, and what cannot be
/// established simply narrows the list -- an unknown call target offers `status`
/// and no ports rather than nothing at all.
///
/// A source that declares no flow is completed as though it were a flow body,
/// which is what an injected fragment in a Python string is.
CompleteResult CompleteAt(std::string_view source, size_t offset);

/// The name of a proposal kind in the output format, in kebab case.
std::string_view ProposalKindName(ProposalKind kind);

/// The `format` field of the completions envelope.
inline constexpr std::string_view kCompletionsFormat = "flow.completions/v1";

}  // namespace a11::flow

#endif  // A11_FLOW_COMPLETE_H_
