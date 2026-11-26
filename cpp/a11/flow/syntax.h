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

#ifndef A11_FLOW_SYNTAX_H_
#define A11_FLOW_SYNTAX_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/flow/token.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow::syntax {

/// The start token's byte span, line, and code-point column.
/// Full source extents are available separately on nodes.
struct Location {
  size_t start = 0;
  size_t end = 0;
  int line = 1;
  int column = 1;
};

/// The location a token occupies.
Location LocationOf(const Token& token);

/// A bare name and its location, such as a loop variable.
struct Node;

struct Word {
  std::string text;
  Location location;
  std::shared_ptr<Node> expression = {};

  [[nodiscard]] bool Empty() const { return text.empty(); }
};

/// A literal or folded literal expression, including nested lists and objects.
/// Integer and floating-point values remain distinct.
struct Constant {
  enum class Kind {
    kNull,
    kBool,
    kInteger,
    kDouble,
    kString,
    kBytes,
    kRegex,
    kDuration,
    kList,
    kObject,
  };

  Kind kind = Kind::kNull;
  bool boolean = false;
  long long integer = 0;
  double number = 0.0;
  std::string text;
  absl::Duration duration;
  std::vector<Constant> items;
  std::vector<std::pair<std::string, Constant>> pairs;

  static Constant Null() { return {}; }

  static Constant Bool(bool value);
  static Constant Integer(long long value);
  static Constant Double(double value);
  static Constant String(std::string value);
  static Constant Bytes(std::string value);
  static Constant Regex(std::string value);
  static Constant Duration(absl::Duration value);

  /// The number this holds, whichever way it was written.
  [[nodiscard]] double AsDouble() const;
};

/// The spelling of a constant's kind in the output formats.
std::string_view ConstantKindName(Constant::Kind kind);

/// A port or cast type, such as `string`, `list[a11.NodeFragment]`, or
/// `"audio/wav"`. `name` holds the base name or quoted media type.
std::string SourceTypeName(std::string_view name);
std::string SourceFunctionName(std::string_view name);
std::string RuntimeActionName(std::string_view name);
std::string ActionSource(std::string_view name);

struct TypeExpression {
  Location location = {};
  std::string name;
  std::vector<TypeExpression> parameters = {};
  bool quoted = false;
  /// Whether `list[T]` used the `T[]` spelling, retained by the formatter.
  bool sugared = false;

  /// The type as it would be written, which is what a message quotes.
  [[nodiscard]] std::string ToString() const;

  /// Emit a bounded type expression, preserving quoted representations.
  absl::StatusOr<std::string> ToSource() const;
};

/// Syntax node kinds, one per construct.
enum class NodeKind {
  /// A syntax error retained in the recovering parser's tree.
  kError,

  kLiteral,
  kListLiteral,
  kObjectLiteral,
  kSpread,
  kIt,
  kDiscard,
  kName,
  kAttr,
  kIndex,
  kBuiltin,
  kZip,
  kTypedValue,
  kUnary,
  kBinary,

  kStage,
  kPipeline,
  kOutcome,
  kActionId,
  kPipelineValue,

  kCallModifiers,
  kCallExpression,

  kBind,
  kBlock,
  kCallStatement,
  kPipe,
  kSkip,
  kWait,
  kClose,
  kCancel,
  kAbort,
  kFail,
  kLog,
  kForEach,
  kRepeat,
  kCarry,
  kUntil,
  kIf,
  kNodes,
  kNodeExpression,

  kPortDeclaration,
  kHeaderDeclaration,
  kFlowDeclaration,
  kFieldDeclaration,
  kDtoDeclaration,
};

/// The name of a node kind in the output formats, in kebab case.
std::string_view NodeKindName(NodeKind kind);

/// Base of every syntax node: what it is, and where it started.
struct Node {
  virtual ~Node() = default;

  NodeKind kind = NodeKind::kError;
  Location location;
  /// Full source extent and transparent grouping, for native edits.
  Location extent;
  std::vector<Location> groups;
};

using NodePtr = std::unique_ptr<Node>;

