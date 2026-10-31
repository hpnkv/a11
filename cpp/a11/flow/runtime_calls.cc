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

void CallHandle::Adopt(std::shared_ptr<actions::Action> action) {
  thread::MutexLock lock(&monitor_->mu());
  action_ = std::move(action);
}

void CallHandle::Started(std::shared_ptr<actions::Action> action) {
  {
    thread::MutexLock lock(&monitor_->mu());
    action_ = std::move(action);
    started_ = true;
  }
  monitor_->Wake();
}

void CallHandle::NeverStarted(absl::Status why) {
  {
    thread::MutexLock lock(&monitor_->mu());
    if (!started_) {
      started_ = true;
      start_error_ = why;
    }
    if (error_.ok()) {
      error_ = std::move(why);
    }
    if (!done_) {
      completion_order_ = monitor_->CompletionOrder();
    }
    done_ = true;
  }
  monitor_->Wake();
}

void CallHandle::Done() {
  {
    thread::MutexLock lock(&monitor_->mu());
    if (!done_) {
      completion_order_ = monitor_->CompletionOrder();
    }
    done_ = true;
  }
  monitor_->Wake();
}

void CallHandle::SetError(absl::Status error) {
  thread::MutexLock lock(&monitor_->mu());
  if (error_.ok()) {
    error_ = std::move(error);
  }
}

absl::StatusOr<std::shared_ptr<actions::Action>> CallHandle::Action() {
  thread::MutexLock lock(&monitor_->mu());
  ABSL_RETURN_IF_ERROR(monitor_->Wait([this] { return started_; }));
  ABSL_RETURN_IF_ERROR(start_error_);
  return action_;
}

absl::StatusOr<NodePtr> CallHandle::Node(const std::string& name,
                                         syntax::PortDirection direction) {
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<actions::Action> action,
                        Action());
  if (direction == syntax::PortDirection::kInput) {
    return action->GetInput(name, std::nullopt);
  }
  return action->GetOutput(name, std::nullopt);
}

absl::Status CallHandle::Outcome(absl::Status* absl_nonnull outcome) {
  std::shared_ptr<actions::Action> action;
  {
    thread::MutexLock lock(&monitor_->mu());
    ABSL_RETURN_IF_ERROR(monitor_->Wait([this] { return done_; }));
    if (!error_.ok()) {
      *outcome = error_;
      return absl::OkStatus();
    }
    action = action_;
  }
  *outcome = action == nullptr ? absl::OkStatus() : action->GetStatus();
  return absl::OkStatus();
}

absl::Status Runner::Run() {
  const auto shared = monitor_;
  ABSL_RETURN_IF_ERROR(action_->SetOnCancelled(
      [shared](const std::shared_ptr<actions::Action>&) {
        shared->Stop(absl::CancelledError("Flow action cancelled"));
        return absl::OkStatus();
      }));
  ABSL_RETURN_IF_ERROR(Preflight());
  if (!snapshot_->sealed) {
    snapshot_->sealed = true;
  }
  Scope root(*this, flow_->graph.root, nullptr, {});
  const absl::Status status = root.Run();
  if (!status.ok()) {
    monitor_->Stop(status);
  }
  return status;
}

absl::StatusOr<std::shared_ptr<nodes::NodeMap>> Runner::NodeMapNamed(
    const std::string& name) {
  thread::MutexLock lock(&monitor_->mu());
  const auto found = node_maps_.find(name);
  if (found != node_maps_.end()) {
    return found->second;
  }
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<nodes::NodeMap> made,
                        nodes::NodeMap::Create());
  node_maps_.emplace(name, made);
  return made;
}

absl::StatusOr<std::unique_ptr<internal::FrameBlock>> Runner::AcquireFrame(
    BodyId body) {
  thread::MutexLock lock(&monitor_->mu());
  return frame_pool_.Acquire(layout_->bodies[body]->storage);
}

void Runner::ReturnFrame(std::unique_ptr<internal::FrameBlock> frame) {
  frame->Reset();
  thread::MutexLock lock(&monitor_->mu());
  frame_pool_.Return(std::move(frame));
}

