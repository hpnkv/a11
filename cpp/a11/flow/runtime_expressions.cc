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

#include <algorithm>
#include <limits>

#include "a11/flow/internal/execution.h"

namespace a11::flow::internal {

absl::StatusOr<int> Scope::Parallelism(int count, ExprId expression) {
  if (expression == kNone) {
    return std::max(count, 1);
  }
  ABSL_ASSIGN_OR_RETURN(Value value, Evaluate(expression));
  if (value.kind() != Value::Kind::kInteger || value.integer() < 1 ||
      value.integer() > std::numeric_limits<int>::max()) {
    return absl::InvalidArgumentError(
        "parallel requires a positive signed 32-bit integer.");
  }
  return static_cast<int>(value.integer());
}

absl::StatusOr<ItemPtr> Scope::StatusItem(RefId ref) {
  const graph::Ref& one = graph().refs[ref];
  absl::Status outcome;
  if (one.subject_step != kNone &&
      graph::RecordsOutcome(graph().steps[one.subject_step].kind)) {
    // A block's or a loop's outcome is recorded by the step itself, so reading
    // it is waiting for the step to be over and then looking. Waiting for a
    // step is what `after` does, and this is the same wait.
    ABSL_RETURN_IF_ERROR(StepDone(one.subject_step));
    for (Scope* at = this; at != nullptr; at = at->parent_) {
      thread::MutexLock lock(&monitor().mu());
      if (graph().steps[one.subject_step].body == at->body_ &&
          at->Step(one.subject_step).outcome.has_value()) {
        return Item::Of(StatusRecord(*at->Step(one.subject_step).outcome));
      }
    }
    return Item::Of(StatusRecord(absl::OkStatus()));
  }
  if (one.subject_step != kNone) {
    ABSL_ASSIGN_OR_RETURN(CallHandle * handle, Call(one.subject_step));
    ABSL_RETURN_IF_ERROR(handle->Outcome(&outcome));
    return Item::Of(StatusRecord(outcome));
  }
  if (one.subject != kNone) {
    ABSL_RETURN_IF_ERROR(NodeOutcome(one.subject, &outcome));
    return Item::Of(StatusRecord(outcome));
  }
  return Fail(absl::StrCat(one.label, " has no status to read."));
}

// Which subject of a race finished first, as the value the barrier is. On the
// scope that ran the step, because a nested body's race is that body's.
/// Which subject of a race finished first, as the value the barrier is.
///
/// Reading it is waiting for the barrier -- the same wait `after` does -- and
/// then looking at what it recorded. On the scope that ran the step, because a
/// nested body's race is that body's.
absl::StatusOr<ItemPtr> Scope::WinnerItem(RefId ref) {
  const graph::Ref& one = graph().refs[ref];
  if (one.subject_step == kNone) {
    return Fail(absl::StrCat(one.label, " has no race to read."));
  }
  ABSL_RETURN_IF_ERROR(StepDone(one.subject_step));
  for (Scope* at = this; at != nullptr; at = at->parent_) {
    thread::MutexLock lock(&monitor().mu());
    if (graph().steps[one.subject_step].body == at->body_ &&
        at->Step(one.subject_step).winner.has_value()) {
      return Item::Of(Value::Integer(*at->Step(one.subject_step).winner));
    }
  }
  // The barrier is done and recorded nothing, which happens when it was given
  // up on rather than won. Reading it then says so rather than answering 0.
  return Fail(absl::StrCat(one.label, " never finished."),
              absl::StatusCode::kFailedPrecondition);
}

/// A node's outcome: its writers are done, or its stream has ended.
///
/// A node this flow writes is finished when the last writer has closed it, so
/// the status is the one the node was closed or aborted with. One it only reads
/// is finished when the stream ends, and the status is the reader's -- which is
/// how an output cut off mid-stream gets noticed.
absl::Status Scope::NodeOutcome(RefId ref, absl::Status* absl_nonnull outcome) {
  if (graph().refs[ref].kind == RefKind::kSink) {
    *outcome = StepDone(graph().refs[ref].subject_step);
    return absl::OkStatus();
  }
  if (Destination* written = FindDestination(ref); written != nullptr) {
    if (written->writers() <= 0) {
      // Nothing in this flow writes it, so nothing in this flow will close it
      // either unless this barrier does.
      ABSL_RETURN_IF_ERROR(written->End());
    }
    ABSL_RETURN_IF_ERROR(written->Finished());
    ABSL_ASSIGN_OR_RETURN(const NodePtr node, written->Node());
    const std::optional<absl::Status> aborted = node->GetWriterAbortStatus();
    *outcome = aborted.has_value() ? *aborted : node->GetWriterStatus();
    return absl::OkStatus();
  }
  ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(ref));
  while (true) {
    absl::StatusOr<ItemPtr> item = reader->Next();
    if (!item.ok()) {
      // The producer cut the stream short, and that is the outcome rather than
      // this flow's own failure.
      *outcome = item.status();
      return absl::OkStatus();
    }
    if (*item == nullptr) {
      break;
    }
  }
  ABSL_ASSIGN_OR_RETURN(const NodePtr node, ReadableNode(ref));
  *outcome = node == nullptr ? absl::OkStatus() : node->GetReaderStatus();
  return absl::OkStatus();
}