/// Assigns a fixed kind to each syntax node type.
template <NodeKind K>
struct NodeOf : Node {
  static constexpr NodeKind kNodeKind = K;

  NodeOf() { kind = K; }
};

/// Returns `node` as `T`, or null when its kind differs.
template <typename T>
const T* As(const Node* node) {
  if (node == nullptr || node->kind != T::kNodeKind) {
    {
      return nullptr;
    }
  }
  return static_cast<const T*>(node);
}

template <typename T>
T* As(Node* node) {
  if (node == nullptr || node->kind != T::kNodeKind) {
    {
      return nullptr;
    }
  }
  return static_cast<T*>(node);
}

/// Whether a node is one of the kinds given.
bool IsAnyOf(const Node* node, std::initializer_list<NodeKind> kinds);

// --- Expressions -------------------------------------------------------------

/// A place the parser wanted a value and did not find one.
struct ErrorNode : NodeOf<NodeKind::kError> {
  /// What was expected there, for a formatter deciding to leave it alone.
  std::string expected;
};

/// A number, string, boolean, null or duration written out.
struct Literal : NodeOf<NodeKind::kLiteral> {
  Constant value;
};

/// `...expr` in a list or object literal.
struct Spread : NodeOf<NodeKind::kSpread> {
  NodePtr value;
};

/// `[a, ...rest, c]`. An item may be a [Spread].
struct ListLiteral : NodeOf<NodeKind::kListLiteral> {
  std::vector<NodePtr> items;
};

/// `{ "key": expr, ...rest }`.
/// A spread pair has an empty key. Later pairs override earlier ones.
struct ObjectLiteral : NodeOf<NodeKind::kObjectLiteral> {
  std::vector<std::pair<std::string, NodePtr>> pairs;
};

/// `it` -- the value a `where`/`map`/`group` stage is looking at.
struct It : NodeOf<NodeKind::kIt> {};

/// `_`, a pipe destination that discards its input.
struct Discard : NodeOf<NodeKind::kDiscard> {};

/// A bare name: a port, a call, a loop variable, a header alias.
struct Name : NodeOf<NodeKind::kName> {
  std::string name;
};

/// `base.name` -- a call's port, or a key of a value.
struct Attr : NodeOf<NodeKind::kAttr> {
  NodePtr base;
  std::string name;
  bool optional = false;
};

/// `base[i]` -- an element of a list, or a key of an object.
struct Index : NodeOf<NodeKind::kIndex> {
  NodePtr base;
  NodePtr index;
  bool optional = false;
};

/// `name(arg, ...)` -- one of the language's fixed functions.
struct Builtin : NodeOf<NodeKind::kBuiltin> {
  std::string name;
  std::vector<NodePtr> args;
};

/// `zip` or `interleave` over stream sources.
/// `zip` yields tuples in step; `interleave` yields values as they arrive.
struct Zip : NodeOf<NodeKind::kZip> {
  std::string name = "zip";
  std::vector<NodePtr> sources;
};

/// `Tag{...}` or `expr as Tag` -- a value made into a type's value.
struct TypedValue : NodeOf<NodeKind::kTypedValue> {
  TypeExpression type;
  NodePtr value;
};

/// `not operand`.
struct Unary : NodeOf<NodeKind::kUnary> {
  std::string op;
  NodePtr operand;
};

/// `left op right` for `and or == != < <= > >= in + -`.
struct Binary : NodeOf<NodeKind::kBinary> {
  std::string op;
  NodePtr left;
  NodePtr right;
};

// --- Pipelines ---------------------------------------------------------------

/// Arguments shared by `log` and `logf` statements and stages.
/// `log` accepts at most one value; `logf` uses a format and its arguments.
struct LogTail {
  /// The level as written, or empty for the default. See
  /// [vocabulary::LogLevels].
  Word level;
  /// `logf`'s format, with escapes resolved. Empty for a `log`.
  std::string format;
  /// Whether a format was written at all, which tells `logf ""` from `log`.
  bool has_format = false;
  /// What to log, or what fills the format. `it` is bound inside a stage.
  std::vector<NodePtr> arguments;
};

