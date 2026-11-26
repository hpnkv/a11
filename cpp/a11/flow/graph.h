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

#ifndef A11_FLOW_GRAPH_H_
#define A11_FLOW_GRAPH_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/time/time.h>
#include <absl/types/span.h>

#include "a11/flow/syntax.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow::graph {

/// Graph IDs are indices into the owning graph's vectors.
using RefId = size_t;
using StepId = size_t;
using BodyId = size_t;
using ExprId = size_t;

/// Sentinel for an absent graph ID.
inline constexpr size_t kNone = static_cast<size_t>(-1);

/// What a stream in the plan *is*.
enum class RefKind {
  /// A virtual continuation destination and its attached writer group.
  kSink,
  /// A declared port of this flow.
  kFlowPort,
  /// A port of an action this flow calls: `x.out`.
  kCallPort,
  /// A node of the flow's own: a stream it can write and read back.
  kNode,
  /// A node's id, as one value -- what a flow hands to an action that expects
  /// to be told where to write.
  kNodeId,
  /// The ID assigned to a run or call, as one value.
  kCallId,
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
  /// Zero-based index of the first subject to finish a `wait first of`.
  kWinner,
  /// A stream the runtime binds per pass: a loop's value, its index, or a
  /// `repeat`'s carry.
  kBound,
};

/// Resolved `log` or `logf` arguments for a statement or stage.
struct LogTail {
  /// The level, canonically spelled, or empty for the default.
  std::string level;
  /// `logf`'s format. Empty and `has_format` false for a `log`.
  std::string format;
  bool has_format = false;
  /// What to log, or what fills the format. Empty in a stage means `it`.
  std::vector<ExprId> arguments;
  /// Source line included in the log record.
  int line = 0;
};

/// A resolved pipeline stage. `kAt` extracts a field from each stream value.
struct Stage {
  std::string name;
  /// Destination context used by a terminal reference stage.
  RefId destination = kNone;
  bool optional = false;
  std::optional<syntax::TypeExpression> type;
  vocabulary::StageArgument takes = vocabulary::StageArgument::kNone;
  /// `kNumber`: a constant count, or the runtime configuration when evaluated.
  long long count = 0;
  /// `kString`/`kOptionalString`: the text. Also the field name for `at`.
  std::string text;
  bool text_is_bytes = false;
  bool match_all = false;
  bool match_regex = false;
  std::string match_offset;
  /// `kExpression`: the expression, with `it` bound. `kNumber`/`kDuration`:
  /// scalar configuration evaluated once per activation, without `it`.
  ExprId expr = kNone;
  /// `context`: surrounding item counts, evaluated once at stage activation.
  ExprId context_before = kNone;
  ExprId context_after = kNone;
  bool context_numbered = false;
  ExprId context_base = kNone;
  /// `kStream`: the stream `then` reads after this one.
  RefId stream = kNone;
  /// `kLog`/`kLogFormat`: what was written after the stage name.
  LogTail log;
  /// Whether `at` was written as an index rather than a field name.
  bool indexed = false;
  long long index = 0;
  std::vector<std::string> destructured;
  std::vector<std::optional<syntax::TypeExpression>> destructured_types;
  /// `kDuration`: how long `timeout` waits for the next value, or `pace` leaves
  /// between two of them.
  absl::Duration duration;
  /// `sort`: whether the order is reversed.
  bool descending = false;
  /// `fold`: initial value, carried name, and its expression reference.
  syntax::Constant start;
  std::string carried;
  RefId carry = kNone;
  /// `try`: tolerate an item's evaluation failure; context may retain it.
  bool tolerant = false;
  /// `try ... failures into ref`: where a tolerated failure goes.
  RefId failures = kNone;
  /// `parallel n`: how many values this stage may work on at once.
  int parallel = 1;
  ExprId parallel_expression = kNone;
  /// Whether values leave in the order they arrived. A parallel stage puts them
  /// back in order unless `unordered` said not to.
  bool ordered = true;
};

/// One plan stream. `kind` determines which fields apply.
/// [Graph::Upstreams] and [Graph::ValueRefs] expose its dependencies.
struct Ref {
  StepId phase_gate = kNone;
  RefKind kind = RefKind::kExpr;
  /// How a reader would say it: `search.hits | truncate 200`.
  std::string label;
  /// The body this belongs to, which is where it is materialised.
  BodyId owner = kNone;
  /// Whether this flow may write the stream.
  bool writable = false;
  /// Whether the stream provably carries at most one value.
  /// Defaults to false when cardinality is unknown.
  bool unary = false;
  /// Values skipped before fan-out; multiple `skip n` statements accumulate.
  long long skip = 0;

