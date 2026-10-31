// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef A11_FLOW_INTERNAL_EXECUTION_H_
#define A11_FLOW_INTERNAL_EXECUTION_H_

#include "a11/flow/internal/runtime_layout.h"

namespace a11::flow::internal {

class Runner;

/// One running instance of a body: the flow's top level, or a loop pass.
class Scope {
 public:
  Scope(Runner& runner, BodyId body, Scope* absl_nullable parent,
        absl::flat_hash_map<RefId, std::vector<ItemPtr>> presets)
      : runner_(&runner),
        body_(body),
        parent_(parent),
        presets_(std::move(presets)) {}

  ~Scope();
  absl::Status Run();

 private:
  friend class Runner;

  absl::Status Prepare();
  absl::Status EnsureStep(StepId step);
  void SkipRef(RefId ref);
  void RetireValueRef(RefId ref);
  bool RetireValueReservation(RefId ref);
  absl::Status RunStep(StepId step);
  absl::Status Execute(StepId step);
  absl::Status StepDone(StepId step);
  absl::Status OperationDone(StepId step);
  absl::Status CompleteStep(StepId step, absl::Status status);
  void CallFinished(StepId step, const absl::Status& status);
  absl::Status ReleaseWriter(RefId ref, const absl::Status& status);
  void WatchVirtual(StepId step, const a11::Task* absl_nullable task);
  absl::Status AbortDestinations(StepId step, absl::Status reason);
  absl::InlinedVector<Destination*, 1> PhysicalDestinations(
      StepId step, bool include_reserved = false);
  void MarkDone(StepId step, const absl::Status& status);

  absl::Status RunForEach(StepId step);
  absl::Status RunRepeat(StepId step);
  absl::StatusOr<bool> PassCondition(const graph::Step& one, const Scope& pass);
  void Record(StepId step, const absl::Status& outcome);
  void PublishVariables(const graph::Step& step, const Scope& child);
  void PresetVariables(
      const graph::Step& step, BodyId body,
      absl::flat_hash_map<RefId, std::vector<ItemPtr>>& presets);
  absl::StatusOr<absl::Status> ChosenStatus(StepId step);
  absl::Status AbortNode(StepId step);
  absl::StatusOr<absl::Time> DeadlineAt(const graph::Step& step);

  std::shared_ptr<a11::Promise<a11::Unit>> ExpressionCleanup(ExprId expr);

  absl::Status RunWait(StepId step);
  absl::Status RunWaitMany(StepId step);
  absl::Status Failure(StepId step);
  absl::Status WriteLog(const graph::LogTail& tail, const ItemPtr& subject);
  absl::Status LogValue(const Value& value, const Item* absl_nullable carried,
                        const actions::LogOptions& options);
  absl::Status Logged(const absl::Status& logged);

  /// An independent view of a ref's values, for one reader.
  absl::StatusOr<ReaderPtr> Subscribe(RefId ref);
  Scope* absl_nonnull Owner(RefId ref);
  Scope* absl_nullable FindOwner(RefId ref);
  Destination* absl_nullable FindDestination(RefId ref);
  absl::StatusOr<Destination*> DestinationOf(RefId ref);
  absl::StatusOr<CallHandle*> Call(StepId step);

  absl::StatusOr<ItemPtr> BoundItem(RefId ref);
  absl::Status Produce(RefId ref, const Sink& sink);
  absl::Status ProduceStage(RefId ref, const Sink& sink);
  absl::Status ProduceZip(RefId ref, const Sink& sink);
  absl::Status ProduceMerge(RefId ref, const Sink& sink);
  absl::StatusOr<ItemPtr> StatusItem(RefId ref);
  absl::StatusOr<ItemPtr> WinnerItem(RefId ref);
  absl::Status NodeOutcome(RefId ref, absl::Status* absl_nonnull outcome);
  absl::StatusOr<NodePtr> ReadableNode(RefId ref);
  absl::StatusOr<NodePtr> DestinationNode(RefId ref);
  absl::StatusOr<NodePtr> LocalNode(RefId ref);
  absl::StatusOr<NodePtr> MakeLocalNode(RefId ref);