/// One `| name arg` stage of a pipeline.
/// `takes` identifies its argument shape in the vocabulary table.
struct Stage : NodeOf<NodeKind::kStage> {
  std::string name;
  vocabulary::StageArgument takes = vocabulary::StageArgument::kNone;
  std::optional<TypeExpression> type;
  /// `kNumber`: a constant count, when the argument folds to an integer.
  double number = 0.0;
  long long integer = 0;
  bool is_integer = false;
  /// `kString`/`kOptionalString`: the text, with escapes resolved.
  std::string text;
  bool text_is_bytes = false;
  /// `match all`: emit each nonoverlapping occurrence.
  bool match_all = false;
  bool match_regex = false;
  std::string match_offset;
  /// `kExpression`/`kOptionalExpression`: the expression, with `it` bound.
  /// `kSortKey`: the `by` key, or null for the values themselves. `kFold`: the
  /// fold expression, with the carried name and `it` bound. `kStream`: the
  /// stream to read next. `kNumber`/`kDuration`: a scalar stage configuration,
  /// evaluated once per activation without `it`.
  NodePtr argument;
  /// `kDuration`: how long `timeout` waits, or `pace` spaces values out by.
  absl::Duration duration;
  /// `kSortKey`: whether `desc` was written.
  bool descending = false;
  /// `kFold`: the initial literal and carried name.
  Constant start;
  Word carried;
  /// `try map ...`: tolerate an item's evaluation failure; context may retain
  /// the item. Written before the stage name, as `try run` is before a call.
  bool tolerant = false;
  /// `failures into ref`: where a tolerated failure goes. Null when
  /// the failures are only logged.
  NodePtr failures;
  /// `parallel n`: maximum concurrent values; one is the default.
  int parallel = 1;
  /// A nonliteral concurrency count, evaluated once when the stage starts.
  NodePtr parallel_expression;
  /// `context`: surrounding item counts, evaluated once outside `it`.
  NodePtr context_before;
  NodePtr context_after;
  bool context_numbered = false;
  NodePtr context_base;
  /// Whether output preserves input order; `unordered` clears this flag.
  bool ordered = true;
  /// `kLog`/`kLogFormat`: what was written after the stage name. The same shape
  /// the statement of the same name carries.
  LogTail log;
};

using StagePtr = std::unique_ptr<Stage>;

/// A source expression and the stages its values pass through.
struct Pipeline : NodeOf<NodeKind::kPipeline> {
  NodePtr source;
  std::vector<StagePtr> stages;
};

using PipelinePtr = std::unique_ptr<Pipeline>;

/// `status subject`; reading it waits for the subject to finish.
struct Outcome : NodeOf<NodeKind::kOutcome> {
  NodePtr subject;
};

/// `id subject` -- the identity assigned to a run or call.
struct ActionId : NodeOf<NodeKind::kActionId> {
  NodePtr subject;
};

/// `(stream | stage ...)` used where a value is expected.
struct PipelineValue : NodeOf<NodeKind::kPipelineValue> {
  PipelinePtr pipeline;
  /// Explicit extraction, with absence on EOF only when optional.
  bool take = false;
  bool optional = false;
};

// --- Calls -------------------------------------------------------------------

/// The `tee`/`via`/`timeout`/`after`/`with`/`id`/`id-into`/`forward` tail.
struct CallModifiers : NodeOf<NodeKind::kCallModifiers> {
  /// Modifier keyword positions, in source order.
  std::vector<Location> clause_locations;
  bool tee = false;
  Word node_map;
  std::optional<absl::Duration> timeout;
  NodePtr deadline;
  NodePtr timeout_expression;
  std::vector<Word> after;
  std::vector<std::pair<std::string, NodePtr>> headers;
  NodePtr action_id;
  NodePtr id_into;
  /// Header names, or `*` patterns, that `forward headers` names.
  std::vector<std::string> forward;
};

using CallModifiersPtr = std::unique_ptr<CallModifiers>;

/// `run`/`call action(port: pipeline, ...)` and its modifiers.
struct CallExpression : NodeOf<NodeKind::kCallExpression> {
  /// One argument: the port named, where it was named, and what feeds it.
  struct Argument {
    Word port;
    PipelinePtr pipeline;
  };