absl::StatusOr<std::string> Runner::FreshNodeId(const std::string& name) {
  uint64_t count = 0;
  {
    thread::MutexLock lock(&monitor_->mu());
    auto& next = node_counts_[name];
    if (next == std::numeric_limits<uint64_t>::max()) {
      return absl::ResourceExhaustedError(
          "Flow node identity space exhausted.");
    }
    count = ++next;
  }
  const std::string suffix = count == 1 ? name : absl::StrCat(name, "-", count);
  return actions::Action::MakeNodeId(action_->GetId(), suffix);
}

absl::StatusOr<const actions::ActionSchema*> Runner::SchemaOf(
    const std::string& action) {
  {
    thread::MutexLock lock(&monitor_->mu());
    const auto found = schemas_.find(action);
    if (found != schemas_.end()) {
      return &*found->second;
    }
  }
  auto schema = std::make_unique<actions::ActionSchema>();
  actions::ActionHandler handler;
  ABSL_RETURN_IF_ERROR(Resolve(action, schema.get(), &handler));
  thread::MutexLock lock(&monitor_->mu());
  const auto [at, added] = schemas_.emplace(action, std::move(schema));
  return at->second.get();
}

bool Runner::PortsChecked(BodyId body) {
  thread::MutexLock lock(&monitor_->mu());
  return !checked_bodies_.insert(body).second;
}

data::ByteMap Runner::Forwarded(const graph::Step& step) const {
  data::ByteMap chosen;
  if (step.forward.empty()) {
    return chosen;
  }
  const data::ByteMap available = action_->Headers();
  for (const std::string& pattern : step.forward) {
    for (const auto& [name, value] : available) {
      if (MatchesFolded(name, pattern)) {
        chosen[name] = value;
      }
    }
  }
  return chosen;
}

absl::StatusOr<actions::ActionHandler> MakeRuntimeHandler(
    std::shared_ptr<const CompiledProgram> program, const ResolvedFlow& flow,
    RunOptions options, std::shared_ptr<internal::FrameBudget> budget) {
  if (options.bridge == nullptr) {
    options.bridge = NativeHostBridge();
  }
  return actions::MakeAsyncActionHandler(
      [program = std::move(program), flow = &flow, options = std::move(options),
       budget = std::move(budget)](
          std::shared_ptr<actions::Action> action) -> absl::Status {
        Runner runner(program, *flow, std::move(action), options.bridge,
                      options.dispatch_stream, options.snapshot, options.frames,
                      budget);
        runner.transport_stats = options.transport_stats;
        return runner.Run();
      });
}

absl::Status Runner::Resolve(const std::string& name,
                             actions::ActionSchema* absl_nonnull schema,
                             actions::ActionHandler* absl_nonnull handler) {
  if (const auto found = bindings_.find(name); found != bindings_.end()) {
    *schema = found->second.first;
    *handler = found->second.second;
    return absl::OkStatus();
  }
  if (const ResolvedFlow* sibling = program_->Flow(name);
      sibling != nullptr && !sibling->plan.interface_only) {
    if (sibling->ambiguous) {
      return absl::FailedPreconditionError(
          absl::StrCat("Ambiguous implementation of '", name, "'."));
    }
    ABSL_ASSIGN_OR_RETURN(
        *schema, FlowSchema(sibling->implementation.value_or(sibling->plan),
                            &program_->program()));
    // A flow of this program that this flow runs is still the same client's, so
    // its own `call` steps belong on the same stream.
    ABSL_ASSIGN_OR_RETURN(
        *handler,
        MakeRuntimeHandler(program_, *sibling,
                           RunOptions{.transport_stats = transport_stats,
                                      .snapshot = snapshot_,
                                      .bridge = bridge_,
                                      .dispatch_stream = dispatch_stream_,
                                      .frames = frame_budget_->options()},
                           frame_budget_));
    bindings_.emplace(name, std::make_pair(*schema, *handler));
    return absl::OkStatus();
  }
  const auto* declaration = program_->Flow(name);
  const std::string wire_name = absl::StrReplaceAll(
      declaration != nullptr && !declaration->plan.binding_name.empty()
          ? declaration->plan.binding_name
          : name,
      {{"::", "#"}});
  if (const auto found = snapshot_->external.find(wire_name);
      found != snapshot_->external.end()) {
    *schema = found->second.first;
    *handler = found->second.second;
    return absl::OkStatus();
  }
  if (snapshot_->sealed) {
    return absl::FailedPreconditionError(
        "Binding is absent from the execution snapshot.");
  }
  const std::shared_ptr<actions::ActionRegistry> registry =
      action_->GetRegistry();
  if (registry == nullptr || !registry->IsRegistered(wire_name)) {
    std::vector<std::string> known = registry == nullptr
                                         ? std::vector<std::string>{}
                                         : registry->ListRegisteredActions();
    std::sort(known.begin(), known.end());
    return Fail(
        absl::StrCat(plan().name, " calls '", name,
                     "', which is not a flow in this program and is not "
                     "registered here (registered: ",
                     known.empty() ? "none" : absl::StrJoin(known, ", "), ")."),
        program_->Flow(name) != nullptr ? absl::StatusCode::kFailedPrecondition
                                        : absl::StatusCode::kNotFound);
  }
  ABSL_ASSIGN_OR_RETURN(*schema, registry->GetSchema(wire_name));
  // Registered without one: the registry reports that as an error rather than
  // as an empty handler, so asking is how it is found out.
  if (absl::StatusOr<actions::ActionHandler> found =
          registry->GetHandler(wire_name);
      found.ok()) {
    *handler = *std::move(found);
  }
  snapshot_->external.emplace(wire_name, std::make_pair(*schema, *handler));
  return absl::OkStatus();
}

