// Copyright 2026 The A11 Authors.

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
  /// Whether a `list[T]` was written `T[]`.
  ///
  /// The two are the same type and everything but the formatter treats them as
  /// one; this is only so a document formatted twice reads the way its author
  /// wrote it.
  bool sugared = false;

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
  kSpread,
  kIt,
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
  kPipelineValue,

  kCallModifiers,
  kCallExpression,

  kBind,
  kLet,
  kAdvance,
  kBlock,
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

/// `...expr` -- everything `expr` holds, in the literal being written.
///
/// Only ever an item of a list literal or a pair of an object literal, which is
/// why it is a node rather than a flag on those: a reader that has not been
/// taught about spreading meets a node kind it does not know and says so,
/// instead of quietly reading `...it` as `it`.
struct Spread : NodeOf<NodeKind::kSpread> {
  NodePtr value;
};

/// `[a, ...rest, c]`. An item may be a [Spread].
struct ListLiteral : NodeOf<NodeKind::kListLiteral> {
  std::vector<NodePtr> items;
};

/// `{ "key": expr, ...rest }`.
///
/// A pair whose value is a [Spread] is one: its key is empty and means nothing,
/// and it contributes every pair of what it holds at the point it is written.
/// Later pairs win, so `{...it, "tags": [..]}` overrides and `{"tags": [..], ...it}`
/// does not.
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

/// `zip(a, b, c)` -- several streams read in step, as one stream of tuples.
///
/// Not a [Builtin], though it is spelled like one: a builtin takes *values* and
/// this takes *streams*, so it stands only where a pipeline's source does and
/// the resolver has to resolve each argument as a reference rather than evaluate
/// it. Keeping the two apart is what stops `len(zip(a, b))` from looking legal.
struct Zip : NodeOf<NodeKind::kZip> {
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

/// `let name = pipeline` -- one value, read from a stream and given a name.
///
/// **Why the language needs this.** Everything else here is a stream, and a
/// stream is the right default: a flow is dataflow, and most of what moves
/// through it is many values. But some of what moves through it is one value --
/// a status code, an image, a summary -- and until now a flow could only reach
/// one *inside* an expression, with `(x | first 1)` written out at every use.
/// A name that is a value can be compared, added, indexed and branched on
/// directly, which is what makes `if code >= 200 and code < 300` say what it
/// means.
///
/// **Why it is a word and not an operator.** `name = ...` already says "a step
/// of this flow"; the thing that differs here is the *kind of thing the name
/// is*, and that is worth a word at the front of the line where a reader
/// scanning the left margin sees it. It reads as one, too: `let code = ...`.
struct Let : NodeOf<NodeKind::kLet> {
  /// One name is the value; several take it apart -- `let name, age = user` by
  /// field, `let first, second = pair` by position. Which of the two is meant is
  /// a question about the value rather than about the text, so it is answered
  /// where the value is: by name, and by position where there is no such field.
  std::vector<Word> names;
  PipelinePtr pipeline;

  /// The first name, which is the whole value where there is only one.
  const Word& name() const {
    static const Word kNone;
    return names.empty() ? kNone : names.front();
  }
};

/// `[try] { ... }` -- a block of statements that runs as one thing.
///
/// Everything in a flow's body runs at once, which is the point of it; a block is
/// how a flow says "these together, and *this* is what came of them". Inside it
/// the ordinary rules hold, so its own statements are concurrent with each other
/// and a condition in it blocks only what is in it. Bound to a name it reads as a
/// status, exactly as a call does, and `try` is what says a failure inside is the
/// flow's to handle rather than the end of it.
struct Block : NodeOf<NodeKind::kBlock> {
  bool tolerant = false;
  std::vector<NodePtr> body;
};

/// `advance name` -- rebind a `let` value to the *next* value of its stream.
///
/// The concise form of writing the `let` again for the value after the one it
/// has. What it buys is the guarantee a second `let` cannot give on its own:
/// which value each use of the name sees.
struct Advance : NodeOf<NodeKind::kAdvance> {
  Word name;
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

/// `for name[, name...] in pipeline [parallel n] { ... }`.
///
/// Several names take the value apart by position -- `for url, title in
/// zip(urls, titles)` -- which is what makes `zip` worth having: the alternative
/// is one name and `it[0]` everywhere, and a tuple whose parts have names reads
/// like the two streams it came from.
struct ForEach : NodeOf<NodeKind::kForEach> {
  std::vector<Word> variables;
  PipelinePtr pipeline;
  int parallel = 1;
  std::vector<NodePtr> body;