// --- Expressions -------------------------------------------------------------

absl::StatusOr<Value> Scope::ValueOf(RefId ref, bool take, bool optional) {
  Scope* owner = Owner(ref);
  // A fold's accumulator is not read from a stream: the stage folding right now
  // put it there, and this is the name that reads it.
  {
    thread::MutexLock lock(&monitor().mu());
    const auto slot = runner_layout().fold_slots[ref];
    if (slot != kNone) {
      const auto& folded = owner->frame_->Get(owner->layout_->folds)[slot];
      if (folded.has_value()) {
        return *folded;
      }
    }
  }
  ValueCursor* cursor = nullptr;
  {
    thread::MutexLock lock(&monitor().mu());
    // One cursor per ref, and one *subscription* per ref.
    ABSL_RETURN_IF_ERROR(monitor().Wait([owner, ref] {
      return owner->cursors_.contains(ref) ||
             !owner->opening_cursors_.contains(ref);
    }));
    const auto found = owner->cursors_.find(ref);
    if (found != owner->cursors_.end()) {
      cursor = &*found->second;
    } else {
      owner->opening_cursors_.insert(ref);
    }
  }
  if (cursor == nullptr) {
    // Outside the lock: subscribing may open a node.
    absl::StatusOr<ReaderPtr> reader = Subscribe(ref);
    // Taken before the reader is moved out below: what is left behind still has
    // this status, but a reader of the code should not have to know that.
    absl::Status subscribed = reader.status();
    {
      thread::MutexLock lock(&monitor().mu());
      // Whatever happened, this ref is no longer being opened: a failure that
      // left the flag set would leave every other reader of it waiting for a
      // cursor nobody is making any more.
      owner->opening_cursors_.erase(ref);
      if (subscribed.ok()) {
        const graph::Ref& one = graph().refs[ref];
        cursor = &owner->cursors_.Construct(ref, bridge(), *std::move(reader),
                                            one.unary, one.label);
      }
    }
    monitor().Wake();
    if (!subscribed.ok()) {
      return subscribed;
    }
  }
  if (take) {
    ABSL_ASSIGN_OR_RETURN(auto value, cursor->Extract(monitor()));
    if (optional) {
      return Value::Option(std::move(value));
    }
    if (!value.has_value()) {
      return absl::FailedPreconditionError(
          "Cannot take a value from an empty stream.");
    }
    return std::move(*value);
  }
  return cursor->Next(monitor());
}

absl::StatusOr<Value> Scope::Evaluate(ExprId expr) {
  return EvaluateWith(expr, expression_it_, false);
}