  /// Read a node as items, each keeping the producer's own chunk.
  absl::Status ReadNode(const NodePtr& node, const Sink& sink);

  /// Run a per-value stage with the requested fixed concurrency.
  ///
  /// Workers serialize reads from the shared cursor and process values
  /// concurrently. Results retain input order unless the stage is `unordered`.
  absl::Status InParallel(
      const graph::Stage& stage, Reader& source, const Sink& sink,
      absl::FunctionRef<absl::Status(const ItemPtr&, std::vector<ItemPtr>&)>
          body,
      bool reads_values);

  absl::StatusOr<Value> ValueOf(RefId ref, bool take = false,
                                bool optional = false);
  absl::StatusOr<Value> Evaluate(ExprId expr);
  absl::StatusOr<int> Parallelism(int count, ExprId expression);
  absl::StatusOr<Value> EvaluateWith(ExprId expr, const Value& it,
                                     bool join = true);
  /// What an aggregating stage compares or adds: the value itself, or what its
  /// expression makes of it. `| sum` and `| sum it.price` differ by this and
  /// nothing else, and so do `| min` and `| sort by`.
  absl::StatusOr<Value> StageValue(const graph::Stage& stage,
                                   const ItemPtr& item, HostBridge& host);
  absl::Status Tolerated(const graph::Stage& stage, const ItemPtr& item,
                         const absl::Status& why, HostBridge& host);
  /// One pass of a `fold`: the accumulator bound to its name, `it` to the
  /// value.
  absl::StatusOr<Value> EvaluateFold(const graph::Stage& stage,
                                     const Value& carried, const Value& it);

  [[nodiscard]] const FlowGraph& graph() const;
  [[nodiscard]] Monitor& monitor() const;
  [[nodiscard]] HostBridge& bridge() const;
  /// The shapes the program declared, for a cast or a coercion in this body.
  [[nodiscard]] const Program& shapes() const;

  Runner* absl_nonnull runner_;
  BodyId body_ = kNone;
  Scope* absl_nullable parent_ = nullptr;
  absl::flat_hash_map<RefId, std::vector<ItemPtr>> presets_;
  /// Who reads and writes what in this body -- the run's copy, not one of its
  /// own: a `for` runs one Scope per pass, and the answer is a property of the
  /// body rather than of the pass. See [Runner::AnalysisOf].
  const graph::Analysis* absl_nonnull analysis_ = nullptr;
  internal::FrameObjects<Bus> buses_;
  absl::flat_hash_map<RefId, std::shared_ptr<SingleSlot>> single_slots_;
  internal::FrameObjects<Lazy> lazies_;
  internal::FrameObjects<Buffer> buffers_;

  using WriterGroup = RuntimeLayouts::WriterGroup;
  internal::FrameObjects<WriterGroup> writer_groups_;
  absl::flat_hash_set<RefId> direct_literals_;
  internal::FrameObjects<Destination> destinations_;
  internal::FrameObjects<CallHandle> calls_;
  using StepState = RuntimeLayouts::Phase;

  RuntimeLayouts::StepState& Step(StepId step);

  const RuntimeLayouts::StepState& Step(StepId step) const {
    return const_cast<Scope*>(this)->Step(step);
  }

  std::optional<Value>& Capture(size_t id) {
    return frame_->Get(layout_->captures)[layout_->capture_slots.at(id)];
  }

  const Value* absl_nullable Captured(size_t id) const;

  const RuntimeLayouts::Flow& runner_layout() const;
  const RuntimeLayouts::Body* absl_nullable layout_ = nullptr;
  std::unique_ptr<internal::FrameBlock> frame_;
  /// One cursor per ref, shared by every value read of it: see [ValueCursor].
  /// On the scope that *owns* the ref, so passes of a loop take turns on the
  /// one view of an outer stream rather than each starting it again.
  internal::FrameObjects<ValueCursor> cursors_;
  /// Refs whose cursor is being made right now, so only one step subscribes.
  absl::flat_hash_set<RefId> opening_cursors_;
  absl::flat_hash_map<RefId, absl::StatusOr<NodePtr>> nodes_;
  absl::flat_hash_set<RefId> opening_;
  std::vector<RefId> unwritten_;