absl::Status Runner::Preflight() {
  std::vector<const ResolvedFlow*> pending{flow_};
  absl::flat_hash_set<std::string> visited;
  while (!pending.empty()) {
    const ResolvedFlow* current = pending.back();
    pending.pop_back();
    if (!visited.insert(current->plan.name).second) {
      continue;
    }
    if (current->ambiguous || current->plan.interface_only) {
      return absl::FailedPreconditionError(absl::StrCat(
          "No unambiguous implementation for '", current->plan.name, "'."));
    }
    for (const graph::Step& step : current->graph.steps) {
      if (step.kind != StepKind::kCall) {
        continue;
      }
      actions::ActionSchema schema;
      actions::ActionHandler handler;
      ABSL_RETURN_IF_ERROR(Resolve(step.action, &schema, &handler));
      if (step.mode == "call" && dispatch_stream_ == nullptr &&
          action_->GetStream() == nullptr && action_->GetSession() == nullptr) {
        return absl::FailedPreconditionError(
            absl::StrCat(current->plan.name, " could not call '", step.action,
                         "': calling requires an attached WireStream or "
                         "Session before execution."));
      }
      if (step.mode == "run" && !handler) {
        return absl::FailedPreconditionError(absl::StrCat(
            current->plan.name, " says 'run ", step.action, "', but ",
            step.action,
            " is registered here for its schema alone and has no handler to "
            "run. Say 'call' to dispatch it on the flow's stream."));
      }
      FlowPlan actual;
      Program evidence = program_->program();
      actual.name = step.action;
      for (const auto direction :
           {syntax::PortDirection::kInput, syntax::PortDirection::kOutput}) {
        const auto& ports = direction == syntax::PortDirection::kInput
                                ? schema.inputs
                                : schema.outputs;
        for (const auto& [name, p] : ports) {
          PortPlan port;
          port.name = name;
          port.type = p.type;
          port.declared = p.type;
          port.direction = direction;
          port.unary = p.unary;
          port.required = p.required;
          if (!p.json_schema.empty()) {
            const auto metadata =
                nlohmann::json::parse(p.json_schema, nullptr, false);
            if (metadata.is_object() && metadata.contains("type") &&
                metadata["type"].is_string() && metadata["type"] == "object") {
              auto imported = JsonSchemaToDtos(
                  metadata, absl::StrCat("schema_", evidence.dtos.size()));
              if (imported.diagnostics.empty() && !imported.dtos.empty()) {
                port.declared = imported.dtos.front().name;
                for (auto& shape : imported.dtos) {
                  evidence.dtos.push_back(std::move(shape));
                }
              }
            }
          }
          actual.ports.push_back(std::move(port));
          if (direction != syntax::PortDirection::kInput) {
            continue;
          }
          bool supplied = false;
          const StepId call_id =
              static_cast<StepId>(&step - current->graph.steps.data());
          for (const auto& feed : current->graph.steps) {
            if (feed.kind != StepKind::kPipe || feed.destination == kNone) {
              continue;
            }
            const auto& target = current->graph.refs[feed.destination];
            if (target.kind != RefKind::kCallPort || target.call != call_id ||
                target.direction != syntax::PortDirection::kInput ||
                target.name != name) {
              continue;
            }
            supplied = true;
            if (feed.source == kNone) {
              continue;
            }
            const auto& source = current->graph.refs[feed.source];
            if (source.kind != RefKind::kExpr || source.expr == kNone) {
              continue;
            }
            const auto& expression = current->graph.exprs[source.expr];
            if (expression.node == nullptr) {
              continue;
            }
            const auto constant = syntax::ConstantValue(expression.node);
            if (!constant.has_value() ||
                constant->kind == syntax::Constant::Kind::kNull) {
              continue;
            }
            std::string type;
            switch (constant->kind) {
              case syntax::Constant::Kind::kNull:
                type = "null";
                break;
              case syntax::Constant::Kind::kBool:
                type = "boolean";
                break;
              case syntax::Constant::Kind::kInteger:
                type = "integer";
                break;
              case syntax::Constant::Kind::kDouble:
                type = "number";
                break;
              case syntax::Constant::Kind::kString:
                type = "string";
                break;
              case syntax::Constant::Kind::kDuration:
                type = "duration";
                break;
              case syntax::Constant::Kind::kList:
                type = "list";
                break;
              case syntax::Constant::Kind::kObject:
                type = "object";
                break;
            }
            if (RefinesType(type, p.type, evidence) == Refinement::kNo) {
              return absl::FailedPreconditionError(
                  absl::StrCat("Call '", step.action, "', input '", name,
                               "' has an incompatible literal payload."));
            }
          }
          if (direction == syntax::PortDirection::kInput && p.required &&
              p.autofills.empty() && !supplied) {
            return absl::FailedPreconditionError(
                absl::StrCat("Call '", step.action,
                             "' is missing required input '", name, "'."));
          }
        }
      }
      for (const auto& [name, h] : schema.headers) {
        HeaderPlan header;
        header.name = name;
        header.has_default = h.default_value.has_value();
        if (header.has_default) {
          header.default_value = syntax::Constant::String(*h.default_value);
        }
        actual.headers.push_back(std::move(header));
      }
      auto imported_interface = DeclareAction(schema);
      if (imported_interface.diagnostics.empty()) {
        actual = std::move(imported_interface.interface);
        for (auto& shape : imported_interface.types) {
          evidence.dtos.push_back(std::move(shape));
        }
      }
      if (const auto* nested = program_->Flow(step.action);
          nested != nullptr && nested->implementation.has_value()) {
        actual = *nested->implementation;
      }
      if (const auto* declaration = program_->Flow(step.action);
          declaration != nullptr) {
        for (const auto& expected : declaration->contracts) {
          const auto checked = CheckImplementation(actual, expected, evidence);
          if (!checked.ok()) {
            return absl::FailedPreconditionError(
                std::string(checked.message()));
          }
        }
      }
      for (const auto& [key, ref] : step.ports) {
        const graph::Ref& p = current->graph.refs[ref];
        const auto& ports = p.direction == syntax::PortDirection::kInput
                                ? schema.inputs
                                : schema.outputs;
        if (ports.find(p.name) == ports.end()) {
          return absl::NotFoundError(absl::StrCat(
              step.action, " has no ",
              p.direction == syntax::PortDirection::kInput ? "input" : "output",
              " port '", p.name, "'."));
        }
      }
      if (const ResolvedFlow* nested = program_->Flow(step.action);
          nested != nullptr && !nested->plan.interface_only) {
        pending.push_back(nested);
      }
    }
  }
  return absl::OkStatus();
}

