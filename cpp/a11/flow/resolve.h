// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_RESOLVE_H_
#define A11_FLOW_RESOLVE_H_

#include <string>
#include <string_view>
#include <vector>

#include "a11/flow/diagnostic.h"
#include "a11/flow/graph.h"
#include "a11/flow/parser.h"
#include "a11/flow/plan.h"
#include "a11/flow/syntax.h"

namespace a11::flow {

/// What a name means inside a flow.
enum class SymbolKind {
  /// An `in` port: read, never written.
  kInputPort,
  /// An `out` port: written, never read... except by this flow, which may read
  /// back what it wrote to one only when it is a node of its own. See
  /// `Symbol::readable`.
  kOutputPort,
  /// A header, under its alias.
  kHeader,
  /// A `run`/`call` step, bound to a name.
  kCall,
  /// A node of the flow's own, from `node()`.
  kNode,
  /// A node map, from `nodes`.
  kNodeMap,
  /// A bound `wait` or `drain`: a barrier that also reads as its outcome.
  kBarrier,
  /// A `for` variable, or the `index` every loop binds.
  kLoopVariable,
  /// What a `repeat` carries.
  kCarry,
  /// One value, read from a stream and given a name by `let`.
  ///
  /// Read as a value wherever an expression is accepted. It is never a write
  /// target.
  kValue,
};

/// One name, what it is, and what became of it.
///
/// The usage counts are here rather than in the inspector because the resolver
/// is the pass that walks every reference: counting as it goes costs nothing,
/// and an inspector that walked the tree again to find out would be a second
/// implementation of name resolution.
struct Symbol {
  SymbolKind kind = SymbolKind::kInputPort;
  std::string name;
  syntax::Location location;
  /// Whether the language bound it rather than the author: the `index` every
  /// loop provides. Nothing is said about one of these going unread.
  bool implicit = false;
  /// Whether a pipeline may read it.
  bool readable = true;
  /// Whether a pipe may write it.
  bool writable = false;
  /// For a call: the action named, whether it was `try`, and the sibling flow
  /// it resolves to if it is one.
  std::string action;
  bool tolerant = false;
  const FlowPlan* absl_nullable target = nullptr;
  /// How many times something read it, wrote it, or waited for it.
  int reads = 0;
  int writes = 0;
  /// For a call: whether anything read its status -- `wait x`, `status x`, or a
  /// bound `wait`. A `try` whose status nobody reads is the flow ignoring the
  /// failure it just said it expected.
  int status_reads = 0;
  // The graph ref this name reads and writes, when a graph was built: a port, a
  // node, a header, a loop variable.
  /// The graph ref this name reads and writes, when a graph was built: a port,
  /// a node, a header, a loop variable. Memoised here because identity is the
  /// whole point -- two mentions of one node are one stream, and `skip n`
  /// accumulates on it.
  graph::RefId ref = graph::kNone;
  /// The graph step this name *is*: a call, or a bound `wait`/`drain`.
  graph::StepId step = graph::kNone;
  /// For a value a `let` bound: the stream it took a value *of*, and which
  /// value
  /// of that stream it is. `advance` reads both to bind the next one, which is
  /// what makes "the first use sees the first value" a fact rather than a hope.
  graph::RefId value_source = graph::kNone;
  int value_offset = 0;
  // The `match` pattern this value came out of, where it came out of a literal
  // one.
  /// The `match` pattern this value came out of, where it came out of a literal
  /// one.
  ///
  /// A pattern names its fields, so this is how the tooling knows what a value
  /// has without the language gaining a type system: the fields are in the text
  /// that made it. Empty for everything else, including a pattern computed at
  /// run
  /// time -- nothing is known about those and offering a guess would be worse
  /// than offering nothing.
  std::string pattern;
  /// Whether this name is one *part* of a value a `let` took apart.
  ///
  /// A fact rather than something inferred from `value_source` being absent:
  /// the editor path builds no graph, so every ref is absent there and a check
  /// that read the ref would be a check an editor never runs.
  bool value_part = false;
};

/// One flow, resolved: its plan, its names, and what is wrong with it.
struct ResolvedFlow {
  FlowPlan plan;
  // The executable graph, when [Resolve] was asked for one.
  /// The executable graph, when [Resolve] was asked for one. Empty otherwise --
  /// `root` is [graph::kNone] and nothing else is filled in.
  ///
  /// **Borrows the syntax tree**, like every graph: it is only valid while the
  /// [ParseResult] it was resolved from lives. See [graph::FlowGraph].
  graph::FlowGraph graph;
  /// Every symbol the flow bound, in the order it bound them. Not a scope
  /// tree: scopes matter while resolving and what an inspector wants
  /// afterwards is the list, with the counts on it.
  std::vector<Symbol> symbols;
  /// The declaration this came from, borrowed. Valid as long as the parse
  /// result
  /// it belongs to is.
  const syntax::FlowDeclaration* absl_nullable declaration = nullptr;
};

/// A whole file, resolved.
struct ResolveResult {
  Program program;
  std::vector<ResolvedFlow> flows;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool HasErrors() const;
  [[nodiscard]] const Diagnostic* absl_nullable FirstError() const;
};

/// Resolve a parsed program: names, ports, scopes, node maps and types.
///
/// **What this is.** The semantic pass: it decides what every name in a flow
/// means,
/// checks that a call's ports exist on the flow it names, that a type is a
/// type,
/// that a `<-` has a `repeat` to carry into, and produces the [Program] that
/// `a11 flow describe` prints. Its errors are `a11/flow/plan.py`'s, in the same
/// words, so the two agree about what compiles.
///
/// With `build_graph` it also builds the **executable graph** for every flow
/// -- the refs, steps and bodies the runtime walks -- beside the description.
/// That is off by default because an editor, `a11 flow check` and CI want none
/// of it, and a graph borrows the parse tree it was built from: whoever asks
/// for one owns both for as long as a flow is registered.
///
/// Like the parser, it **never throws and always returns**: a flow with an
/// unresolvable name in it is still resolved as far as it goes, because an
/// editor
/// wants every problem in the file rather than the first.
ResolveResult Resolve(std::string_view source, const ParseResult& parsed,
                      bool build_graph = false);

/// The name of a symbol kind, for a message and for the JSON.
std::string_view SymbolKindName(SymbolKind kind);

}  // namespace a11::flow

#endif  // A11_FLOW_RESOLVE_H_