  struct OwnedNode {
    std::shared_ptr<nodes::NodeMap> map;
    NodePtr node;
    std::string id;
  };

  std::vector<OwnedNode> owned_nodes_;
  Scope* absl_nullable resource_owner_ = nullptr;
  Group* absl_nullable work_ = nullptr;
  std::shared_ptr<a11::Promise<Value>> expression_result_;
  Value expression_it_;
  absl::flat_hash_map<ExprId, std::shared_ptr<a11::Promise<a11::Unit>>>
      expression_cleanups_;
};

/// One execution of one flow, against the action it is running as.
class Runner {
 public:
  Runner(std::shared_ptr<const CompiledProgram> program,
         const ResolvedFlow& flow, std::shared_ptr<actions::Action> action,
         std::shared_ptr<HostBridge> bridge,
         std::shared_ptr<net::WireStream> dispatch_stream,
         std::shared_ptr<ExecutionSnapshot> snapshot, FrameOptions frames,
         std::shared_ptr<internal::FrameBudget> budget = nullptr)
      : program_(std::move(program)),
        flow_(&flow),
        action_(std::move(action)),
        bridge_(std::move(bridge)),
        dispatch_stream_(std::move(dispatch_stream)),
        snapshot_(snapshot == nullptr ? std::make_shared<ExecutionSnapshot>()
                                      : std::move(snapshot)),
        layout_(&RuntimeAccess::Layout(*program_, flow.graph.name)),
        frame_budget_(budget == nullptr
                          ? std::make_shared<internal::FrameBudget>(frames)
                          : std::move(budget)),
        frame_pool_(frame_budget_) {}

  absl::Status Run();

  Monitor& monitor() { return *monitor_; }

  std::shared_ptr<Monitor> shared_monitor() { return monitor_; }

  HostBridge& bridge() { return *bridge_; }

  const Program& shapes() const { return program_->program(); }

  const PreparedExpressions& prepared() const { return program_->prepared(); }

  const FlowGraph& graph() const { return flow_->graph; }

  const FlowPlan& plan() const { return flow_->plan; }

  const SingleItemAnalysis* absl_nullable SingleItems() const {
    return program_->SingleItems(graph().name);
  }

  std::shared_ptr<TransportStats> transport_stats;

  const std::shared_ptr<actions::Action>& action() const { return action_; }

  const std::shared_ptr<net::WireStream>& dispatch_stream() const {
    return dispatch_stream_;
  }

  /// The temporary node map a `nodes` block names.
  ///
  /// One map per name per execution. Nodes created in it are not in the
  /// session's node map, so a peer neither sees them nor receives their
  /// fragments.
  absl::StatusOr<std::shared_ptr<nodes::NodeMap>> NodeMapNamed(
      const std::string& name);

  const RuntimeLayouts::Flow& layout() const { return *layout_; }

  absl::StatusOr<std::unique_ptr<internal::FrameBlock>> AcquireFrame(
      BodyId body);

  void ReturnFrame(std::unique_ptr<internal::FrameBlock> frame);

  /// An id for a node the flow declared, unique within this run.
  ///
  /// Named after the flow's own action and the name the flow gave it, so a node
  /// a peer does see is recognisable rather than a bare identifier.
  absl::StatusOr<std::string> FreshNodeId(const std::string& name);

  /// The schema and handler for a call target.
  ///
  /// The handler may be empty: an action registered for its schema alone is one
  /// whose ports are known here and whose *work* happens on the peer, and that
  /// is what a flow composing a gateway's actions from the outside registers.
  /// Such an action can only be `call`ed; what `run` needs is a handler, and
  /// saying `run` without one is an error rather than a quiet trip to the
  /// session.
  absl::Status Resolve(const std::string& name,
                       actions::ActionSchema* absl_nonnull schema,
                       actions::ActionHandler* absl_nonnull handler);
  absl::Status Preflight();

