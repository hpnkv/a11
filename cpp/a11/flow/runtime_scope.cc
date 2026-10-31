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

#include "a11/flow/internal/execution.h"

namespace a11::flow::internal {

std::shared_ptr<a11::Promise<a11::Unit>> Scope::ExpressionCleanup(ExprId expr) {
  thread::MutexLock lock(&monitor().mu());
  const auto found = expression_cleanups_.find(expr);
  return found == expression_cleanups_.end() ? nullptr : found->second;
}

Scope::~Scope() {
  if (auto stats = runner_->transport_stats; stats != nullptr) {
    stats->active_writer_groups.fetch_sub(writer_groups_.size(),
                                          std::memory_order_relaxed);
  }

  for (auto& [step, handle] : calls_) {
    const auto action = handle->action_now();
    if (action == nullptr || action->IsDone()) {
      continue;
    }
    action->Cancel().IgnoreError();

    struct Completion {
      explicit Completion(std::shared_ptr<Monitor> shared)
          : monitor(std::move(shared)), changed(*monitor) {}

      std::shared_ptr<Monitor> monitor;
      Monitor::Condition changed;
      bool ready = false;
    };

    const auto state = std::make_shared<Completion>(runner_->shared_monitor());
    action->Wait().OnReady([state](const auto&) {
      thread::MutexLock lock(&state->monitor->mu());
      state->ready = true;
      state->changed.Wake();
    });
    thread::MutexLock lock(&state->monitor->mu());
    state->monitor->WaitComplete(state->changed, [&] { return state->ready; });
  }
  for (const auto& owned : owned_nodes_) {
    owned.map->Discard(owned.id, owned.node).status().IgnoreError();
  }
  for (auto& [step, handle] : calls_) {
    const auto action = handle->action_now();
    if (action == nullptr || graph().steps[step].action_id != kNone) {
      continue;
    }
    const auto map = action->GetNodeMap();
    if (map == nullptr) {
      continue;
    }
    const auto discard = [&](std::string_view name) {
      const auto id = actions::Action::MakeNodeId(action->GetId(), name);
      if (!id.ok()) {
        return;
      }
      const auto node = map->GetIfExists(*id);
      if (node.ok() && *node != nullptr) {
        map->Discard(*id, *node).status().IgnoreError();
      }
    };
    const auto schema = action->GetSchema();
    for (const auto* ports : {&schema.inputs, &schema.outputs}) {
      for (const auto& [name, port] : *ports) {
        discard(name);
      }
    }
    for (const auto name :
         {actions::kActionStatusOutput, actions::kActionDispatchStatusOutput,
          actions::kActionLogOutput}) {
      discard(name);
    }
  }
  expression_cleanups_.clear();
  expression_result_.reset();
  expression_it_ = {};
  owned_nodes_.clear();
  nodes_.clear();
  cursors_.clear();
  calls_.clear();
  destinations_.clear();
  writer_groups_.clear();
  buffers_.clear();
  lazies_.clear();
  single_slots_.clear();
  buses_.clear();
  presets_.clear();
  if (frame_ != nullptr) {
    runner_->ReturnFrame(std::move(frame_));
  }
}

// --- Wiring ------------------------------------------------------------------

absl::Status Scope::Prepare() {
  const FlowGraph& flow = graph();
  layout_ = runner_layout().bodies[body_].get();
  analysis_ = &layout_->analysis;
  ABSL_ASSIGN_OR_RETURN(frame_, runner_->AcquireFrame(body_));
  frame_->Initialize();
  for (const auto& [ref, count] : analysis_->value_consumers) {
    frame_->Get(layout_->value_consumers)[layout_->cursors.index.at(ref)] =
        count;
  }
  calls_.Bind(*frame_, layout_->calls);
  lazies_.Bind(*frame_, layout_->lazies);
  buses_.Bind(*frame_, layout_->buses);
  buffers_.Bind(*frame_, layout_->buffers);
  destinations_.Bind(*frame_, layout_->destinations);
  writer_groups_.Bind(*frame_, layout_->writers);
  cursors_.Bind(*frame_, layout_->cursors);

  const bool check_ports = !runner_->PortsChecked(body_);
  for (const StepId step : flow.bodies[body_].steps) {
    if (flow.steps[step].kind != StepKind::kCall) {
      continue;
    }
    if (!check_ports) {
      calls_.Construct(step, monitor());
      continue;
    }
    ABSL_ASSIGN_OR_RETURN(const actions::ActionSchema* absl_nonnull schema,
                          runner_->SchemaOf(flow.steps[step].action));
    // A port the target does not declare, rejected before anything runs.
    for (const auto& [key, ref] : flow.steps[step].ports) {
      const bool input = absl::StartsWith(key, "inputs:");
      const std::string name = flow.refs[ref].name;
      const auto& declared = input ? schema->inputs : schema->outputs;
      if (declared.contains(name)) {
        continue;
      }
      std::vector<std::string> known;
      known.reserve(declared.size());
      for (const auto& [port, unused] : declared) {
        known.push_back(port);
      }
      std::sort(known.begin(), known.end());
      return Fail(
          absl::StrCat(flow.steps[step].action, " has no ",
                       input ? "input" : "output", " port '", name,
                       "' (declared: ",
                       known.empty() ? "none" : absl::StrJoin(known, ", "),
                       "), but ", flow.refs[ref].label, " names one."),
          absl::StatusCode::kNotFound);
    }
    calls_.Construct(step, monitor());
  }

  for (const RefId ref : analysis_->refs) {
    const auto readers = analysis_->readers.find(ref);
    const int count = readers == analysis_->readers.end() ? 0 : readers->second;
    if (count <= 0 || presets_.contains(ref)) {
      continue;
    }
    // A fold's accumulator is read by name and produced by the fold itself, one
    // value at a time. Giving it a bus would be a producer nothing ever asks
    // for -- and a pump waiting to be wanted is a flow that never ends.
    if (flow.refs[ref].kind == RefKind::kBound &&
        flow.refs[ref].role == "fold") {
      continue;
    }
    if (flow.refs[ref].kind == RefKind::kBound) {
      lazies_.Construct(ref, monitor(), [this, ref] { return BoundItem(ref); });
      continue;
    }
    if (flow.refs[ref].kind == RefKind::kStatus) {
      lazies_.Construct(ref, monitor(),
                        [this, ref] { return StatusItem(ref); });
      continue;
    }
    // Which subject of a race won: produced by the barrier, so reading it is
    // waiting for the barrier -- the same shape as a status.
    if (flow.refs[ref].kind == RefKind::kWinner) {
      lazies_.Construct(ref, monitor(),
                        [this, ref] { return WinnerItem(ref); });
      continue;
    }
    const auto* single_items = runner_->SingleItems();
    if (single_items != nullptr && single_items->refs[ref].eligible) {
      const auto& fact = single_items->refs[ref];
      if (fact.direct_transfer && count == 1 &&
          analysis_->materialise.find(ref) == analysis_->materialise.end()) {
        direct_literals_.insert(ref);
        continue;
      }

      const bool replay =
          analysis_->materialise.find(ref) != analysis_->materialise.end();
      single_slots_.emplace(
          ref, std::make_shared<SingleSlot>(
                   monitor(),
                   [this, ref](const Sink& sink) { return Produce(ref, sink); },
                   static_cast<size_t>(count), replay, fact.pump_free,
                   runner_->transport_stats));
      if (runner_->transport_stats != nullptr) {
        runner_->transport_stats->slots.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }
    if (runner_->transport_stats != nullptr) {
      runner_->transport_stats->queue_readers.fetch_add(
          static_cast<size_t>(count), std::memory_order_relaxed);
    }
    auto* bus = &buses_.Construct(
        ref, monitor(), flow.refs[ref].label,
        [this, ref](const Sink& sink) { return Produce(ref, sink); }, count);
    if (analysis_->materialise.contains(ref)) {
      // Read from inside a loop or a branch: buffered once here, replayed per
      // pass, so the buffer is the one reader of the underlying stream.
      ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, bus->Take());
      buffers_.Construct(ref, monitor(), std::move(reader));
    }
  }

  // A node of the flow's own that nothing writes still has to end, or a reader
  // of it would wait for a value that was never coming.
  for (const RefId ref : analysis_->nodes) {
    if (flow.refs[ref].id_expr != kNone) {
      continue;
    }
    const auto readers = analysis_->readers.find(ref);
    if (readers == analysis_->readers.end() || readers->second <= 0) {
      continue;
    }
    if (std::find(analysis_->destinations.begin(),
                  analysis_->destinations.end(),
                  ref) != analysis_->destinations.end()) {
      continue;
    }
    unwritten_.push_back(ref);
  }

  for (const RefId ref : analysis_->destinations) {
    const auto writers = analysis_->writers.find(ref);
    const int count = writers == analysis_->writers.end() ? 0 : writers->second;
    const graph::Ref& one = flow.refs[ref];
    if (one.kind == RefKind::kSink) {
      writer_groups_.Construct(ref,
                               WriterGroup{count, absl::OkStatus(), nullptr});
      if (auto stats = runner_->transport_stats; stats != nullptr) {
        stats->virtual_groups.fetch_add(1, std::memory_order_relaxed);
        const auto active = stats->active_writer_groups.fetch_add(
                                1, std::memory_order_relaxed) +
                            1;
        auto peak = stats->peak_writer_groups.load(std::memory_order_relaxed);
        while (active > peak &&
               !stats->peak_writer_groups.compare_exchange_weak(
                   peak, active, std::memory_order_relaxed)) {}
      }
      continue;
    }
    const bool tolerant = one.kind == RefKind::kCallPort && one.call != kNone &&
                          flow.steps[one.call].tolerant;
    destinations_.Construct(
        ref, monitor(), one.label, [this, ref] { return DestinationNode(ref); },
        count, tolerant);
  }
  return absl::OkStatus();
}

// --- Lookups -----------------------------------------------------------------

Scope* absl_nullable Scope::FindOwner(RefId ref) {
  const BodyId owner = graph().refs[ref].owner;
  for (Scope* at = this; at != nullptr; at = at->parent_) {
    if (at->body_ == owner) {
      return at;
    }
  }
  return nullptr;
}

Scope* absl_nonnull Scope::Owner(RefId ref) {
  Scope* found = FindOwner(ref);
  return found == nullptr ? this : found;
}

absl::StatusOr<CallHandle*> Scope::Call(StepId step) {
  for (Scope* at = this; at != nullptr; at = at->parent_) {
    const auto found = at->calls_.find(step);
    if (found != at->calls_.end()) {
      ABSL_RETURN_IF_ERROR(at->EnsureStep(step));
      return &*found->second;
    }
  }
  return Fail(absl::StrCat("Internal flow error: ", graph().steps[step].label,
                           " was never started."));
}

Destination* absl_nullable Scope::FindDestination(RefId ref) {
  for (Scope* at = this; at != nullptr; at = at->parent_) {
    const auto found = at->destinations_.find(ref);
    if (found != at->destinations_.end()) {
      return &*found->second;
    }
  }
  return nullptr;
}

absl::StatusOr<Destination*> Scope::DestinationOf(RefId ref) {
  if (graph().refs[ref].kind == RefKind::kSink) {
    const auto& targets = graph().refs[ref].sources;
    if (targets.size() != 1) {
      return Fail("This operation requires one physical destination.");
    }
    ref = targets.front();
  }
  Destination* found = FindDestination(ref);
  if (found != nullptr) {
    return found;
  }
  return Fail(absl::StrCat("Internal flow error: nothing writes ",
                           graph().refs[ref].label, "."));
}

absl::StatusOr<ReaderPtr> Scope::Subscribe(RefId ref) {
  if (ref == kNone) {
    return Fail("Internal flow error: nothing to read.");
  }
  Scope* owner = FindOwner(ref);
  if (owner == nullptr) {
    return Fail(absl::StrCat("Internal flow error: ", graph().refs[ref].label,
                             " is not in scope here."));
  }
  if (graph().refs[ref].phase_gate != kNone) {
    ABSL_RETURN_IF_ERROR(StepDone(graph().refs[ref].phase_gate));
  }
  if (const auto found = owner->presets_.find(ref);
      found != owner->presets_.end()) {
    return ReaderPtr(new ListReader(found->second));
  }
  if (const auto found = owner->lazies_.find(ref);
      found != owner->lazies_.end()) {
    return found->second->Replay();
  }
  if (const auto found = owner->buffers_.find(ref);
      found != owner->buffers_.end()) {
    return found->second->Replay();
  }
  if (const auto found = owner->single_slots_.find(ref);
      found != owner->single_slots_.end()) {
    return found->second->Take();
  }
  const auto found = owner->buses_.find(ref);
  if (found == owner->buses_.end()) {
    return Fail(absl::StrCat("Internal flow error: ", graph().refs[ref].label,
                             " has no reader slot left."));
  }
  return found->second->Take();
}

// --- Nodes -------------------------------------------------------------------

absl::StatusOr<NodePtr> Scope::LocalNode(RefId ref) {
  if (graph().refs[ref].phase_gate != kNone) {
    ABSL_RETURN_IF_ERROR(StepDone(graph().refs[ref].phase_gate));
  }
  Scope* owner = Owner(ref);
  Monitor& monitor = this->monitor();
  {
    thread::MutexLock lock(&monitor.mu());
    if (owner->opening_.contains(ref)) {
      ABSL_RETURN_IF_ERROR(
          monitor.Wait([owner, ref] { return owner->nodes_.contains(ref); }));
      return owner->nodes_.at(ref);
    }
    owner->opening_.insert(ref);
  }
  absl::StatusOr<NodePtr> node = owner->MakeLocalNode(ref);
  {
    thread::MutexLock lock(&monitor.mu());
    owner->nodes_.insert_or_assign(ref, node);
  }
  monitor.Wake();
  return node;
}

absl::StatusOr<NodePtr> Scope::MakeLocalNode(RefId ref) {
  if (work_ != nullptr) {
    ABSL_RETURN_IF_ERROR(work_->CheckActive());
  }
  const graph::Ref& one = graph().refs[ref];
  std::shared_ptr<nodes::NodeMap> map;
  if (one.node_map.empty()) {
    map = runner_->action()->GetNodeMap();
  } else {
    ABSL_ASSIGN_OR_RETURN(map, runner_->NodeMapNamed(one.node_map));
  }
  if (map == nullptr) {
    return Fail(absl::StrCat(one.label, " has no node map to live in."));
  }
  std::string node_id;
  if (one.id_expr != kNone) {
    // A declaration with an id attaches to the node that names -- the one a
    // caller passed in a header, say -- and writes there instead.
    ABSL_ASSIGN_OR_RETURN(const Value named, Evaluate(one.id_expr));
    const Value* field = named.Get("id");
    node_id = AsText(field == nullptr ? named : *field);
    if (node_id.empty()) {
      return Fail(
          absl::StrCat(one.label, " was given no node id to attach to."));
    }
  } else {
    ABSL_ASSIGN_OR_RETURN(node_id, runner_->FreshNodeId(one.name));
  }
  ABSL_ASSIGN_OR_RETURN(auto node, map->Get(node_id));
  if (one.id_expr == kNone) {
    thread::MutexLock lock(&monitor().mu());
    Scope* owner = resource_owner_ == nullptr ? this : resource_owner_;
    owner->owned_nodes_.push_back({map, node, std::move(node_id)});
  }
  return node;
}

absl::StatusOr<NodePtr> Scope::DestinationNode(RefId ref) {
  const graph::Ref& one = graph().refs[ref];
  switch (one.kind) {
    case RefKind::kCallPort: {
      ABSL_ASSIGN_OR_RETURN(CallHandle * handle, Call(one.call));
      return handle->Node(one.name, one.direction);
    }
    case RefKind::kFlowPort:
      return runner_->action()->GetOutput(one.name);
    case RefKind::kNode:
      return LocalNode(ref);
    default:
      return Fail(
          absl::StrCat(one.label, " is not something a flow can write."));
  }
}

absl::StatusOr<NodePtr> Scope::ReadableNode(RefId ref) {
  const graph::Ref& one = graph().refs[ref];
  switch (one.kind) {
    case RefKind::kNode:
      return LocalNode(ref);
    case RefKind::kCallPort: {
      ABSL_ASSIGN_OR_RETURN(CallHandle * handle, Call(one.call));
      return handle->Node(one.name, one.direction);
    }
    case RefKind::kFlowPort:
      if (one.direction == syntax::PortDirection::kInput) {
        return runner_->action()->GetInput(one.name);
      }
      return NodePtr{};
    default:
      return NodePtr{};
  }
}

absl::Status Scope::ReadNode(const NodePtr& node, const Sink& sink) {
  // Every value a flow reads enters here, so this is the one read worth
  // batching: `NextFragments` hands back what the store already had and waits
  // only when it had nothing, so a batch costs the same await as a value would.
  std::vector<ItemPtr> items;
  items.reserve(kQueueDepth);
  while (true) {
    absl::StatusOr<std::vector<std::optional<data::NodeFragment>>> batch =
        node->NextFragments(kQueueDepth).Await();
    if (!batch.ok()) {
      return batch.status();
    }
    bool ended = false;
    for (std::optional<data::NodeFragment>& fragment : *batch) {
      if (!fragment.has_value()) {
        ended = true;
        break;
      }
      ABSL_ASSIGN_OR_RETURN(data::Chunk * chunk, fragment->GetChunk());
      // A null chunk is a marker rather than a value: a final one ends the
      // node, and any other is skipped, which is how an empty stream stays
      // empty instead of turning into a value nobody wrote.
      if (chunk->IsNull()) {
        if (!fragment->continued) {
          ended = true;
          break;
        }
        continue;
      }
      // Moved out of the fragment, which nothing else reads: a value's payload
      // is the value, and copying it here is a copy per stage.
      items.push_back(Item::OfChunk(std::move(*chunk)));
    }
    ABSL_RETURN_IF_ERROR(sink.Many(items));
    if (ended) {
      return absl::OkStatus();
    }
  }
}

absl::Status Scope::EnsureStep(StepId step) {
  if (work_ != nullptr) {
    ABSL_RETURN_IF_ERROR(work_->CheckActive());
  }
  if (work_ == nullptr) {
    return Fail("A step has no owning execution scope.");
  }
  {
    thread::MutexLock lock(&monitor().mu());
    if (Step(step).started) {
      return absl::OkStatus();
    }
    Step(step).started = true;
  }
  const auto task =
      work_->Spawn([this, step]() -> absl::Status { return RunStep(step); },
                   [this, step](absl::Status status) {
                     return CompleteStep(step, std::move(status));
                   });
  if (task.has_value() && graph().steps[step].kind == StepKind::kPipe) {
    WatchVirtual(step, &*task);
    const auto destinations = PhysicalDestinations(step);
    const auto& pipe = graph().steps[step];
    const bool borrowed_input =
        pipe.activation != kNone && pipe.transfer == nullptr &&
        pipe.expression_cleanups.empty() && pipe.source != kNone &&
        graph().refs[pipe.source].kind == RefKind::kFlowPort;
    for (auto* destination : destinations) {
      destination->Watch(this, step, *task,
                         borrowed_input && destinations.size() == 1);
    }
    bool done;
    {
      thread::MutexLock lock(&monitor().mu());
      done = Step(step).phase == StepState::kDone;
    }
    if (done) {
      WatchVirtual(step, nullptr);
      for (auto* destination : destinations) {
        destination->Unwatch(this, step);
      }
    }
  }
  if (graph().steps[step].kind == StepKind::kCall) {
    for (const StepId feed : graph().bodies[body_].steps) {
      if (graph().steps[feed].activation == step) {
        ABSL_RETURN_IF_ERROR(EnsureStep(feed));
      }
    }
  }
  return absl::OkStatus();
}

absl::Status Scope::Run() {
  ABSL_RETURN_IF_ERROR(Prepare());
  Group group(monitor(), parent_ == nullptr ? nullptr : parent_->work_);
  work_ = &group;
  // A node of the flow's own that nothing writes is ended here, and without a
  // fiber each: `Finalize` hands the write and the close to the node's own
  // writer pump, so the awaitables are collected and waited on together.
  std::vector<a11::Task> closing;
  closing.reserve(unwritten_.size());
  absl::flat_hash_map<StepId, std::vector<RefId>> gated_closing;
  for (const RefId ref : unwritten_) {
    if (graph().refs[ref].phase_gate != kNone) {
      gated_closing[graph().refs[ref].phase_gate].push_back(ref);
      continue;
    }
    ABSL_ASSIGN_OR_RETURN(const NodePtr node, LocalNode(ref));
    closing.push_back(node->Finalize({.wait = true}));
  }
  for (const absl::StatusOr<a11::Unit>& closed : a11::AwaitAll(closing)) {
    ABSL_RETURN_IF_ERROR(closed.status());
  }
  for (auto& [gate, refs] : gated_closing) {
    group.Spawn([this, closing_refs = std::move(refs)]() -> absl::Status {
      for (const auto ref : closing_refs) {
        ABSL_ASSIGN_OR_RETURN(const NodePtr node, LocalNode(ref));
        ABSL_RETURN_IF_ERROR(node->Finalize({.wait = true}).Await().status());
      }
      return absl::OkStatus();
    });
  }
  for (const auto& [ref, slot] : single_slots_) {
    if (slot->replay()) {
      slot->Wanted();
    }
    if (!slot->inline_producer()) {
      group.Spawn([this, slot]() -> absl::Status {
        if (runner_->transport_stats != nullptr) {
          runner_->transport_stats->slot_pumps.fetch_add(
              1, std::memory_order_relaxed);
        }
        return slot->Pump();
      });
    }
  }
  for (auto& [ref, bus] : buses_) {
    Bus* one = &*bus;
    const auto task = group.Spawn(
        [this, one]() -> absl::Status {
          if (runner_->transport_stats != nullptr) {
            runner_->transport_stats->bus_pumps.fetch_add(
                1, std::memory_order_relaxed);
          }
          return one->Pump();
        },
        [one](absl::Status status) {
          return one->CompletePump(std::move(status));
        });
    if (task.has_value() && graph().refs[ref].kind == RefKind::kFlowPort &&
        graph().refs[ref].direction == syntax::PortDirection::kInput) {
      one->TrackInputPump(*task);
    }
  }
  for (auto& [ref, buffer] : buffers_) {
    Buffer* one = &*buffer;
    group.Spawn([this, one]() -> absl::Status {
      if (runner_->transport_stats != nullptr) {
        runner_->transport_stats->buffer_pumps.fetch_add(
            1, std::memory_order_relaxed);
      }
      return one->Fill();
    });
  }
  for (const StepId step : graph().bodies[body_].steps) {
    if (!graph().steps[step].deferred) {
      ABSL_RETURN_IF_ERROR(EnsureStep(step));
    }
  }
  const absl::Status result = group.Join();
  if (!result.ok()) {
    const auto owner_step = graph().bodies[body_].owner_step;
    if (parent_ != nullptr && owner_step != kNone &&
        graph().steps[owner_step].kind != StepKind::kCall) {
      parent_->AbortDestinations(owner_step, result).IgnoreError();
    }
    for (const auto& owned : owned_nodes_) {
      owned.node->AbortWithStatus(result).Await().IgnoreError();
    }
    for (auto& [step, handle] : calls_) {
      const auto action = handle->action_now();
      if (action != nullptr) {
        action->Cancel().IgnoreError();
      }
    }
  }
  work_ = nullptr;
  return result;
}

void Scope::MarkDone(StepId step, const absl::Status& status) {
  {
    thread::MutexLock lock(&monitor().mu());
    Step(step).phase = StepState::kDone;
    Step(step).completion = status;
    Step(step).order = monitor().CompletionOrder();
  }
  monitor().Wake();
}

absl::InlinedVector<Destination*, 1> Scope::PhysicalDestinations(
    StepId step, bool include_reserved) {
  absl::InlinedVector<Destination*, 1> result;
  const auto add = [&](RefId ref) {
    if (ref != kNone) {
      if (auto* destination = FindDestination(ref); destination != nullptr) {
        if (std::find(result.begin(), result.end(), destination) ==
            result.end()) {
          result.push_back(destination);
        }
      }
    }
  };
  for (const auto target : graph::TransferTargets(graph().steps[step])) {
    if (target != kNone && graph().refs[target].kind == RefKind::kSink) {
      for (const auto physical : graph().refs[target].sources) {
        add(physical);
      }
    } else {
      add(target);
    }
  }
  const auto held = analysis_->held.find(step);
  if (include_reserved && held != analysis_->held.end()) {
    for (const auto ref : held->second) {
      if (graph().refs[ref].kind != RefKind::kSink) {
        add(ref);
      }
    }
  }
  return result;
}

void Scope::WatchVirtual(StepId step, const a11::Task* absl_nullable task) {
  bool cancelled = false;
  for (const auto ref : graph().Destinations(step)) {
    const auto& sink = graph().refs[ref];
    if (sink.kind != RefKind::kSink ||
        std::any_of(sink.sources.begin(), sink.sources.end(),
                    [](auto id) { return id != kNone; })) {
      continue;
    }
    auto* owner = Owner(ref);
    thread::MutexLock lock(&monitor().mu());
    auto& group = owner->writer_groups_.at(ref);
    if (task == nullptr) {
      if (group.feeds != nullptr) {
        group.feeds->erase({this, step});
      }
    } else if (!group.failure.ok()) {
      cancelled = true;
    } else {
      if (group.feeds == nullptr) {
        group.feeds = std::make_unique<
            absl::flat_hash_map<std::pair<const void*, StepId>, a11::Task>>();
      }
      group.feeds->insert_or_assign({this, step}, *task);
    }
  }
  if (cancelled && task != nullptr) {
    task->Cancel().IgnoreError();
    monitor().WakeAll();
  }
}

absl::Status Scope::AbortDestinations(StepId step, absl::Status reason) {
  for (auto* destination : PhysicalDestinations(step, true)) {
    destination->Abort(reason).IgnoreError();
    reason = destination->failure();
  }
  auto refs = graph().Destinations(step);
  const auto held = analysis_->held.find(step);
  if (held != analysis_->held.end()) {
    refs.insert(refs.end(), held->second.begin(), held->second.end());
  }
  absl::InlinedVector<a11::Task, 1> tasks;
  for (auto ref : refs) {
    if (graph().refs[ref].kind != RefKind::kSink ||
        std::any_of(graph().refs[ref].sources.begin(),
                    graph().refs[ref].sources.end(),
                    [](auto id) { return id != kNone; })) {
      continue;
    }
    while (graph().refs[ref].subject != kNone) {
      ref = graph().refs[ref].subject;
    }
    auto* owner = Owner(ref);
    thread::MutexLock lock(&monitor().mu());
    auto& group = owner->writer_groups_.at(ref);
    if (group.failure.ok()) {
      group.failure = reason;
    }
    reason = group.failure;
    if (group.feeds != nullptr) {
      for (const auto& [identity, task] : *group.feeds) {
        tasks.push_back(task);
      }
    }
  }
  for (const auto& task : tasks) {
    task.Cancel().IgnoreError();
  }
  monitor().WakeAll();
  return reason;
}

absl::Status Scope::ReleaseWriter(RefId ref, const absl::Status& status) {
  if (graph().refs[ref].kind != RefKind::kSink) {
    ABSL_ASSIGN_OR_RETURN(auto* destination, DestinationOf(ref));
    return destination->Release();
  }
  auto* owner = Owner(ref);
  {
    thread::MutexLock lock(&monitor().mu());
    auto& group = owner->writer_groups_.at(ref);
    if (!status.ok() && group.failure.ok()) {
      group.failure = status;
    }
    if (group.remaining > 0) {
      --group.remaining;
    }
  }
  monitor().Wake();
  return absl::OkStatus();
}

absl::Status Scope::StepDone(StepId step) {
  ABSL_RETURN_IF_ERROR(OperationDone(step));
  const auto ref = graph().steps[step].sink;
  if (ref != kNone) {
    auto* owner = Owner(ref);
    thread::MutexLock lock(&monitor().mu());
    ABSL_RETURN_IF_ERROR(monitor().Wait(
        [&] { return owner->writer_groups_.at(ref).remaining == 0; }));
    ABSL_RETURN_IF_ERROR(owner->writer_groups_.at(ref).failure);
  }
  const auto& transfer = graph().steps[step].transfer;
  if (transfer != nullptr) {
    for (const auto called : transfer->destination_calls) {
      ABSL_RETURN_IF_ERROR(StepDone(called));
    }
  }
  return absl::OkStatus();
}

absl::Status Scope::OperationDone(StepId step) {
  for (Scope* at = this; at != nullptr; at = at->parent_) {
    if (graph().steps[step].body != at->body_) {
      continue;
    }
    ABSL_RETURN_IF_ERROR(at->EnsureStep(step));
    thread::MutexLock lock(&monitor().mu());
    ABSL_RETURN_IF_ERROR(monitor().Wait(
        [at, step] { return at->Step(step).phase == StepState::kDone; }));
    return at->Step(step).completion;
  }
  return Fail(absl::StrCat("Internal flow error: ", graph().steps[step].label,
                           " is not in scope."));
}

absl::Status Scope::RunStep(StepId step) {
  absl::Status status;
  if (graph().steps[step].phase_gate != kNone) {
    status = StepDone(graph().steps[step].phase_gate);
  }
  for (const StepId dependency : graph().steps[step].after) {
    if (status.ok()) {
      status = StepDone(dependency);
    }
    if (!status.ok()) {
      break;
    }
  }
  if (status.ok()) {
    {
      thread::MutexLock lock(&monitor().mu());
      Step(step).phase = StepState::kExecuting;
    }
    status = Execute(step);
  }
  return status;
}

absl::Status Scope::CompleteStep(StepId step, absl::Status status) {
  if (absl::IsCancelled(status) &&
      graph().steps[step].kind == StepKind::kPipe) {
    const auto destinations = PhysicalDestinations(step);
    if (!destinations.empty() &&
        std::all_of(destinations.begin(), destinations.end(),
                    [](auto* dest) { return dest->AcceptStoppedFeed(); }) &&
        work_->CheckActive(false).ok()) {
      bool pending;
      {
        thread::MutexLock lock(&monitor().mu());
        pending = Step(step).phase == StepState::kPending;
      }
      if (pending) {
        for (const auto source : graph::TransferSources(graph().steps[step])) {
          SkipRef(source);
        }
      }
      status = absl::OkStatus();
    }
  }
  if (absl::IsCancelled(status)) {
    for (auto* destination : PhysicalDestinations(step, true)) {
      const auto failure = destination->failure();
      if (!failure.ok()) {
        status = failure;
        break;
      }
    }
    if (absl::IsCancelled(status)) {
      for (const auto ref : graph().Destinations(step)) {
        if (graph().refs[ref].kind != RefKind::kSink) {
          continue;
        }
        auto* owner = Owner(ref);
        thread::MutexLock lock(&monitor().mu());
        const auto& failure = owner->writer_groups_.at(ref).failure;
        if (!failure.ok()) {
          status = failure;
          break;
        }
      }
    }
    if (absl::IsCancelled(status)) {
      const auto cause = work_->CheckActive();
      if (!cause.ok()) {
        status = cause;
      }
    }
  }
  if (!status.ok()) {
    status = AbortDestinations(step, status);
  }
  const auto held = analysis_->held.find(step);
  if (held != analysis_->held.end()) {
    for (const auto ref : held->second) {
      if (graph().refs[ref].kind == RefKind::kSink) {
        continue;
      }
      const auto released = ReleaseWriter(ref, status);
      if (status.ok() && !released.ok()) {
        status = released;
      }
    }
  }
  for (auto* destination : PhysicalDestinations(step, true)) {
    destination->Unwatch(this, step);
  }
  if (graph::RecordsOutcome(graph().steps[step].kind)) {
    thread::MutexLock lock(&monitor().mu());
    if (!Step(step).outcome.has_value()) {
      Step(step).outcome = status;
    }
  }
  WatchVirtual(step, nullptr);
  for (const RefId ref : graph().ValueSources(step)) {
    RetireValueRef(ref);
  }
  MarkDone(step, status);
  if (held != analysis_->held.end()) {
    for (const auto ref : held->second) {
      if (graph().refs[ref].kind == RefKind::kSink) {
        ReleaseWriter(ref, status).IgnoreError();
      }
    }
  }
  return status;
}

void Scope::CallFinished(StepId step, const absl::Status& status) {
  for (auto& [ref, destination] : destinations_) {
    const auto& port = graph().refs[ref];
    if (port.kind == RefKind::kCallPort && port.call == step) {
      destination->StopFeeding(status);
    }
  }
}

}  // namespace a11::flow::internal