absl::Status Runner::RunCall(Scope& scope, StepId step) {
  ABSL_ASSIGN_OR_RETURN(CallHandle * handle, scope.Call(step));
  if (const absl::Status started = StartCall(scope, step, *handle);
      !started.ok()) {
    // A step that never started still has to say so: everything wiring itself
    // to this call waits for it, and an unanswered wait is a deadlock rather
    // than a failure anybody can see.
    handle->NeverStarted(started);
    if (const auto action = handle->action_now(); action != nullptr) {
      action->Cancel().IgnoreError();
    }
    scope.CallFinished(step, started);
    return graph().steps[step].tolerant ? absl::OkStatus() : started;
  }
  const auto status = PumpCall(step, *handle);
  const auto cause = handle->error();
  scope.CallFinished(step, cause.ok() ? status : cause);
  return status;
}

absl::Status Runner::StartCall(Scope& scope, StepId step, CallHandle& handle) {
  ABSL_RETURN_IF_ERROR(scope.work_->CheckActive());
  const graph::Step& one = graph().steps[step];
  ABSL_ASSIGN_OR_RETURN(const absl::Time deadline, scope.DeadlineAt(one));
  handle.SetDeadline(deadline);
  if (deadline <= absl::Now()) {
    return absl::DeadlineExceededError(
        "The call's deadline expired before activation.");
  }
  if (thread::Cancelled() || action_->Cancelled()) {
    return absl::CancelledError("Flow stopped before action activation");
  }
  actions::ActionSchema schema;
  actions::ActionHandler handler;
  ABSL_RETURN_IF_ERROR(Resolve(one.action, &schema, &handler));
  const bool local = one.mode == "run";
  if (local && handler == nullptr) {
    return Fail(
        absl::StrCat(plan().name, " says 'run ", one.action, "', but ",
                     one.action,
                     " is registered here for its schema alone and has no "
                     "handler to run. Say 'call' to dispatch it on the stream "
                     "this flow is attached to."),
        absl::StatusCode::kFailedPrecondition);
  }

  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<actions::Action> nested,
                        action_->MakeNested(schema));
  handle.Adopt(nested);
  scope.work_->OwnAction(nested);
  if (!one.node_map.empty()) {
    ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<nodes::NodeMap> map,
                          NodeMapNamed(one.node_map));
    ABSL_RETURN_IF_ERROR(nested->BindNodeMap(map));
  }
  if (!local && dispatch_stream_ != nullptr) {
    // A peer call uses the dispatch stream, which the flow action does not
    // hold.
    ABSL_RETURN_IF_ERROR(nested->BindStream(dispatch_stream_));
  }
  if (local) {
    ABSL_RETURN_IF_ERROR(nested->BindHandler(handler));
    if (!one.tee) {
      // Keep a local step's streams off the wire: the peer that dispatched this
      // flow asked for its outputs, not for every intermediate node inside it.
      ABSL_RETURN_IF_ERROR(nested->BindStream(nullptr));
    }
  }
  // `forward headers` first, so an explicit `with` of the same name -- the more
  // specific of the two -- is the one that lands.
  for (auto& [name, value] : Forwarded(one)) {
    ABSL_RETURN_IF_ERROR(nested->SetHeader(name, value));
  }
  for (const auto& [name, expr] : one.headers) {
    ABSL_ASSIGN_OR_RETURN(const Value value, scope.Evaluate(expr));
    ABSL_RETURN_IF_ERROR(nested->SetHeader(
        name,
        value.kind() == Value::Kind::kBytes ? value.text() : AsText(value)));
  }
  if (one.action_id != kNone) {
    ABSL_ASSIGN_OR_RETURN(const Value value, scope.Evaluate(one.action_id));
    ABSL_RETURN_IF_ERROR(nested->SetId(AsText(value)));
  }

  // Create every port node before the action starts, so a reader that
  // subscribes later still sees the whole stream from its beginning.
  absl::flat_hash_set<std::string> written;
  for (const RefId ref : scope.analysis_->destinations) {
    const graph::Ref& port = graph().refs[ref];
    if (port.kind == RefKind::kCallPort && port.call == step) {
      written.insert(port.name);
    }
  }
  absl::flat_hash_set<std::string> read;
  for (const RefId ref : scope.analysis_->refs) {
    const graph::Ref& port = graph().refs[ref];
    if (port.kind != RefKind::kCallPort || port.call != step) {
      continue;
    }
    if (port.direction != syntax::PortDirection::kOutput) {
      continue;
    }
    const auto readers = scope.analysis_->readers.find(ref);
    if (readers != scope.analysis_->readers.end() && readers->second > 0) {
      read.insert(port.name);
    }
  }
  std::vector<NodePtr> unclosed;
  for (const auto& [name, declared] : schema.inputs) {
    ABSL_ASSIGN_OR_RETURN(const NodePtr node,
                          nested->GetInput(name, std::nullopt));
    if (!written.contains(name) && declared.autofills.empty()) {
      unclosed.push_back(node);
    }
  }
  std::vector<NodePtr> undrained;
  for (const auto& [name, declared] : schema.outputs) {
    ABSL_ASSIGN_OR_RETURN(const NodePtr node,
                          nested->GetOutput(name, std::nullopt));
    if (!read.contains(name)) {
      undrained.push_back(node);
    }
  }
  handle.unclosed = std::move(unclosed);
  handle.undrained = std::move(undrained);
  handle.Started(nested);
  if (thread::Cancelled() || action_->Cancelled()) {
    nested->Cancel().IgnoreError();
    return absl::CancelledError("Flow stopped during action activation");
  }
  return absl::OkStatus();
}