absl::StatusOr<Value> Scope::EvaluateWith(ExprId expr, const Value& it,
                                          bool join) {
  if (expr == kNone) {
    return Value::Null();
  }
  if (work_ != nullptr) {
    ABSL_RETURN_IF_ERROR(work_->CheckActive());
  }
  const graph::Expr& one = graph().exprs[expr];
  if (one.node == nullptr) {
    return Value::Null();
  }
  if (one.activation != kNone) {
    if (work_ == nullptr) {
      return Fail("Expression has no owning execution scope.");
    }
    auto child = std::make_unique<Scope>(
        *runner_, one.activation, this,
        absl::flat_hash_map<RefId, std::vector<ItemPtr>>{});
    child->resource_owner_ =
        join ? child.get()
             : (resource_owner_ == nullptr ? this : resource_owner_);
    child->expression_result_ = std::make_shared<a11::Promise<Value>>();
    child->expression_it_ = it;
    auto result = child->expression_result_->future();
    auto done = std::make_shared<a11::Promise<a11::Unit>>();
    auto completed = done->future();
    if (!join) {
      thread::MutexLock lock(&monitor().mu());
      expression_cleanups_.insert_or_assign(expr, done);
    }
    const bool pipeline_owned = one.pipeline_owned;
    work_->Spawn([child = std::move(child), done, join,
                  pipeline_owned]() mutable -> absl::Status {
      const absl::Status status = child->Run();
      if (!status.ok()) {
        child->expression_result_->SetStatus(status).IgnoreError();
      } else {
        child->expression_result_->SetValue(Value::Null()).IgnoreError();
      }
      child.reset();
      done->SetResult(status.ok() ? absl::StatusOr<a11::Unit>(a11::Unit{})
                                  : absl::StatusOr<a11::Unit>(status))
          .IgnoreError();
      return join || pipeline_owned ? absl::OkStatus() : status;
    });
    auto value = result.Await();
    if (join) {
      const auto cleanup = completed.Await();
      if (!cleanup.ok()) {
        return cleanup.status();
      }
    }
    return value;
  }
  absl::flat_hash_map<RefId, Value> taken;
  EvalContext context;
  context.prepared = &runner_->prepared();
  context.lookup =
      [&](const syntax::Node& node) -> absl::StatusOr<std::optional<Value>> {
    for (const auto& [bound, ref] : one.bound) {
      if (bound != &node) {
        continue;
      }
      const auto* extraction = syntax::As<syntax::PipelineValue>(&node);
      const bool take =
          extraction != nullptr && extraction->take && !one.materialized;
      if (const auto cached = taken.find(ref); !take && cached != taken.end()) {
        return std::optional<Value>(cached->second);
      }
      ABSL_ASSIGN_OR_RETURN(Value value,
                            ValueOf(ref, take, take && extraction->optional));
      taken.emplace(ref, value);
      return std::optional<Value>(std::move(value));
    }
    return std::optional<Value>();
  };
  context.it = it;
  context.has_it = true;
  context.bridge = &bridge();
  context.shapes = &shapes();
  auto value = flow::Evaluate(*one.node, context);
  for (const RefId ref : one.refs) {
    if (taken.find(ref) != taken.end() && expression_result_ != nullptr &&
        graph().refs[ref].owner == body_) {
      const auto cursor = cursors_.find(ref);
      if (cursor != cursors_.end()) {
        cursor->second->Stop();
      }
    }
  }
  return value;
}

void Scope::RetireValueRef(RefId ref) {
  if (RetireValueReservation(ref)) {
    SkipRef(ref);
  }
}

bool Scope::RetireValueReservation(RefId ref) {
  Scope* owner = FindOwner(ref);
  if (owner == nullptr) {
    return false;
  }
  bool unused = false;
  {
    thread::MutexLock lock(&monitor().mu());
    const auto found = owner->layout_->cursors.index.find(ref);
    if (found == owner->layout_->cursors.index.end()) {
      return false;
    }
    int& count =
        owner->frame_->Get(owner->layout_->value_consumers)[found->second];
    if (count == 0) {
      return false;
    }
    --count;
    unused = count == 0 && !owner->cursors_.contains(ref) &&
             !owner->opening_cursors_.contains(ref);
  }
  return unused;
}

