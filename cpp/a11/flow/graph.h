// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_GRAPH_H_
#define A11_FLOW_GRAPH_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/time/time.h>

#include "a11/flow/syntax.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow::graph {

/// Everything in a graph is named by index into the graph that owns it.
///
/// Not pointers: the vectors grow while the graph is built, and a plan is
/// handed across a language boundary and held for as long as a flow is
/// registered. An index survives both.
using RefId = size_t;
using StepId = size_t;
using BodyId = size_t;
using ExprId = size_t;

/// No such thing -- the absent id, for the many fields only some kinds use.
inline constexpr size_t kNone = static_cast<size_t>(-1);

/// What a stream in the plan *is*.
enum class RefKind {
  /// A declared port of this flow.
  kFlowPort,
  /// A port of an action this flow calls: `x.out`.
  kCallPort,
  /// A node of the flow's own: a stream it can write and read back.
  kNode,
  /// A node's id, as one value -- what a flow hands to an action that expects
  /// to be told where to write.
  kNodeId,
  /// The outcome of a call, a node, a port or a barrier, as a status record.
  kStatus,
  /// One header of the call running this flow, as a single value.
  kHeader,
  /// A stream of one value: an expression evaluated once.
  kExpr,
  /// One stage applied to another stream.
  kDerived,
  /// Several streams read in step, as one stream of tuples: `zip(a, b)`.
  kZip,
  /// Several streams read at once, as one stream of their values in the order
  /// they arrive: `interleave(a, b)`.
  kMerge,
  /// Which subject of a `wait first of` finished first, counted from zero, as
  /// one value. Produced by the barrier rather than read from anywhere, the way
  /// a status is produced by the thing it is the status of.
  kWinner,
  /// A stream the runtime binds per pass: a loop's value, its index, or a
  /// `repeat`'s carry.
  kBound,
};

/// One resolved pipeline stage.
///
/// `takes` says which field carries the argument, and it comes from the
/// vocabulary rather than being decided here: the stage table is the one table.
/// `kAt` is the one stage the grammar has no spelling for -- it is what
/// `x.field` and `x[0]` over a *stream* compile to, taking the field out of
/// each value. What a resolved `log`/`logf` was written with, in a statement or
/// a stage.
///
/// The resolved twin of [syntax::LogTail], and one struct for both places for
/// the same reason: `log warning it.error` means the same thing wherever it
/// stands, so the runtime reads it out of one shape.
struct LogTail {
  /// The level, canonically spelled, or empty for the default.
  std::string level;
  /// `logf`'s format. Empty and `has_format` false for a `log`.
  std::string format;
  bool has_format = false;
  /// What to log, or what fills the format. Empty in a stage means `it`.
  std::vector<ExprId> arguments;
  /// The line it was written on, which the log carries so a consumer can point
  /// at it. There is no path to go with it: a plan does not know where it was
  /// read from, and a flow's name is not a file.
  int line = 0;
};

struct Stage {
  std::string name;
  vocabulary::StageArgument takes = vocabulary::StageArgument::kNone;
  /// `kNumber`: the count.
  long long count = 0;
  /// `kString`/`kOptionalString`: the text. Also the field name for `at`.
  std::string text;
  /// `kExpression`: the expression, with `it` bound.
  ExprId expr = kNone;
  /// `kStream`: the stream `then` reads after this one.
  RefId stream = kNone;
  /// `kLog`/`kLogFormat`: what was written after the stage name.
  LogTail log;
  /// Whether `at` was written as an index rather than a field name.
  bool indexed = false;
  long long index = 0;
  /// Whether `at` should try its name and *then* its index, which is what a
  /// destructuring `let` needs: `let name, age = user` is by field over a
  /// record and by position over a pair, and only the value knows which it is.
  bool named_or_indexed = false;
  /// `kDuration`: how long `timeout` waits for the next value, or `pace` leaves
  /// between two of them.
  absl::Duration duration;
  /// `sort`: whether the order is reversed.
  bool descending = false;
  /// `fold`: what the first pass carries, the name it is bound to, and the ref
  /// that name resolves to inside the fold's expression. The ref is how the
  /// accumulator reaches [Evaluate]: a name in an expression is a ref, and this
  /// is the one the runtime fills in per value rather than reading a stream
  /// for.
  syntax::Constant start;
  std::string carried;
  RefId carry = kNone;
  /// `try`: a value this stage cannot do is dropped rather than ending the
  /// pipeline.
  bool tolerant = false;
  /// `try ... into ref`: where a tolerated failure goes, as a status record.
  RefId failures = kNone;
  /// `parallel n`: how many values this stage may work on at once.
  int parallel = 1;
  /// Whether values leave in the order they arrived. A parallel stage puts them
  /// back in order unless `unordered` said not to.
  bool ordered = true;
};