  /// The first name, which is the whole value where there is only one.
  const Word& variable() const {
    static const Word kNone;
    return variables.empty() ? kNone : variables.front();
  }
};

/// `repeat [name = expr] [max n] { ... }`.
struct Repeat : NodeOf<NodeKind::kRepeat> {
  /// Empty where the repeat carries nothing.
  Word variable;
  NodePtr start;
  /// `max n`, where one was written. Nothing means no bound: the loop runs until
  /// its `until`/`while` says to stop.
  ///
  /// There used to be a default of 16 here, which meant a `repeat` whose
  /// condition never held stopped after sixteen passes and reported *success*.
  /// A silent bound presented as a clean finish is worse than either an honest
  /// loop or an honest error, so a bound is now only ever the author's.
  std::optional<int> max_iterations;
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

/// A bound on a field: `1..200`, `1..`, `..200`.
///
/// What it bounds depends on what the field holds -- the *value* of a number, a
/// duration or an instant, and the *length* of a string, a byte string or a
/// list. One spelling for both because it is one idea, and because which one is
/// meant is never in doubt once the type is known.
struct FieldRange {
  bool has_minimum = false;
  bool has_maximum = false;
  Constant minimum;
  Constant maximum;

  bool Empty() const { return !has_minimum && !has_maximum; }
};

/// One `name: type [modifiers] ["description"]` field of a `struct`.
struct FieldDeclaration : NodeOf<NodeKind::kFieldDeclaration> {
  Word name;
  TypeExpression type;
  /// Whether a value has to be given. A field that is not required and has no
  /// default is simply absent when it was not sent.
  bool required = false;
  /// `unique`: no two items of a list are equal.
  bool unique = false;
  FieldRange range;
  /// `matching "..."`: the pattern every value has to match, unanchored, as
  /// JSONSchema's `pattern` is.
  std::string pattern;
  bool has_pattern = false;
  /// `one of [..]`: the only values allowed.
  std::vector<Constant> enumeration;
  bool has_enumeration = false;
  /// `default ..`: what a value that was not given is.
  Constant default_value;
  bool has_default = false;
  std::string description;
};

using FieldDeclarationPtr = std::unique_ptr<FieldDeclaration>;

/// One `struct name { ... }` declaration: a shape a port may be typed with.
///
/// A sibling of [FlowDeclaration] rather than something inside one, because a
/// shape is not a flow's private business: two flows in a file describe the same
/// records, and a caller reading the file wants the type once.
struct DtoDeclaration : NodeOf<NodeKind::kDtoDeclaration> {
  Word name;
  std::string description;
  std::vector<FieldDeclarationPtr> fields;
};

using DtoDeclarationPtr = std::unique_ptr<DtoDeclaration>;

/// The constant `node` folds to, or `nullopt` where it is not one all the way
/// down.
///
/// A literal, and a list or object of literals; anything that has to be read at
/// run time is not a constant. This is what the grammar's constant positions --
/// a header's default -- are checked with.
std::optional<Constant> ConstantValue(const Node* node);

/// Every node `node` directly holds, in the order they were written.
///
/// One place that knows the shape of the tree, so a pass that only cares about
/// *some* node kind -- which types a body names, where a symbol is declared --
/// says so and lets this find them, rather than restating the grammar. A pass
/// that needs to treat each kind differently still switches on the kind; this is
/// for the ones that do not.
///
/// Only the tree: a [TypeExpression] is a value on a node rather than a node, and
/// is reached through the node that holds it.
void VisitChildren(const Node& node,
                   const std::function<void(const Node&)>& visit);

/// `a11.sdk.AudioBuffer` for a chain of plain names, or `nullopt`.
///
/// A tag is the only thing on the left of a `{` that means a type, and it arrives
/// as the same `Name`/`Attr` chain any other dotted reference does.
std::optional<std::string> DottedName(const Node* node);

}  // namespace a11::flow::syntax

#endif  // A11_FLOW_SYNTAX_H_