  /// The schema of an action this flow calls, resolved once per run.
  ///
  /// By pointer, because a schema is a map of ports and this is asked for once
  /// per call step *per pass of a loop*: returning it by value copied every
  /// port name of every action a loop body calls, once per value the loop read.
  /// The table holds the schemas by pointer so an answer stays valid while it
  /// grows.
  absl::StatusOr<const actions::ActionSchema*> SchemaOf(
      const std::string& action);

  /// Whether this body's call ports have already been checked against the
  /// schemas of what they call.
  ///
  /// The check is a property of the body and the registry, neither of which
  /// changes while a flow runs, so a loop body is checked on its first pass and
  /// not on its thousandth.
  bool PortsChecked(BodyId body);

  /// The headers `forward headers` sends on to a step, as they arrived.
  ///
  /// A pattern matches the flow's own headers -- what its caller sent -- and
  /// `*` in one matches any run of characters. Nothing is invented: a header
  /// that was not sent is simply not forwarded, because a composition should
  /// not fail over an optional one nobody supplied.
  data::ByteMap Forwarded(const graph::Step& step) const;

  absl::Status RunCall(Scope& scope, StepId step);

 private:
  absl::Status StartCall(Scope& scope, StepId step, CallHandle& handle);
  absl::Status PumpCall(StepId step, CallHandle& handle);
  absl::Status AwaitCall(const graph::Step& step, CallHandle& handle);

  std::shared_ptr<const CompiledProgram> program_;
  const ResolvedFlow* absl_nonnull flow_;
  std::shared_ptr<actions::Action> action_;
  std::shared_ptr<HostBridge> bridge_;
  std::shared_ptr<net::WireStream> dispatch_stream_;
  std::shared_ptr<ExecutionSnapshot> snapshot_;
  std::shared_ptr<Monitor> monitor_ = std::make_shared<Monitor>();
  absl::flat_hash_map<std::string, std::shared_ptr<nodes::NodeMap>> node_maps_;
  absl::flat_hash_map<std::string, uint64_t> node_counts_;
  const RuntimeLayouts::Flow* absl_nonnull layout_;
  std::shared_ptr<internal::FrameBudget> frame_budget_;
  internal::FramePool frame_pool_;
  absl::flat_hash_map<std::string, std::unique_ptr<actions::ActionSchema>>
      schemas_;
  /// Bodies whose call ports have been checked; see [PortsChecked].
  absl::flat_hash_set<BodyId> checked_bodies_;
  absl::flat_hash_map<std::string,
                      std::pair<actions::ActionSchema, actions::ActionHandler>>
      bindings_;
};

inline RuntimeLayouts::StepState& Scope::Step(StepId step) {
  CHECK_EQ(graph().steps[step].body, body_);
  return frame_->Get(layout_->steps)[runner_layout().step_slots[step]];
}

inline const Value* absl_nullable Scope::Captured(size_t id) const {
  if (layout_ == nullptr || frame_ == nullptr) {
    return nullptr;
  }
  const auto found = layout_->capture_slots.find(id);
  if (found == layout_->capture_slots.end()) {
    return nullptr;
  }
  const auto& value = frame_->Get(layout_->captures)[found->second];
  return value.has_value() ? &*value : nullptr;
}

inline const RuntimeLayouts::Flow& Scope::runner_layout() const {
  return runner_->layout();
}

inline const FlowGraph& Scope::graph() const {
  return runner_->graph();
}

inline Monitor& Scope::monitor() const {
  return runner_->monitor();
}

inline HostBridge& Scope::bridge() const {
  return runner_->bridge();
}

inline const Program& Scope::shapes() const {
  return runner_->shapes();
}

absl::StatusOr<actions::ActionHandler> MakeRuntimeHandler(
    std::shared_ptr<const CompiledProgram> program, const ResolvedFlow& flow,
    RunOptions options, std::shared_ptr<FrameBudget> budget = nullptr);

}  // namespace a11::flow::internal
#endif  // A11_FLOW_INTERNAL_EXECUTION_H_