/// One stream in the plan.
///
/// A tagged struct rather than a class hierarchy: which fields mean anything
/// depends on `kind`, and the runtime switches on it anyway. What every kind
/// shares is the two questions the analysis asks -- what it reads as a stream,
/// and what it reads for one value -- and those are answered by
/// [Graph::Upstreams] and [Graph::ValueRefs] rather than by each kind
/// separately.
struct Ref {
  RefKind kind = RefKind::kExpr;
  /// How a reader would say it: `search.hits | truncate 200`.
  std::string label;
  /// The body this belongs to, which is where it is materialised.
  BodyId owner = kNone;
  /// Whether this flow could ever write it, which is what makes it a
  /// destination rather than something finished by being read to its end.
  bool writable = false;
  /// Whether this stream provably carries at most one value.
  ///
  /// A *claim*, so the default is the absence of one: something that forgets to
  /// say is treated as a stream, which costs a check rather than being wrong. A
  /// declared port says so (`in q: string` against `in q: string stream`), an
  /// action's schema says so for its own ports, and everything built out of
  /// those is derived from them by [Builder::AddRef] -- a reducing stage makes
  /// one value out of many, a per-value stage keeps the count it was given, and
  /// a node the flow writes from anywhere is never one.
  ///
  /// What it is *for*: reading a stream where a value is expected. A unary
  /// stream can be consumed -- take the value, and a second one is an error the
  /// language can name -- while a stream of many has to say which value it
  /// means.
  bool unary = false;
  /// How many of this stream's first values `skip n` has spoken for.
  ///
  /// Applied where the stream is produced, upstream of the fan-out, so it is
  /// the same values every reader does not see. Several `skip n` statements
  /// naming one node add up: the ref is one object however many times the flow
  /// mentions it.
  long long skip = 0;

  /// `kFlowPort`, `kCallPort`, `kNode`: the port or node name.
  std::string name;
  /// `kFlowPort`, `kCallPort`: which side it is on.
  syntax::PortDirection direction = syntax::PortDirection::kInput;
  /// `kCallPort`: the call it belongs to.
  StepId call = kNone;
  /// `kNode`: the id expression it attaches to, and the map it lands in.
  ExprId id_expr = kNone;
  std::string node_map;
  /// `kNodeId`: the node. `kStatus`: the node or port it is the outcome of.
  RefId subject = kNone;
  /// `kStatus`: the call or barrier it is the outcome of.
  StepId subject_step = kNone;
  /// `kHeader`: the header name, and the default when it was not sent.
  std::string header;
  syntax::Constant fallback;
  bool has_fallback = false;
  /// `kExpr`: the expression evaluated once.
  ExprId expr = kNone;
  /// `kDerived`: what it reads, and the stage applied to it.
  RefId source = kNone;
  Stage stage;
  /// `kBound`: which of a loop's streams this is -- `item`, `index`, `carry`.
  std::string role;
  StepId bound_by = kNone;
  /// `kZip`: the streams read in step, in the order they were written, which is
  /// the order their values appear in each tuple. `kMerge`: the streams read at
  /// once, where the order they were written in says nothing about the order
  /// their values arrive in.
  std::vector<RefId> sources;
};

/// What a statement became.
enum class StepKind {
  kCall,
  kPipe,
  kSkip,
  kWait,
  kDrain,
  kCancel,
  /// `abort node ..`: end a node with a failure rather than with an end.
  kAbort,
  kFail,
  kLog,
  /// Remember a stream's first value for the loop that owns this body: what
  /// `<-` and an `until` condition compile to.
  kCapture,
  kForEach,
  kRepeat,
  kIf,
  /// `[try] { ... }`: a body run as one step, whose outcome is its own.
  kBlock,
};

std::string_view StepKindName(StepKind kind);

/// Whether a step records an outcome of its own for a name to read.
///
/// The steps that run a nested body: what makes them different from a `kCall`
/// is that their status is not a *call's* status to be asked for, it is theirs
/// to have recorded. A name bound to one reads what it recorded, which is why
/// `s = try { .. }` and `done = for x in s { .. }` are the same shape.
inline bool RecordsOutcome(StepKind kind) {
  return kind == StepKind::kBlock || kind == StepKind::kForEach ||
         kind == StepKind::kRepeat || kind == StepKind::kPipe;
}

