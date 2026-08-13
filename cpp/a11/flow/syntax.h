// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_SYNTAX_H_
#define A11_FLOW_SYNTAX_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/time/time.h>

#include "a11/flow/token.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow::syntax {

/// Where a piece of syntax starts, reduced to what every reader of it needs.
///
/// The span of the *token* the construct began at, not of the whole construct --
/// the same thing `a11/flow/syntax.py` keeps, and what a diagnostic wants to point
/// at. A frontend that needs the full extent of a statement has the token stream.
///
/// A node carries this rather than the [Token] it was made from, because the AST
/// outlives the token stream: a token borrows the source text, and a tree that
/// borrowed it could not be handed across a language boundary or kept while the
/// document changed underneath. Offsets are bytes, so an editor can edit with
/// them; line and column count code points, as everything in the language does.
struct Location {
  size_t start = 0;
  size_t end = 0;
  int line = 1;
  int column = 1;
};

/// The location a token occupies.
Location LocationOf(const Token& token);

/// A name as written, and where it was written.
///
/// Used wherever the grammar takes a bare name rather than an expression -- a
/// step's `after`, a `via`, a loop variable. The Python reference keeps these as
/// plain strings and so cannot point at one; a resolver reporting "nothing binds
/// this" has to, so the position travels with the word.
struct Word {
  std::string text;
  Location location;

  bool Empty() const { return text.empty(); }
};

/// A value the language can write out in full: what a literal is, and what
/// folding a literal expression gives.
///
/// Recursive, because a list of objects of lists is a constant like any other.
/// `kInteger` is kept apart from `kDouble` for the reason the grammar cares
/// about: a count of values has to be a whole number of them.
struct Constant {
  enum class Kind {
    kNull,
    kBool,
    kInteger,
    kDouble,
    kString,
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
  static Constant Duration(absl::Duration value);

  /// The number this holds, whichever way it was written.
  double AsDouble() const;
};

/// The spelling of a constant's kind in the output formats.
std::string_view ConstantKindName(Constant::Kind kind);

/// The type of a port, or of a value being made one: `string`,
/// `list[a11.NodeFragment]`, `"audio/wav"`.
///
/// Not a [Node]: a type never stands where an expression does, and keeping it a
/// value type is what lets a port declaration and a cast hold one directly.
/// `name` is what was written before the brackets -- a built-in name, a dotted
/// serialisation tag, or a mimetype when `quoted` -- and `parameters` are the
/// types a generic one was given.
struct TypeExpression {
  Location location;
  std::string name;
  std::vector<TypeExpression> parameters;
  bool quoted = false;

  /// The type as it would be written, which is what a message quotes.
  std::string ToString() const;
};

/// What a node is. One per construct, mirroring `a11/flow/syntax.py`.
enum class NodeKind {
  /// Something the parser could not read. Carried so the tree keeps its shape
  /// around a mistake: a statement with a broken argument is still a statement,
  /// and everything after it is still worth checking.
  kError,

  kLiteral,
  kListLiteral,
  kObjectLiteral,
  kIt,
  kName,
  kAttr,
  kIndex,
  kBuiltin,
  kTypedValue,
  kUnary,
  kBinary,

  kStage,
  kPipeline,
  kOutcome,
  kPipelineValue,

  kCallModifiers,
  kCallExpression,

  kBind,
  kCallStatement,
  kPipe,
  kSkip,
  kWait,
  kDrain,
  kCancel,
  kFail,
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
};

/// The name of a node kind in the output formats, in kebab case.
std::string_view NodeKindName(NodeKind kind);

/// Base of every syntax node: what it is, and where it started.
struct Node {
  virtual ~Node() = default;

  NodeKind kind = NodeKind::kError;
  Location location;
};

using NodePtr = std::unique_ptr<Node>;

/// Gives each construct its kind without every constructor restating it.
template <NodeKind K>
struct NodeOf : Node {
  static constexpr NodeKind kNodeKind = K;