void Scope::SkipRef(RefId initial) {
  std::vector<RefId> pending{initial};
  while (!pending.empty()) {
    const RefId ref = pending.back();
    pending.pop_back();
    Scope* owner = FindOwner(ref);
    if (owner == nullptr ||
        owner->buffers_.find(ref) != owner->buffers_.end()) {
      continue;
    }
    const auto slot = owner->single_slots_.find(ref);
    if (slot != owner->single_slots_.end()) {
      if (slot->second->replay() || !slot->second->Skip()) {
        continue;
      }
    } else {
      const auto bus = owner->buses_.find(ref);
      if (bus == owner->buses_.end() || !bus->second->Skip()) {
        continue;
      }
    }
    for (const RefId upstream : graph().Upstreams(ref)) {
      pending.push_back(upstream);
    }
    for (const RefId upstream : graph().ValueRefs(ref)) {
      if (RetireValueReservation(upstream)) {
        pending.push_back(upstream);
      }
    }
    const graph::Ref& node = graph().refs[ref];
    if (node.kind == RefKind::kCallPort && node.call != kNone &&
        graph().steps[node.call].deferred) {
      bool inactive;
      {
        thread::MutexLock lock(&monitor().mu());
        inactive = !owner->Step(node.call).started;
      }
      if (inactive) {
        for (const StepId step : graph().bodies[owner->body_].steps) {
          if (graph().steps[step].activation == node.call) {
            for (const RefId input : graph().Sources(step)) {
              pending.push_back(input);
            }
          }
        }
        for (const RefId input : graph().ValueSources(node.call)) {
          if (RetireValueReservation(input)) {
            pending.push_back(input);
          }
        }
      }
    }
  }
}

absl::StatusOr<Value> Scope::EvaluateFold(const graph::Stage& stage,
                                          const Value& carried,
                                          const Value& it) {
  if (stage.carry != kNone) {
    thread::MutexLock lock(&monitor().mu());
    const auto slot = runner_layout().fold_slots[stage.carry];
    CHECK_NE(slot, kNone);
    frame_->Get(layout_->folds)[slot] = carried;
  }
  return EvaluateWith(stage.expr, it);
}

absl::StatusOr<Value> Divide(const Value& total, const Value& count) {
  const double by = AsDouble(count);
  if (by == 0.0) {
    return Value::Null();
  }
  if (total.kind() == Value::Kind::kDuration) {
    return Value::Duration(total.duration() / by);
  }
  return Value::Double(AsDouble(total) / by);
}

/// Decode every item of a batch that still needs it, in one crossing.
///
/// A no-op when fewer than two values need decoding. Batching reduces host
/// crossings while preserving the direct path for pipes that only move values.
void PrimeBatch(const std::vector<ItemPtr>& items,
                HostBridge* absl_nonnull bridge) {
  std::vector<const data::Chunk*> chunks;
  std::vector<const Item*> owners;
  for (const ItemPtr& item : items) {
    if (!item->NeedsDecoding()) {
      continue;
    }
    chunks.push_back(&*item->chunk());
    owners.push_back(item.get());
  }
  if (chunks.size() < 2) {
    return;
  }
  std::vector<absl::StatusOr<Value>> values = bridge->FromChunks(chunks);
  if (values.size() != owners.size()) {
    return;
  }
  for (size_t index = 0; index < owners.size(); ++index) {
    owners[index]->Prime(std::move(values[index]));
  }
}

/// A mimetype without its parameters: `application/x-msgpack;type=X`.
std::string BaseMimetype(std::string_view mimetype) {
  const size_t at = mimetype.find(';');
  std::string base(at == std::string_view::npos ? mimetype
                                                : mimetype.substr(0, at));
  absl::AsciiStrToLower(&base);
  return std::string(absl::StripAsciiWhitespace(base));
}

/// Whether `name` matches a pattern whose `*` spans any characters.
bool Matches(std::string_view name, std::string_view pattern) {
  const std::vector<std::string_view> parts = absl::StrSplit(pattern, '*');
  if (parts.size() == 1) {
    return name == pattern;
  }
  if (!absl::StartsWith(name, parts.front())) {
    return false;
  }
  if (!absl::EndsWith(name, parts.back())) {
    return false;
  }
  size_t at = parts.front().size();
  const size_t limit = name.size() - parts.back().size();
  if (at > limit) {
    return false;
  }
  for (size_t index = 1; index + 1 < parts.size(); ++index) {
    if (parts[index].empty()) {
      continue;
    }
    const size_t found = name.find(parts[index], at);
    if (found == std::string_view::npos ||
        found + parts[index].size() > limit) {
      return false;
    }
    at = found + parts[index].size();
  }
  return true;
}

bool MatchesFolded(std::string_view name, std::string_view pattern) {
  return Matches(absl::AsciiStrToLower(name), absl::AsciiStrToLower(pattern));
}

std::string SubjectOf(const graph::Step& step) {
  const size_t space = step.label.find(' ');
  if (space == std::string::npos) {
    return step.label;
  }
  return step.label.substr(space + 1);
}

}  // namespace a11::flow::internal