/// One resolved statement.
struct Step {
  StepKind kind = StepKind::kPipe;
  /// The name this step is known by: a bound name, or `action`/`for`/`if` with
  /// a `#2` after it where one label is used twice.
  std::string label;
  BodyId body = kNone;
  /// What it waits for.
  std::vector<StepId> after;
  syntax::Location location;

  /// `kCall`.
  std::string action;
  std::string mode;
  std::string node_map;
  std::optional<absl::Duration> timeout;
  bool tee = false;
  bool tolerant = false;
  std::vector<std::pair<std::string, ExprId>> headers;
  std::vector<std::string> forward;
  ExprId action_id = kNone;
  /// Every port of this call that the flow wired up, by `direction:name`.
  absl::flat_hash_map<std::string, RefId> ports;
  /// Its outcome, once something has asked for one.
  RefId status = kNone;

  /// `kPipe`: what it reads and what it writes. `kSkip`/`kCapture`: what it
  /// reads.
  RefId source = kNone;
  RefId destination = kNone;
  /// `kPipe`: `-> _`, which reads the stream and keeps nothing.
  ///
  /// The opposite end of the language from a counted `skip`: that one is
  /// *elided*, because the values were already taken where the stream is
  /// produced and the step has nothing left to do. A discard is performed --
  /// every stage on the pipeline runs, and the values are dropped only once
  /// there is nowhere left for them to go. So this is a step with a reader slot
  /// (see [FlowGraph::Sources]) and no destination at all: `destination` is
  /// `kNone`, nothing counts as a writer of anything, and no data is kept.
  bool discard = false;
  /// `kSkip`: `skip n port`, which claims no reader slot -- the count is
  /// applied where the stream is produced and this step has nothing left to do.
  std::optional<long long> count;
  /// `kSkip`: the call `skip act` named, when its real ports are not known here
  /// (an action from a registry, not a sibling flow) so there is nothing to set
  /// `source` to. Purely informational -- the runtime already drains every
  /// output of a call that nothing reads, whether or not this step exists.
  StepId call = kNone;
  /// `kCapture`: which slot of the enclosing loop it fills.
  std::string slot;

  /// `kWait`/`kDrain`: the outcome read, and whether a bad one is this flow's
  /// business or the subject's.
  RefId outcome = kNone;
  /// `kWait`: the outcomes of a `wait first of` / `wait all of`, in the order
  /// they were written. Empty for the single-subject form, whose one outcome is
  /// `outcome` -- which is also the *first* of these, so anything reading a
  /// wait's outcome keeps working without knowing which form it was.
  std::vector<RefId> subjects;
  /// `kWait`: whether the first subject to finish is enough.
  bool race = false;
  /// `kWait`: the value a race is -- which subject won -- for whoever reads it.
  /// `kNone` where nothing does, and on the forms that have no winner.
  RefId winner = kNone;

  /// `kCancel`: the call to stop.
  StepId target = kNone;

  /// `kFail`.
  ExprId code = kNone;
  ExprId message = kNone;
  /// The canonical code `fail` named outright: `fail not_found "..."`.
  ///
  /// Kept apart from `code` because a written-out name is not an expression --
  /// nothing in scope is called `not_found` -- and the runtime should not have
  /// to decide which of the two a bare word was. Where this is set, `code` is
  /// not.
  std::string code_name;

  /// `kLog`: the level, the format and what fills it.
  LogTail log;

  /// `kForEach`, `kRepeat`, `kIf`: the bodies nested here, in reading order. An
  /// `if` has two; the loops have one.
  std::vector<BodyId> bodies;
  /// `kForEach`: the value and the index each pass binds.
  RefId item = kNone;
  RefId index = kNone;
  int parallel = 1;
  /// `kRepeat`: what it carries, where the next pass's value comes from, and
  /// when it stops.
  RefId carry = kNone;
  RefId carry_source = kNone;
  syntax::Constant start;
  /// `max n`, where one was written; nothing means the loop is bounded only by
  /// its condition. See [syntax::Repeat::max_iterations].
  std::optional<int> max_iterations;
  ExprId condition = kNone;
  bool stop_when = true;
};

/// A block of steps: a flow's top level, or a loop or branch body.
struct Body {
  std::string label;
  BodyId parent = kNone;
  StepId owner_step = kNone;
  std::vector<StepId> steps;
};