  /// `kFlowPort`, `kCallPort`, `kNode`: the port or node name.
  std::string name;
  /// `kFlowPort`, `kCallPort`: which side it is on.
  syntax::PortDirection direction = syntax::PortDirection::kInput;
  /// `kFlowPort`, `kCallPort`: write canonical status chunks here.
  bool status_port = false;
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
  /// `kExpr`: the expression evaluated once.
  ExprId expr = kNone;
  /// `kDerived`: what it reads, and the stage applied to it.
  RefId source = kNone;
  Stage stage;
  /// `kBound`: which of a loop's streams this is -- `item`, `index`, `carry`.
  std::string role;
  StepId bound_by = kNone;
  /// `kZip` sources follow tuple order; `kMerge` follows arrival order.
  std::vector<RefId> sources;
};

/// What a statement became.
enum class StepKind {
  kCall,
  kPipe,
  kSkip,
  kWait,
  kClose,
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

/// Whether a step records its own outcome for a bound name.
inline bool RecordsOutcome(StepKind kind) {
  return kind == StepKind::kBlock || kind == StepKind::kForEach ||
         kind == StepKind::kRepeat || kind == StepKind::kPipe ||
         kind == StepKind::kWait || kind == StepKind::kClose;
}

/// Sparse joins retain observable owned-call and expression cleanup boundaries.
struct TransferJoin {
  RefId source = kNone;
  size_t target = kNone;
  bool expression = false;
};

/// Extra transfer metadata is absent on ordinary single-arrow statements.
struct TransferPlan {
  std::vector<RefId> sources;
  std::vector<RefId> destinations;
  std::vector<TransferJoin> joins;
  std::vector<StepId> destination_calls;
};

/// One resolved statement.
struct Step {
  StepId phase_gate = kNone;
  bool scope_barrier = false;
  std::vector<StepId> preceding_steps;
  StepKind kind = StepKind::kPipe;
  /// The name this step is known by: a bound name, or `action`/`for`/`if` with
  /// a `#2` after it where one label is used twice.
  std::string label;
  BodyId body = kNone;
  /// What it waits for.
  std::vector<StepId> after;
  syntax::Location location;
  bool deferred = false;
  StepId activation = kNone;

  /// `kCall`.
  std::string action;
  std::string mode;
  std::string node_map;
  std::optional<absl::Duration> timeout;
  StepId deadline = kNone;
  StepId timeout_value = kNone;
  bool tee = false;
  bool tolerant = false;
  std::vector<std::pair<std::string, ExprId>> headers;
  std::vector<std::string> forward;
  ExprId action_id = kNone;
  /// Every port of this call that the flow wired up, by `direction:name`.
  absl::flat_hash_map<std::string, RefId> ports;
  /// Its outcome, once something has asked for one.
  RefId status = kNone;

  /// `kPipe` reads and writes these refs. `kCapture` reads `source`.
  RefId source = kNone;
  RefId destination = kNone;
  std::shared_ptr<TransferPlan> transfer;
  /// The observed continuation group produced by this operation, if any.
  RefId sink = kNone;
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
  std::vector<ExprId> expression_cleanups;
  /// `kSkip`: `skip n port`, which claims no reader slot -- the count is
  /// applied where the stream is produced and this step has nothing left to do.
  std::optional<long long> count;
  /// `kCapture`: which slot of the enclosing loop it fills.
  std::string slot;

  /// `kWait`/`kClose`: the outcome read, and whether a bad one is this flow's
  /// business or the subject's.
  RefId outcome = kNone;
  /// `kWait`: the outcomes of a `wait first of` / `wait all of`, in the order
  /// they were written. Empty for the single-subject form, whose one outcome is
  /// `outcome` -- which is also the *first* of these, so anything reading a
  /// wait's outcome keeps working without knowing which form it was.
  std::vector<RefId> subjects;
  std::vector<bool> subject_tolerant;
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
  /// Canonical code written as a word, such as `fail not_found "..."`.
  /// `code` is unset when this field is set.
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
  ExprId parallel_expression = kNone;
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

  /// Outer values updated by a child body, with its per-body input versions.
  struct VariableUpdate {
    RefId result = kNone;
    std::vector<std::pair<BodyId, RefId>> inputs;
  };