  std::string action;
  Location action_location = {};
  /// The verb written: `"run"` for the handler registered here, `"call"` for
  /// the stream this flow is attached to.
  std::string mode;
  std::vector<Argument> args;
  CallModifiersPtr modifiers;
  bool tolerant = false;

  struct TargetSuffix {
    Location location = {};
    std::string member;
    NodePtr index;
  };

  bool invalid_target = false;
  std::vector<TargetSuffix> target_suffixes;
};

using CallExpressionPtr = std::unique_ptr<CallExpression>;

// --- Statements --------------------------------------------------------------

/// `name = ...` -- a step the rest of the flow can refer to.
struct Bind : NodeOf<NodeKind::kBind> {
  Word name;
  std::vector<Word> names;
  NodePtr value;
  std::optional<TypeExpression> type;
  bool constant = false;
};

/// `[try] { ... }` groups concurrent statements into one outcome.
/// `try` makes a block failure available to the flow.
struct Block : NodeOf<NodeKind::kBlock> {
  bool tolerant = false;
  std::vector<Word> after;
  std::vector<NodePtr> body;
};

/// A call statement whose unnamed outputs are drained.
struct CallStatement : NodeOf<NodeKind::kCallStatement> {
  CallExpressionPtr call;
};

/// `pipeline -> target, target` -- write a stream into one or more nodes.
struct Pipe : NodeOf<NodeKind::kPipe> {
  PipelinePtr pipeline;
  std::vector<NodePtr> targets;
  std::vector<Word> after;
  /// `try source -> dest`: capture a source or destination failure as a value.
  bool tolerant = false;
};

/// The one reference changed by a counted `skip`.
struct SkipTarget {
  PipelinePtr pipeline;
  Word call;
  std::vector<Word> outputs;
};

/// `skip n reference` -- discard the first `n` values for every reader.
///
/// `flow.syntax/v1` represents the target as a one-element vector.
struct Skip : NodeOf<NodeKind::kSkip> {
  std::vector<SkipTarget> targets;
  std::vector<Word> after;
  std::optional<long long> count;
};

/// Wait for one subject, the first of several, or all of several.
/// Single waits use `subject`; plural waits use `subjects`.
struct Wait : NodeOf<NodeKind::kWait> {
  bool scope_barrier = false;
  NodePtr subject;
  std::vector<NodePtr> subjects;
  /// Whether the first plural subject to finish satisfies the wait.
  bool race = false;
  std::optional<absl::Duration> timeout;
  NodePtr deadline;
  NodePtr timeout_expression;
  std::vector<Word> after;
  /// Destinations for the winning subject's zero-based index.
  std::vector<NodePtr> targets;
};

/// `close target` -- hold until a node's writers are done and its buffer has
/// landed.
struct Close : NodeOf<NodeKind::kClose> {
  NodePtr target;
  std::vector<Word> after;
};

/// `cancel name` -- ask a called action to stop, cooperatively.
struct Cancel : NodeOf<NodeKind::kCancel> {
  Word name;
  std::vector<Word> after;
};

/// `abort node [code] [message]` -- end a node with a failure.
struct Abort : NodeOf<NodeKind::kAbort> {
  NodePtr target;
  NodePtr code;
  NodePtr message;
  std::vector<Word> after;
};

/// `fail [code] [message]` -- end the flow with a status.
struct Fail : NodeOf<NodeKind::kFail> {
  NodePtr code;
  NodePtr message;
  std::vector<Word> after;
};

/// `log [level] [what]` / `logf [level] "format" [args]` -- write to the flow's
/// own log.
struct Log : NodeOf<NodeKind::kLog> {
  LogTail tail;
  std::vector<Word> after;
};

/// `for name[, name...] in pipeline [parallel n] { ... }`.
///
/// Several names unpack the value by position, as in
/// `for url, title in zip(urls, titles)`.
struct ForEach : NodeOf<NodeKind::kForEach> {
  std::vector<Word> variables;
  PipelinePtr pipeline;
  int parallel = 1;
  /// A nonliteral concurrency count, evaluated once before iteration.
  NodePtr parallel_expression;
  std::vector<NodePtr> body;
  /// Optional postcondition, evaluated after a complete pass.
  NodePtr termination;
  /// Dependencies that finish before the loop starts.
  std::vector<Word> after;