/// One resolved expression, and the streams it reads.
///
/// `node` is borrowed from the parse tree, which the graph does not own -- see
/// [Graph]. `bound` is how a name inside an expression becomes a value: the
/// runtime reads each ref's first value and hands [Evaluate] a map keyed by the
/// syntax node that named it. Keying on the node rather than rewriting the tree
/// is what lets the tree stay immutable and shared.
struct Expr {
  const syntax::Node* absl_nullable node = nullptr;
  std::vector<std::pair<const syntax::Node* absl_nonnull, RefId>> bound;
  /// The same refs, in the order they were first mentioned, for the analysis.
  std::vector<RefId> refs;
};

/// The executable graph of one flow.
///
/// **Borrows the syntax tree.** Every expression here points into the
/// [ParseResult] the flow was resolved from, because an expression is evaluated
/// by walking it and copying the tree per flow would be paying for a second
/// one. So a graph is only valid while that parse result lives, and whatever
/// owns a registered flow owns both.
struct FlowGraph {
  std::string name;
  std::vector<Ref> refs;
  std::vector<Step> steps;
  std::vector<Body> bodies;
  std::vector<Expr> exprs;
  /// The flow's top level.
  BodyId root = kNone;

  /// The refs this one reads *as streams* to produce itself.
  [[nodiscard]] std::vector<RefId> Upstreams(RefId ref) const;
  /// The refs this one reads for their first value to produce itself.
  [[nodiscard]] std::vector<RefId> ValueRefs(RefId ref) const;

  /// The refs a step reads as streams, one entry per independent read.
  [[nodiscard]] std::vector<RefId> Sources(StepId step) const;
  /// The refs a step reads for one value each.
  [[nodiscard]] std::vector<RefId> ValueSources(StepId step) const;
  /// The refs a step writes, one entry per independent writer.
  [[nodiscard]] std::vector<RefId> Destinations(StepId step) const;
  /// The refs the *stages* a step reads write to: a `try ... into failures`.
  ///
  /// A stage is part of a stream rather than of a statement, so its destination
  /// is reached through the refs the step reads rather than off the step
  /// itself. Counted as a writer beside [Destinations] and released with the
  /// step, which is what closes a failure stream exactly once however many
  /// values went to it.
  [[nodiscard]] std::vector<RefId> StageDestinations(StepId step) const;
  /// Written refs a step only watches, without writing them itself.
  [[nodiscard]] std::vector<RefId> Observed(StepId step) const;
  /// Every body inside this one, at any depth.
  [[nodiscard]] std::vector<BodyId> NestedBodies(BodyId body) const;
};

/// Who reads and who writes each ref a body owns.
///
/// The counts are the load-bearing part: a stream gets exactly as many readers
/// as the plan says it has, and a node is closed exactly when the last of its
/// writers finishes -- including a loop or a branch, which counts as one writer
/// for as long as it runs. Getting a count wrong is a flow that hangs, not a
/// flow that is slightly off.
struct Analysis {
  BodyId body = kNone;
  /// Every ref this body owns.
  std::vector<RefId> refs;
  /// How many readers each of them has.
  absl::flat_hash_map<RefId, int> readers;
  /// The ones read from inside a nested body, which are buffered once and
  /// replayed to each reader: that is what lets every pass of a loop see the
  /// same outer value, and it is the one place the language trades streaming
  /// for repeatability.
  absl::flat_hash_set<RefId> materialise;
  /// How many writers each written ref has.
  absl::flat_hash_map<RefId, int> writers;
  /// Every ref this body writes, whether or not anything reads it back.
  std::vector<RefId> destinations;
  /// Which destinations each step holds open until it finishes.
  absl::flat_hash_map<StepId, std::vector<RefId>> held;
  /// The nodes of its own this body names, wherever it names them.
  std::vector<RefId> nodes;
};

/// Work out who reads and writes what in one body.
Analysis Analyse(const FlowGraph& flow, BodyId body);

/// Appends to a [FlowGraph] while the resolver walks a flow.
///
/// Deliberately thin: the graph is vectors, and the only thing worth having a
/// type for is that a step is appended to its own body's list at the moment it
/// gets its id, so the two cannot fall out of step. The resolver holds one of
/// these, or none at all on the editor path -- `a11 flow check` pays nothing
/// for a graph nobody runs.
class GraphBuilder {
 public:
  explicit GraphBuilder(FlowGraph& flow) : flow_(&flow) {}

  FlowGraph& flow() { return *flow_; }

  BodyId AddBody(std::string label, BodyId parent = kNone,
                 StepId owner_step = kNone) {
    Body body;
    body.label = std::move(label);
    body.parent = parent;
    body.owner_step = owner_step;
    flow_->bodies.push_back(std::move(body));
    return flow_->bodies.size() - 1;
  }