  std::vector<VariableUpdate> variable_updates;
};

inline absl::Span<const RefId> TransferSources(const Step& step) {
  return step.transfer != nullptr && !step.transfer->sources.empty()
             ? absl::MakeConstSpan(step.transfer->sources)
             : absl::MakeConstSpan(
                   &step.source, step.source == kNone ? size_t{0} : size_t{1});
}

inline absl::Span<const RefId> TransferTargets(const Step& step) {
  return step.transfer != nullptr && !step.transfer->destinations.empty()
             ? absl::MakeConstSpan(step.transfer->destinations)
             : absl::MakeConstSpan(&step.destination, step.destination == kNone
                                                          ? size_t{0}
                                                          : size_t{1});
}

/// A block of steps: a flow's top level, or a loop or branch body.
struct Body {
  std::string label;
  BodyId parent = kNone;
  StepId owner_step = kNone;
  std::vector<StepId> steps;
};

/// A resolved expression and its stream dependencies.
/// `node` borrows the parse tree; `bound` maps names to value references.
struct Expr {
  bool materialized = false;
  const syntax::Node* absl_nullable node = nullptr;
  /// A linked module constant, available without a syntax-tree expression.
  std::optional<syntax::Constant> constant;
  std::vector<std::pair<const syntax::Node* absl_nonnull, RefId>> bound;

  struct ReferenceBinding {
    const syntax::Builtin* absl_nonnull node;
    RefId source = kNone;
    RefId destination = kNone;
  };

  std::vector<ReferenceBinding> references;
  /// The same refs, in the order they were first mentioned, for the analysis.
  std::vector<RefId> refs;
  BodyId activation = kNone;
  bool pipeline_owned = false;
};

/// The executable graph of one flow.
/// Expression nodes borrow the syntax tree; the parse result must outlive it.
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
  /// Destinations written by stages a step reads, including failure streams.
  /// These writer counts close when the step finishes.
  [[nodiscard]] std::vector<RefId> StageDestinations(StepId step) const;
  /// Written refs a step only watches, without writing them itself.
  [[nodiscard]] std::vector<RefId> Observed(StepId step) const;
  /// Every body inside this one, at any depth.
  [[nodiscard]] std::vector<BodyId> NestedBodies(BodyId body) const;
};

/// Reader and writer counts for the references owned by a body.
/// A node closes when its last writer finishes.
struct Analysis {
  BodyId body = kNone;
  /// Every ref this body owns.
  std::vector<RefId> refs;
  /// How many readers each of them has.
  absl::flat_hash_map<RefId, int> readers;
  /// Consumers sharing each value cursor; retire it after the last consumer.
  absl::flat_hash_map<RefId, int> value_consumers;
  /// Refs read inside a nested body. They are buffered once and replayed so
  /// each loop pass sees the same outer value.
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
/// Keeps step allocation and insertion into the owning body in one operation.
/// The resolver omits this builder on editor-only paths that do not need an
/// executable graph.
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

  /// Whether a stage reduces its input to one value, including `first 1`.
  static bool StageMakesOne(const Stage& stage) {
    if (vocabulary::ReducingStages().contains(stage.name)) {
      {
        return true;
      }
    }
    return stage.name == "first" && stage.count == 1;
  }

  /// Whether a stage produces at most one output per input value.
  /// Unknown stages default to false.
  static bool StagePreservesCount(const Stage& stage) {
    static const auto* const kPerValue = new absl::flat_hash_set<std::string>{
        "map", "at", "truncate", "text", "json", "packb", "strformat", "where",
        "mime", "distinct", "first", "last", "drop", "log", "logf", "scan",
        // `sort` reorders and keeps every value; `timeout` and `pace` say
        // *when* a value may pass and change nothing about which do.
        "sort", "timeout", "pace", "coerce", "destructure"};
    return kPerValue->contains(stage.name);
  }

  /// Appends a ref and derives cardinality from its kind and sources.
  /// Declared ports retain their supplied cardinality.
  RefId AddRef(Ref ref) {
    switch (ref.kind) {
      case RefKind::kHeader:
      case RefKind::kNodeId:
      case RefKind::kCallId:
      case RefKind::kStatus:
      case RefKind::kWinner:
      case RefKind::kExpr:
      case RefKind::kBound:
        // One header, one id, one status record, one winner, one evaluated
        // expression, and one value bound per pass of a loop.
        ref.unary = true;
        break;
      case RefKind::kSink:
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