  /// The first name, which is the whole value where there is only one.
  [[nodiscard]] const Word& variable() const {
    static const Word kNone;
    return variables.empty() ? kNone : variables.front();
  }
};

/// `repeat [name = expr] [max n] { ... }`.
struct Repeat : NodeOf<NodeKind::kRepeat> {
  /// Empty where the repeat carries nothing.
  Word variable;
  NodePtr start;
  /// Optional `max n` bound; absent for a condition-only loop.
  std::optional<int> max_iterations;
  NodePtr max_source;
  std::vector<NodePtr> body;
  /// Optional postcondition, resolved in the iteration scope.
  NodePtr termination;
  /// What has to have finished before the loop starts. See ForEach::after.
  std::vector<Word> after;
};

/// `name <- pipeline` -- what the next pass of a `repeat` carries.
struct Carry : NodeOf<NodeKind::kCarry> {
  Word name;
  PipelinePtr pipeline;
};

/// `until expr` / `while expr` -- when a `repeat` stops.
struct Until : NodeOf<NodeKind::kUntil> {
  NodePtr condition;
  bool stop_when = true;
};

/// `if expr { ... } else { ... }`.
struct If : NodeOf<NodeKind::kIf> {
  Word binding;
  Location binding_scope;
  NodePtr condition;
  std::vector<NodePtr> then_body;
  std::vector<NodePtr> else_body;
};

/// `nodes name [{ ... }]` -- declare a temporary node map.
struct Nodes : NodeOf<NodeKind::kNodes> {
  Word name;
  std::vector<NodePtr> body;
  /// Whether a node-map block was written, including an empty block.
  bool has_body = false;
};

/// `node(id) [in map]` -- a node of this flow's own.
struct NodeExpression : NodeOf<NodeKind::kNodeExpression> {
  NodePtr id;
  Word node_map;
};

// --- Declarations ------------------------------------------------------------

/// Which side of the descriptor a port lands on, spelled as the plan spells it.
enum class PortDirection { kInput, kOutput };

/// `"inputs"` or `"outputs"`.
std::string_view PortDirectionName(PortDirection direction);

/// `in`/`out name: type [stream] [required]`.
struct PortDeclaration : NodeOf<NodeKind::kPortDeclaration> {
  Word name;
  PortDirection direction = PortDirection::kInput;
  TypeExpression type;
  /// Whether the port carries one value. Most do, so it is the default.
  bool unary = true;
  bool required = false;
  std::string description;
};

using PortDeclarationPtr = std::unique_ptr<PortDeclaration>;

/// `header "x-name" [as alias] [default value]`.
struct HeaderDeclaration : NodeOf<NodeKind::kHeaderDeclaration> {
  std::string name;
  Word alias;
  Constant default_value;
  NodePtr default_source;
  bool has_default = false;
  std::string description;
};

using HeaderDeclarationPtr = std::unique_ptr<HeaderDeclaration>;

struct ImportedSymbol {
  Word name;
  Word alias;
  bool explicit_alias = false;
};

/// A source module bound to a namespace or selected local symbols.
struct Import {
  std::string source;
  std::string scope;
  bool namespace_source = false;
  bool explicit_alias = false;
  Location source_location;
  bool exported = false;
  bool selective = false;
  size_t selection_start = 0;
  std::vector<ImportedSymbol> symbols;
  Word alias;
  Location location;
  Location extent;
};

/// A namespace contribution in one source document.
struct ModuleDeclaration {
  Word name;
  bool interface_only = false;
  bool exported = false;
  Location extent;
};

/// A file or module value, fixed for the lifetime of a compiled program.
struct ConstDeclaration {
  Word name;
  std::string scope;
  std::optional<TypeExpression> type;
  NodePtr value;
  bool exported = false;
  Location extent;
};

/// A public alias of an existing namespace member.
struct ExportDeclaration {
  Word symbol;
  Word alias;
  std::string scope;
  Location extent;
};