  /// Whether the ref already in the graph carries at most one value.
  [[nodiscard]] bool Carries(RefId ref) const {
    return ref != kNone && ref < flow_->refs.size() && flow_->refs[ref].unary;
  }

  /// Whether a stage yields exactly one value however many it was given.
  ///
  /// The reducing three, and `first 1`, which is how a pipeline says "the
  /// value" out loud. Read from [vocabulary::ReducingStages] rather than listed
  /// again, so a stage that joins that set is counted here without being
  /// taught.
  static bool StageMakesOne(const Stage& stage) {
    if (vocabulary::ReducingStages().contains(stage.name)) {
      {
        return true;
      }
    }
    return stage.name == "first" && stage.count == 1;
  }

  /// Whether a stage yields one value per value it was given.
  ///
  /// Then one in gives one out. `map`, `at`, `truncate`, `text`, `json`,
  /// `packb` and `strformat` reshape each value; `log` and `logf` reshape
  /// nothing and pass each value straight on; `where`, `mime` and `distinct`
  /// may drop one, which is still at most one out per one in; `scan` publishes
  /// the state it reached at each value, so one in gives exactly one out;
  /// `batch` and `group` gather several into a list, which is *fewer*, as is
  /// `window` -- its first `n - 1` values produce nothing -- and `chunk` and
  /// `then` make more. Anything the language gains is assumed not to preserve
  /// the count until it says so, which is the safe direction.
  static bool StagePreservesCount(const Stage& stage) {
    static const auto* const kPerValue = new absl::flat_hash_set<std::string>{
        "map", "at", "truncate", "text", "json", "packb", "strformat", "where",
        "mime", "distinct", "first", "last", "drop", "log", "logf", "scan",
        // `sort` reorders and keeps every value; `timeout` and `pace` say
        // *when* a value may pass and change nothing about which do.
        "sort", "timeout", "pace"};
    return kPerValue->contains(stage.name);
  }

  /// Append a ref, working out what it carries.
  ///
  /// Unarity is settled here rather than at each of the dozen places a ref is
  /// made, because it is a property of the *shape* of the ref for every kind
  /// but the two that name a declared port -- and a kind added to the language
  /// then gets an answer by default instead of silently keeping whatever the
  /// struct's initialiser said.
  RefId AddRef(Ref ref) {
    switch (ref.kind) {
      case RefKind::kHeader:
      case RefKind::kNodeId:
      case RefKind::kStatus:
      case RefKind::kWinner:
      case RefKind::kExpr:
      case RefKind::kBound:
        // One header, one id, one status record, one winner, one evaluated
        // expression, and one value bound per pass of a loop.
        ref.unary = true;
        break;
      case RefKind::kNode:
        // A node is a stream the flow may write from anywhere, including from
        // inside a loop, so nothing about it is provable from the plan.
        ref.unary = false;
        break;
      case RefKind::kDerived:
        ref.unary = StageMakesOne(ref.stage) ||
                    (Carries(ref.source) && StagePreservesCount(ref.stage));
        break;
      case RefKind::kZip:
        // A tuple per round, and the rounds run until every source has ended,
        // so one round is only certain when every source has at most one value.
        ref.unary = !ref.sources.empty();
        for (const RefId source : ref.sources) {
          if (!Carries(source)) {
            {
              ref.unary = false;
            }
          }
        }
        break;
      case RefKind::kMerge:
        // Every value of every source, so one value only when they all have at
        // most one *and* there is one of them: two unary sources interleave
        // into two values.
        ref.unary = ref.sources.size() == 1 && Carries(ref.sources.front());
        break;
      case RefKind::kFlowPort:
      case RefKind::kCallPort:
        // Whatever the declaration said, which only the caller knows.
        break;
    }
    flow_->refs.push_back(std::move(ref));
    return flow_->refs.size() - 1;
  }

  /// Append a step, and record it in the body it belongs to.
  StepId AddStep(Step step) {
    const BodyId body = step.body;
    flow_->steps.push_back(std::move(step));
    const StepId id = flow_->steps.size() - 1;
    if (body != kNone) {
      {
        flow_->bodies[body].steps.push_back(id);
      }
    }
    return id;
  }

  ExprId AddExpr(Expr expr) {
    flow_->exprs.push_back(std::move(expr));
    return flow_->exprs.size() - 1;
  }

  Ref& ref(RefId id) { return flow_->refs[id]; }

  Step& step(StepId id) { return flow_->steps[id]; }

  Expr& expr(ExprId id) { return flow_->exprs[id]; }

 private:
  FlowGraph* absl_nonnull flow_;
};

}  // namespace a11::flow::graph

#endif  // A11_FLOW_GRAPH_H_