absl::Status Runner::PumpCall(StepId step, CallHandle& handle) {
  const graph::Step& one = graph().steps[step];
  const std::shared_ptr<actions::Action> nested = handle.action_now();
  absl::Status start;
  if (one.mode == "run") {
    start = nested->Run().status();
  } else if (const absl::Status called = nested->Call().Await().status();
             !called.ok()) {
    start = Fail(absl::StrCat(plan().name, " could not call '", one.action,
                              "' on its session: ", called.message()),
                 called.code());
  }
  if (!start.ok()) {
    handle.NeverStarted(start);
    nested->Cancel().IgnoreError();
    return one.tolerant ? absl::OkStatus() : start;
  }
  {
    Group group(*monitor_);
    for (const NodePtr& node : handle.unclosed) {
      group.Spawn([node]() -> absl::Status { return CloseNode(node); });
    }
    const bool tolerant = one.tolerant;
    for (const NodePtr& node : handle.undrained) {
      group.Spawn([node, tolerant]() -> absl::Status {
        while (true) {
          absl::StatusOr<std::optional<data::NodeFragment>> fragment =
              node->NextFragment().Await();
          if (!fragment.ok()) {
            return tolerant ? absl::OkStatus() : fragment.status();
          }
          if (!fragment->has_value()) {
            return absl::OkStatus();
          }
        }
      });
    }
    group.Spawn([this, step, &handle]() -> absl::Status {
      return AwaitCall(graph().steps[step], handle);
    });
    const absl::Status status = group.Join();
    handle.Done();
    if (!status.ok()) {
      const absl::Status error = handle.error();
      return absl::IsDeadlineExceeded(error) && !one.tolerant ? error : status;
    }
  }
  if (const absl::Status error = handle.error(); !error.ok() && !one.tolerant) {
    return error;
  }
  return absl::OkStatus();
}

absl::Status Runner::AwaitCall(const graph::Step& step, CallHandle& handle) {
  const std::shared_ptr<actions::Action> action = handle.action_now();
  if (action == nullptr) {
    return absl::OkStatus();
  }
  const absl::Status status =
      action
          ->Wait(std::min(step.timeout.value_or(absl::InfiniteDuration()),
                          handle.deadline() - absl::Now()))
          .Await()
          .status();
  if (status.ok()) {
    return absl::OkStatus();
  }
  if (absl::IsCancelled(status) && action->Cancelled() &&
      !action_->Cancelled()) {
    // A `cancel` statement cancels the call, which cancels the wait for it.
    // That is this call's outcome, not the flow being cancelled -- unless the
    // flow really is going away too.
    handle.SetError(action->GetStatus());
    return absl::OkStatus();
  }
  handle.SetError(status);
  if (absl::IsDeadlineExceeded(status)) {
    // End the producer before joining output drains that depend on its end.
    // The saved timeout remains the call's cause through cancellation cleanup.
    action->Cancel().IgnoreError();
  }
  return absl::OkStatus();
}

}  // namespace a11::flow::internal