struct DtoDeclaration;

/// One `flow name { ... }` declaration, or the one `flow { ... }` entry point.
struct FlowDeclaration : NodeOf<NodeKind::kFlowDeclaration> {
  Word name;
  std::string binding_name;
  std::string module;
  std::string scope;
  /// Whether this is the unnamed `flow { ... }` entry point.
  /// The interpreter supplies its `argc` and `argv` ports.
  bool entry = false;
  bool interface_only = false;
  bool imported = false;
  bool exported = false;
  bool registerable = true;
  std::string description;
  std::vector<PortDeclarationPtr> ports;
  std::vector<HeaderDeclarationPtr> headers;
  std::vector<Import> imports;
  /// Names visible to this flow through module imports, mapped to linked constants.
  std::vector<std::pair<std::string, std::string>> constant_aliases;
  std::vector<std::unique_ptr<FlowDeclaration>> declarations;
  std::vector<std::unique_ptr<DtoDeclaration>> structs;
  std::vector<NodePtr> body;
};

using FlowDeclarationPtr = std::unique_ptr<FlowDeclaration>;

/// A field bound such as `1..200`, `1..`, or `..200`.
/// It applies to numeric or temporal values and to text or collection lengths.
struct FieldRange {
  bool has_minimum = false;
  bool has_maximum = false;
  Constant minimum;
  Constant maximum;

  [[nodiscard]] bool Empty() const { return !has_minimum && !has_maximum; }
};

/// One `name: type [modifiers] ["description"]` field of a `struct`.
struct FieldDeclaration : NodeOf<NodeKind::kFieldDeclaration> {
  Word name;
  TypeExpression type;
  /// Whether the value must be supplied; optional fields may be absent.
  bool required = false;
  /// `unique`: no two items of a list are equal.
  bool unique = false;
  FieldRange range;
  NodePtr minimum_source;
  NodePtr maximum_source;
  /// `matching "..."`: the pattern every value has to match, unanchored, as
  /// JSONSchema's `pattern` is.
  std::string pattern;
  NodePtr pattern_source;
  bool has_pattern = false;
  /// `one of [..]`: the only values allowed.
  std::vector<Constant> enumeration;
  NodePtr enumeration_source;
  bool has_enumeration = false;
  /// `default ..`: what a value that was not given is.
  Constant default_value;
  NodePtr default_source;
  bool has_default = false;
  std::string description;
};

using FieldDeclarationPtr = std::unique_ptr<FieldDeclaration>;

/// One `struct name { ... }` declaration: a shape a port may be typed with.
///
/// A module-level shape or a shape local to an implemented flow.
struct DtoDeclaration : NodeOf<NodeKind::kDtoDeclaration> {
  Word name;
  std::string module;
  std::string scope;
  bool exported = false;
  std::string description;
  std::vector<FieldDeclarationPtr> fields;
};

using DtoDeclarationPtr = std::unique_ptr<DtoDeclaration>;

/// Whether a bounded expression contains only literal data and pure operators.
bool IsPureLiteralExpression(const Node& node);

/// Folds a literal expression, or returns `nullopt` for runtime values.
using ConstantLookup =
    std::function<std::optional<Constant>(std::string_view name)>;
std::optional<Constant> ConstantValue(const Node* node,
                                      const ConstantLookup* lookup = nullptr);

/// Visits direct child nodes in source order.
/// Type expressions are values on nodes and are not visited separately.
void VisitChildren(const Node& node,
                   const std::function<void(const Node&)>& visit);

/// Visits a subtree iteratively, starting with @p node.
/// Parents precede children; sibling order is unspecified.
void VisitSubtree(const Node& node,
                  const std::function<void(const Node&)>& visit);

/// Walk an exclusively owned syntax tree for native linking/editing.
void VisitMutableSubtree(Node& node, const std::function<void(Node&)>& visit);

/// Returns a dotted `Name`/`Attr` chain, or `nullopt` for other nodes.
std::optional<std::string> DottedName(const Node* node);

}  // namespace a11::flow::syntax

#endif  // A11_FLOW_SYNTAX_H_