  NodeOf() { kind = K; }
};

/// `node` as a `T`, or `nullptr` if it is something else.
///
/// The one way to go from a node to a construct: it checks the kind, so a
/// mistaken assumption is a null rather than a misread object.
template <typename T>
const T* As(const Node* node) {
  if (node == nullptr || node->kind != T::kNodeKind) return nullptr;
  return static_cast<const T*>(node);
}

template <typename T>
T* As(Node* node) {
  if (node == nullptr || node->kind != T::kNodeKind) return nullptr;
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

/// `[a, b, c]`.
struct ListLiteral : NodeOf<NodeKind::kListLiteral> {
  std::vector<NodePtr> items;
};

/// `{ "key": expr, ... }`.
struct ObjectLiteral : NodeOf<NodeKind::kObjectLiteral> {
  std::vector<std::pair<std::string, NodePtr>> pairs;
};

/// `it` -- the value a `where`/`map`/`group` stage is looking at.
struct It : NodeOf<NodeKind::kIt> {};

/// A bare name: a port, a call, a loop variable, a header alias.
struct Name : NodeOf<NodeKind::kName> {
  std::string name;
};

/// `base.name` -- a call's port, or a key of a value.
struct Attr : NodeOf<NodeKind::kAttr> {
  NodePtr base;
  std::string name;
};

/// `base[i]` -- an element of a list, or a key of an object.
struct Index : NodeOf<NodeKind::kIndex> {
  NodePtr base;
  NodePtr index;
};

/// `name(arg, ...)` -- one of the language's fixed functions.
struct Builtin : NodeOf<NodeKind::kBuiltin> {
  std::string name;
  std::vector<NodePtr> args;
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

/// One `| name arg` stage of a pipeline.
///
/// `takes` says which of the argument fields is the one that was filled, and is
/// read from the vocabulary rather than decided here: the stage table is the one
/// table, and a stage the language does not have is a diagnostic, not a shape.
struct Stage : NodeOf<NodeKind::kStage> {
  std::string name;
  vocabulary::StageArgument takes = vocabulary::StageArgument::kNone;
  /// `kNumber`: the count, and whether it was written as a whole number.
  double number = 0.0;
  bool is_integer = false;
  /// `kString`/`kOptionalString`: the text, with escapes resolved.
  std::string text;
  /// `kExpression`: the expression, with `it` bound. `kStream`: the stream to
  /// read next.
  NodePtr argument;
};

using StagePtr = std::unique_ptr<Stage>;

/// A source expression and the stages its values pass through.
struct Pipeline : NodeOf<NodeKind::kPipeline> {
  NodePtr source;
  std::vector<StagePtr> stages;
};

using PipelinePtr = std::unique_ptr<Pipeline>;

/// `status subject` -- the status of a call, a node, or a barrier.
///
/// Reading one is a synchronisation point: the subject has to be finished before
/// there is a status to report.
struct Outcome : NodeOf<NodeKind::kOutcome> {
  NodePtr subject;
};

/// `(stream | stage ...)` used where a value is expected.
struct PipelineValue : NodeOf<NodeKind::kPipelineValue> {
  PipelinePtr pipeline;
};

// --- Calls -------------------------------------------------------------------

/// The `tee`/`via`/`timeout`/`after`/`with`/`id`/`forward` tail of a call.
struct CallModifiers : NodeOf<NodeKind::kCallModifiers> {
  bool tee = false;
  Word node_map;
  std::optional<absl::Duration> timeout;
  std::vector<Word> after;
  std::vector<std::pair<std::string, NodePtr>> headers;
  NodePtr action_id;
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
  /// The verb written: `"run"` for the handler registered here, `"call"` for
  /// the stream this flow is attached to.
  std::string mode;
  std::vector<Argument> args;
  CallModifiersPtr modifiers;
  bool tolerant = false;
};

using CallExpressionPtr = std::unique_ptr<CallExpression>;

// --- Statements --------------------------------------------------------------

/// `name = ...` -- a step the rest of the flow can refer to.
struct Bind : NodeOf<NodeKind::kBind> {
  Word name;
  NodePtr value;
};

/// A call whose outputs nobody names (they are drained for it).
struct CallStatement : NodeOf<NodeKind::kCallStatement> {
  CallExpressionPtr call;
};

/// `pipeline -> target, target` -- write a stream into one or more nodes.
struct Pipe : NodeOf<NodeKind::kPipe> {
  PipelinePtr pipeline;
  std::vector<NodePtr> targets;
  std::vector<Word> after;
};

/// `skip pipeline` -- read a stream to its end and discard the values.
///
/// With a `count` it is `skip n reference`, which discards the first `n` values
/// of that one node for every reader of it rather than reading the whole thing.
struct Skip : NodeOf<NodeKind::kSkip> {
  PipelinePtr pipeline;
  std::vector<Word> after;
  std::optional<long long> count;
};

/// `wait subject` -- hold until a call, or a node this flow writes, is finished.
struct Wait : NodeOf<NodeKind::kWait> {
  NodePtr subject;
  std::optional<absl::Duration> timeout;
  std::vector<Word> after;
};

/// `drain target` -- hold until a node's writers are done and its buffer has
/// landed.
struct Drain : NodeOf<NodeKind::kDrain> {
  NodePtr target;
  std::vector<Word> after;
};

/// `cancel name` -- ask a called action to stop, cooperatively.
struct Cancel : NodeOf<NodeKind::kCancel> {
  Word name;
  std::vector<Word> after;
};

/// `fail [code] [message]` -- end the flow with a status.
struct Fail : NodeOf<NodeKind::kFail> {
  NodePtr code;
  NodePtr message;
  std::vector<Word> after;
};

/// `for name in pipeline [parallel n] { ... }`.
struct ForEach : NodeOf<NodeKind::kForEach> {
  Word variable;
  PipelinePtr pipeline;
  int parallel = 1;
  std::vector<NodePtr> body;
};

/// `repeat [name = expr] [max n] { ... }`.
struct Repeat : NodeOf<NodeKind::kRepeat> {
  /// Empty where the repeat carries nothing.
  Word variable;
  NodePtr start;
  int max_iterations = 16;
  std::vector<NodePtr> body;
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
  NodePtr condition;
  std::vector<NodePtr> then_body;
  std::vector<NodePtr> else_body;
};

/// `nodes name [{ ... }]` -- declare a temporary node map.
struct Nodes : NodeOf<NodeKind::kNodes> {
  Word name;
  std::vector<NodePtr> body;
  /// Whether a block was written, which is what tells `nodes x {}` -- every
  /// call inside it placed in the map -- from a bare declaration of one.
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
  bool has_default = false;
  std::string description;
};

using HeaderDeclarationPtr = std::unique_ptr<HeaderDeclaration>;

/// One `flow name { ... }` declaration.
struct FlowDeclaration : NodeOf<NodeKind::kFlowDeclaration> {
  Word name;
  std::string description;
  std::vector<PortDeclarationPtr> ports;
  std::vector<HeaderDeclarationPtr> headers;
  std::vector<NodePtr> body;
};

using FlowDeclarationPtr = std::unique_ptr<FlowDeclaration>;

/// The constant `node` folds to, or `nullopt` where it is not one all the way
/// down.
///
/// A literal, and a list or object of literals; anything that has to be read at
/// run time is not a constant. This is what the grammar's constant positions --
/// a header's default -- are checked with.
std::optional<Constant> ConstantValue(const Node* node);

/// `a11.sdk.AudioBuffer` for a chain of plain names, or `nullopt`.
///
/// A tag is the only thing on the left of a `{` that means a type, and it arrives
/// as the same `Name`/`Attr` chain any other dotted reference does.
std::optional<std::string> DottedName(const Node* node);

}  // namespace a11::flow::syntax

#endif  // A11_FLOW_SYNTAX_H_
