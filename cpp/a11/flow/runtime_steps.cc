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

absl::Status Scope::Execute(StepId step) {
  const graph::Step& one = graph().steps[step];
  switch (one.kind) {
    case StepKind::kCall:
      return runner_->RunCall(*this, step);
    case StepKind::kPipe: {
      if (auto stats = runner_->transport_stats; stats != nullptr) {
        stats->transfer_tasks.fetch_add(1, std::memory_order_relaxed);
      }
      const auto destinations = PhysicalDestinations(step);
      const auto sources = graph::TransferSources(one);
      size_t next = 0;
      bool subscribed = false;
      const auto transfer = [&]() -> absl::Status {
        for (auto* destination : destinations) {
          ABSL_RETURN_IF_ERROR(destination->CheckWritable());
        }
        if (one.destination != kNone &&
            graph().refs[one.destination].kind == RefKind::kSink) {
          const auto parent = graph().refs[one.destination].subject_step;
          ABSL_RETURN_IF_ERROR(OperationDone(parent));
        }
        std::vector<ItemPtr> batch;
        batch.reserve(kQueueDepth);
        size_t literal_bytes = 0;
        const auto flush = [&]() -> absl::Status {
          for (auto* destination : destinations) {
            ABSL_RETURN_IF_ERROR(destination->WriteBatch(batch, &bridge()));
          }
          batch.clear();
          literal_bytes = 0;
          return absl::OkStatus();
        };
        for (; next < sources.size(); ++next) {
          subscribed = false;
          if (direct_literals_.find(sources[next]) != direct_literals_.end()) {
            const auto& ref = graph().refs[sources[next]];
            if (ref.phase_gate != kNone) {
              ABSL_RETURN_IF_ERROR(StepDone(ref.phase_gate));
            }
            if (ref.skip == 0) {
              ABSL_ASSIGN_OR_RETURN(auto value, Evaluate(ref.expr));
              const auto& expression = graph().exprs[ref.expr];
              const size_t bytes =
                  expression.node->extent.end - expression.node->extent.start;
              if (literal_bytes + bytes > 65536) {
                ABSL_RETURN_IF_ERROR(flush());
              }
              batch.push_back(Item::Of(std::move(value)));
              literal_bytes += bytes;
              if (batch.size() >= kQueueDepth || literal_bytes >= 65536) {
                ABSL_RETURN_IF_ERROR(flush());
              }
            }
            if (auto stats = runner_->transport_stats; stats != nullptr) {
              stats->literal_transfers.fetch_add(1, std::memory_order_relaxed);
            }
          } else {
            ABSL_RETURN_IF_ERROR(flush());
            ABSL_ASSIGN_OR_RETURN(auto reader, Subscribe(sources[next]));
            subscribed = true;
            while (true) {
              batch.clear();
              ABSL_RETURN_IF_ERROR(reader->NextMany(batch, kQueueDepth));
              if (batch.empty()) {
                break;
              }
              for (auto* destination : destinations) {
                ABSL_RETURN_IF_ERROR(destination->WriteBatch(batch, &bridge()));
              }
            }
          }
          if (one.transfer != nullptr) {
            for (const auto& join : one.transfer->joins) {
              if (join.source != sources[next]) {
                continue;
              }
              if (join.expression) {
                auto cleanup = ExpressionCleanup(join.target);
                if (cleanup != nullptr) {
                  ABSL_RETURN_IF_ERROR(cleanup->future().Await().status());
                }
              } else {
                ABSL_RETURN_IF_ERROR(StepDone(join.target));
              }
            }
          }
        }
        return flush();
      };
      auto result = transfer();
      // Unvisited sources still own reader reservations on lazy transports.
      if (!subscribed && next < sources.size()) {
        SkipRef(sources[next]);
      }
      for (size_t id = next + 1; id < sources.size(); ++id) {
        SkipRef(sources[id]);
      }
      for (const auto expr : one.expression_cleanups) {
        auto cleanup = ExpressionCleanup(expr);
        if (cleanup != nullptr) {
          const auto status = cleanup->future().Await().status();
          if (!status.ok()) {
            Record(step, status);
          }
        }
      }
      if (!result.ok() && one.tolerant) {
        Record(step, result);
        return absl::OkStatus();
      }
      return result;
    }
    case StepKind::kSkip: {
      // Values are removed where the stream is produced.
      return absl::OkStatus();
    }
    case StepKind::kCapture: {
      ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(one.source));
      ABSL_ASSIGN_OR_RETURN(const ItemPtr item, reader->Next());
      Value value;
      if (item != nullptr) {
        ABSL_ASSIGN_OR_RETURN(value, item->Read(&bridge()));
      }
      reader->Stop();
      if (one.slot.starts_with("deadline:") ||
          one.slot.starts_with("timeout:")) {
        const bool absolute = one.slot.starts_with("deadline:");
        if (value.kind() !=
            (absolute ? Value::Kind::kTime : Value::Kind::kDuration)) {
          return Fail(absolute
                          ? "A deadline expression must produce std::time."
                          : "A timeout expression must produce std::duration.");
        }
        syntax::TypeExpression type;
        type.name = absolute ? "time" : "duration";
        ABSL_ASSIGN_OR_RETURN(
            value,
            Coerce(value, type,
                   CoerceContext{.bridge = &bridge(), .shapes = &shapes()}));
        if (type.name == "duration" &&
            value.duration() <= absl::ZeroDuration()) {
          return Fail("A timeout must be positive.");
        }
      }
      if (runner_layout().step_captures[step] ==
              runner_layout().result_capture &&
          expression_result_ != nullptr) {
        expression_result_->SetValue(value).IgnoreError();
      }
      thread::MutexLock lock(&monitor().mu());
      Capture(runner_layout().step_captures[step]) = value;
      return absl::OkStatus();
    }
    case StepKind::kWait:
      if (one.scope_barrier) {
        for (const auto before : one.preceding_steps) {
          if (!graph().steps[before].deferred) {
            ABSL_RETURN_IF_ERROR(StepDone(before));
          }
        }
        for (const auto expr : one.expression_cleanups) {
          if (graph().exprs[expr].pipeline_owned) {
            continue;
          }
          auto cleanup = ExpressionCleanup(expr);
          if (cleanup != nullptr) {
            ABSL_RETURN_IF_ERROR(cleanup->future().Await().status());
          }
        }
        absl::flat_hash_set<StepId> joined;
        bool changed = true;
        while (changed) {
          changed = false;
          for (const auto before : one.preceding_steps) {
            bool started;
            {
              thread::MutexLock lock(&monitor().mu());
              started = Step(before).started;
            }
            if (started && joined.insert(before).second) {
              changed = true;
              ABSL_RETURN_IF_ERROR(StepDone(before));
            }
          }
          for (auto& [ref, bus] : buses_) {
            const auto& stream = graph().refs[ref];
            if (stream.kind != RefKind::kCallPort ||
                stream.direction != syntax::PortDirection::kOutput ||
                std::find(one.preceding_steps.begin(),
                          one.preceding_steps.end(),
                          stream.call) == one.preceding_steps.end()) {
              continue;
            }
            bool started;
            {
              thread::MutexLock lock(&monitor().mu());
              started = Step(stream.call).started;
            }
            if (started) {
              ABSL_RETURN_IF_ERROR(bus->WaitFinished());
            }
          }
        }
        return absl::OkStatus();
      }
      [[fallthrough]];
    case StepKind::kClose:
      return RunWait(step);
    case StepKind::kCancel: {
      if (one.target == kNone) {
        return absl::OkStatus();
      }
      if (graph().steps[one.target].kind == StepKind::kPipe) {
        auto* owner = this;
        while (owner != nullptr &&
               owner->body_ != graph().steps[one.target].body) {
          owner = owner->parent_;
        }
        if (owner == nullptr) {
          return Fail("The continuation has no active owning scope.");
        }
        owner
            ->AbortDestinations(
                one.target, absl::CancelledError("Continuation sink cancelled"))
            .IgnoreError();
        return absl::OkStatus();
      }
      ABSL_ASSIGN_OR_RETURN(CallHandle * handle, Call(one.target));
      ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<actions::Action> action,
                            handle->Action());
      return action->Cancel();
    }
    case StepKind::kFail:
      return Failure(step);
    case StepKind::kAbort:
      return AbortNode(step);
    case StepKind::kLog:
      return WriteLog(one.log, nullptr);
    case StepKind::kBlock: {
      // The same nesting an `if` branch runs in. What a block adds is that its
      // outcome is *its own*: a failure inside it is the block's, and a `try`
      // says the flow means to read it rather than end there.
      if (one.bodies.empty()) {
        return absl::OkStatus();
      }
      Scope child(*runner_, one.bodies.front(), this, {});
      const absl::Status ran = child.Run();
      if (ran.ok()) {
        PublishVariables(one, child);
      }
      Record(step, ran);
      return one.tolerant ? absl::OkStatus() : ran;
    }
    case StepKind::kIf: {
      ABSL_ASSIGN_OR_RETURN(Value taken, Evaluate(one.condition));
      bool selected;
      absl::flat_hash_map<RefId, std::vector<ItemPtr>> presets;
      if (one.item != kNone) {
        if (!taken.is_option()) {
          const auto& type = graph().refs[one.item].stage.type;
          if (!type.has_value()) {
            return Fail("An optional binding requires an option.");
          }
          ABSL_ASSIGN_OR_RETURN(taken,
                                Coerce(taken, *type, {&bridge(), &shapes()}));
        }
        const auto* flag = taken.Get("has_value");
        if (flag == nullptr || flag->kind() != Value::Kind::kBool) {
          return Fail("An option requires a boolean has_value field.");
        }
        selected = flag->boolean();
        if (selected) {
          const auto* payload = taken.Get("value");
          if (payload == nullptr) {
            return Fail("A present option requires a value.");
          }
          if (!graph().bodies[one.bodies.front()].steps.empty()) {
            presets[one.item] = {Item::Of(*payload)};
          }
        }
      } else {
        selected = Truthy(taken);
      }
      taken = Value::Null();
      const BodyId body = selected ? one.bodies.front() : one.bodies.back();
      if (graph().bodies[body].steps.empty()) {
        return absl::OkStatus();
      }
      Scope child(*runner_, body, this, std::move(presets));
      ABSL_RETURN_IF_ERROR(child.Run());
      PublishVariables(one, child);
      return absl::OkStatus();
    }
    case StepKind::kForEach:
    case StepKind::kRepeat: {
      // Recorded so a name bound to the loop can read it, exactly as a block's
      // is. OK when every pass succeeded, which is what `group.Join()` already
      // answers -- including for a loop its `until` ended early.
      const absl::Status ran =
          one.kind == StepKind::kForEach ? RunForEach(step) : RunRepeat(step);
      Record(step, ran);
      return ran;
    }
  }
  return Fail(
      absl::StrCat("Cannot run a ", graph::StepKindName(one.kind), "."));
}

absl::Status Scope::RunWaitMany(StepId step) {
  const graph::Step& one = graph().steps[step];
  ABSL_ASSIGN_OR_RETURN(const absl::Time deadline, DeadlineAt(one));
  if (!one.race) {
    absl::Status first;
    for (size_t i = 0; i < one.subjects.size(); ++i) {
      ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(one.subjects[i]));
      ABSL_ASSIGN_OR_RETURN(ItemPtr item, reader->Next());
      if (item == nullptr) {
        continue;
      }
      ABSL_ASSIGN_OR_RETURN(Value record, item->Read(&bridge()));
      const absl::Status status = StatusOfRecord(record);
      if (first.ok()) {
        first = status;
      }
      if (!status.ok() &&
          (i >= one.subject_tolerant.size() || !one.subject_tolerant[i])) {
        return status;
      }
    }
    Record(step, first);
    return absl::OkStatus();
  }

  struct Candidate {
    CallHandle* call = nullptr;
    Scope* scope = nullptr;
    StepId step = kNone;
    RefId node = kNone;
  };

  struct Observed {
    std::shared_ptr<Monitor> monitor;
    std::vector<std::optional<absl::Status>> status;
    std::vector<uint64_t> order;
  };

  struct Subscriptions {
    std::vector<a11::Task> tasks;

    ~Subscriptions() {
      for (const auto& task : tasks) {
        task.Cancel().IgnoreError();
      }
    }
  } subscriptions;

  auto observed = std::make_shared<Observed>();
  observed->monitor = runner_->shared_monitor();
  observed->status.resize(one.subjects.size());
  observed->order.resize(one.subjects.size());
  std::vector<Candidate> candidates;
  for (const RefId subject : one.subjects) {
    const graph::Ref& status = graph().refs[subject];
    Candidate candidate;
    candidate.step = status.subject_step;
    candidate.node = status.subject;
    if (candidate.step != kNone) {
      for (Scope* scope = this; scope != nullptr; scope = scope->parent_) {
        if (scope->body_ != graph().steps[candidate.step].body) {
          continue;
        }
        candidate.scope = scope;
        ABSL_RETURN_IF_ERROR(scope->EnsureStep(candidate.step));
        if (graph().steps[candidate.step].kind == StepKind::kCall) {
          ABSL_ASSIGN_OR_RETURN(candidate.call, scope->Call(candidate.step));
        }
        break;
      }
      if (candidate.scope == nullptr) {
        return Fail("Completion subject is outside its execution scope.");
      }
    } else if (candidate.node != kNone) {
      const graph::Ref& node = graph().refs[candidate.node];
      if (node.kind == RefKind::kCallPort) {
        ABSL_ASSIGN_OR_RETURN(CallHandle * handle, Call(node.call));
        (void)handle;
      }
    } else {
      return Fail("Race subject has no completion identity.");
    }
    candidates.push_back(candidate);
  }
  if (candidates.empty()) {
    return Fail("A race needs at least one subject.");
  }
  absl::flat_hash_map<nodes::AsyncNode*,
                      std::pair<NodePtr, std::vector<size_t>>>
      node_subjects;
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (candidates[i].node == kNone) {
      continue;
    }
    ABSL_ASSIGN_OR_RETURN(NodePtr node, ReadableNode(candidates[i].node));
    if (node == nullptr) {
      ABSL_ASSIGN_OR_RETURN(node, DestinationNode(candidates[i].node));
    }
    if (node == nullptr) {
      return Fail("Race subject is not a node-backed stream.");
    }
    auto& entry = node_subjects[node.get()];
    entry.first = node;
    entry.second.push_back(i);
  }
  for (const auto& [identity, entry] : node_subjects) {
    auto completion = entry.first->ObserveCompletion();
    subscriptions.tasks.push_back(completion);
    completion.OnReady([observed, indices = entry.second](
                           const absl::StatusOr<a11::Unit>& result) {
      {
        thread::MutexLock lock(&observed->monitor->mu());
        const auto order = observed->monitor->CompletionOrder();
        for (const auto i : indices) {
          observed->status[i] = result.status();
          observed->order[i] = order;
        }
      }
      observed->monitor->Wake();
    });
  }
  size_t winner = 0;
  {
    thread::MutexLock lock(&monitor().mu());
    bool initial = true;
    const auto ready = [&] {
      uint64_t earliest = std::numeric_limits<uint64_t>::max();
      bool found = false;
      for (size_t i = 0; i < candidates.size(); ++i) {
        const Candidate& candidate = candidates[i];
        const bool terminal =
            candidate.call != nullptr ? candidate.call->finished()
            : candidate.scope != nullptr
                ? candidate.scope->Step(candidate.step).phase ==
                      StepState::kDone
                : observed->status[i].has_value();
        if (!terminal) {
          continue;
        }
        const uint64_t order = candidate.call != nullptr
                                   ? candidate.call->completion_order()
                               : candidate.scope != nullptr
                                   ? candidate.scope->Step(candidate.step).order
                                   : observed->order[i];
        if (!found || (!initial && order < earliest)) {
          winner = i;
          earliest = order;
          found = true;
        }
        if (initial) {
          break;
        }
      }
      initial = false;
      return found;
    };
    if (deadline < absl::InfiniteFuture()) {
      ABSL_RETURN_IF_ERROR(monitor().WaitUntil(deadline, ready));
    } else {
      ABSL_RETURN_IF_ERROR(monitor().Wait(ready));
    }
  }
  absl::Status status;
  const Candidate& chosen = candidates[winner];
  if (chosen.call != nullptr) {
    ABSL_RETURN_IF_ERROR(chosen.call->Outcome(&status));
  } else {
    thread::MutexLock lock(&monitor().mu());
    if (chosen.scope != nullptr) {
      const auto& outcome = chosen.scope->Step(chosen.step).outcome;
      if (outcome.has_value()) {
        status = *outcome;
      }
    } else {
      status = *observed->status[winner];
    }
  }
  {
    thread::MutexLock lock(&monitor().mu());
    Step(step).winner = static_cast<int64_t>(winner);
    Step(step).outcome = status;
  }
  const bool tolerated =
      winner < one.subject_tolerant.size() && one.subject_tolerant[winner];
  return tolerated ? absl::OkStatus() : status;
}

/// Read the subject's status, and let a bad one through when it is ours.
///
/// A subject a flow said it would handle -- a `try` step -- reports and the
/// flow carries on. Anything else that finished badly ends the flow here, with
/// the status it finished with rather than a new one.
absl::StatusOr<absl::Time> Scope::DeadlineAt(const graph::Step& step) {
  absl::Time deadline = step.timeout.has_value() ? absl::Now() + *step.timeout
                                                 : absl::InfiniteFuture();
  if (step.timeout_value != graph::kNone) {
    thread::MutexLock lock(&monitor().mu());
    const auto* found =
        Captured(runner_layout().step_captures[step.timeout_value]);
    if (found == nullptr || found->kind() != Value::Kind::kDuration) {
      return Fail("A timeout requires a std::duration value.");
    }
    if (found->duration() <= absl::ZeroDuration()) {
      return Fail("A timeout must be positive.");
    }
    deadline = absl::Now() + found->duration();
  }
  if (step.deadline != graph::kNone) {
    thread::MutexLock lock(&monitor().mu());
    const auto* found = Captured(runner_layout().step_captures[step.deadline]);
    if (found == nullptr || found->kind() != Value::Kind::kTime) {
      return Fail("A deadline requires a std::time value.");
    }
    deadline = std::min(deadline, found->time());
  }
  return deadline;
}

absl::Status Scope::RunWait(StepId step) {
  const graph::Step& one = graph().steps[step];
  ABSL_ASSIGN_OR_RETURN(const absl::Time deadline, DeadlineAt(one));
  if (!one.subjects.empty()) {
    return RunWaitMany(step);
  }
  Value record;
  if (deadline < absl::InfiniteFuture()) {
    // Reading the outcome blocks on whatever the subject is doing, so a timeout
    // is a race against that read rather than something the read itself knows
    // about.
    struct Waiting {
      Monitor* absl_nonnull monitor;
      bool ready = false;
      absl::Status status = {};
      Value value = {};
    };

    Monitor& monitor = this->monitor();
    auto waiting = std::make_shared<Waiting>(Waiting{.monitor = &monitor});
    const RefId outcome = one.outcome;
    Group reading(monitor);
    reading.Spawn([this, outcome, waiting]() -> absl::Status {
      absl::Status status;
      Value value;
      absl::StatusOr<ReaderPtr> reader = Subscribe(outcome);
      if (!reader.ok()) {
        status = reader.status();
      } else if (absl::StatusOr<ItemPtr> item = (*reader)->Next(); !item.ok()) {
        status = item.status();
      } else if (*item != nullptr) {
        absl::StatusOr<Value> read = (*item)->Read(&this->bridge());
        if (read.ok()) {
          value = *std::move(read);
        } else {
          status = read.status();
        }
      }
      {
        thread::MutexLock lock(&waiting->monitor->mu());
        waiting->status = status;
        waiting->value = std::move(value);
        waiting->ready = true;
      }
      waiting->monitor->Wake();
      return absl::OkStatus();
    });
    absl::Status waited;
    {
      thread::MutexLock lock(&monitor.mu());
      waited =
          monitor.WaitUntil(deadline, [waiting] { return waiting->ready; });
      if (waited.ok()) {
        ABSL_RETURN_IF_ERROR(waiting->status);
        record = waiting->value;
      }
    }
    if (absl::IsDeadlineExceeded(waited)) {
      // The group's destructor ends the read on the way out.
      return Fail(absl::StrCat("Waiting for ", SubjectOf(one),
                               " exceeded its deadline at ",
                               absl::FormatTime(deadline), "."),
                  absl::StatusCode::kDeadlineExceeded);
    }
    ABSL_RETURN_IF_ERROR(waited);
    ABSL_RETURN_IF_ERROR(reading.Join());
  } else {
    ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(one.outcome));
    ABSL_ASSIGN_OR_RETURN(const ItemPtr item, reader->Next());
    if (item != nullptr) {
      ABSL_ASSIGN_OR_RETURN(record, item->Read(&bridge()));
    }
  }
  if (record.kind() == Value::Kind::kObject) {
    Record(step, StatusOfRecord(record));
  }
  if (one.tolerant || record.kind() != Value::Kind::kObject) {
    return absl::OkStatus();
  }
  const Value* ok = record.Get("ok");
  if (ok != nullptr && Truthy(*ok)) {
    return absl::OkStatus();
  }
  return StatusOfRecord(record);
}

/// The status a `fail` statement raises.
absl::Status Scope::Failure(StepId step) {
  ABSL_ASSIGN_OR_RETURN(const absl::Status chosen, ChosenStatus(step));
  return chosen;
}

/// The status a `fail` or an `abort` names, evaluated.
///
/// StatusOr of a Status, and the nesting is the point: the outer one is whether
/// the *expressions* could be read, and the inner one is what the flow asked
/// for. A `fail` returns the inner one as its own outcome; an `abort` hands it
/// to a node.
absl::StatusOr<absl::Status> Scope::ChosenStatus(StepId step) {
  const graph::Step& one = graph().steps[step];
  Value code;
  if (!one.code_name.empty()) {
    code = Value::String(one.code_name);
  } else {
    ABSL_ASSIGN_OR_RETURN(code, Evaluate(one.code));
  }
  Value message;
  bool has_message = one.message != kNone;
  if (has_message) {
    ABSL_ASSIGN_OR_RETURN(message, Evaluate(one.message));
  }
  if (!has_message && code.kind() == Value::Kind::kObject) {
    // `fail check` -- raise the status the flow recovered from.
    return StatusOfRecord(code);
  }
  if (!has_message && !StatusCodeOf(code).has_value()) {
    message = code;
    has_message = true;
    code = Value::Null();
  }
  absl::StatusCode resolved =
      StatusCodeOf(code).value_or(absl::StatusCode::kInternal);
  if (resolved == absl::StatusCode::kOk) {
    resolved = absl::StatusCode::kInternal;
  }
  const std::string text = has_message ? AsText(message) : "";
  return Fail(
      text.empty() ? absl::StrCat(runner_->plan().name, " failed.") : text,
      resolved);
}

absl::Status Scope::AbortNode(StepId step) {
  const graph::Step& one = graph().steps[step];
  ABSL_ASSIGN_OR_RETURN(const absl::Status reason, ChosenStatus(step));
  ABSL_ASSIGN_OR_RETURN(Destination * destination,
                        DestinationOf(one.destination));
  return destination->Abort(reason);
}

/// Write one entry to the flow's own log.
///
/// `subject` is the value a stage is looking at, and null in a statement -- the
/// only difference between the two, since `it` means nothing where there is no
/// value in hand.
///
/// Logged values retain their representation. `logf` produces formatted text;
/// `log` preserves records and chunk bytes
/// with their mimetype. Consumers choose
/// how to render the value.
///
/// A failure to log is the flow's, not the log's: writing is what
/// actions::Action::Log already declines to fail on, so what can go wrong here
/// is only evaluating what was written, and that is a mistake in the flow.
absl::Status Scope::WriteLog(const graph::LogTail& tail,
                             const ItemPtr& subject) {
  std::optional<Value> in_hand;
  if (subject != nullptr) {
    ABSL_ASSIGN_OR_RETURN(in_hand, subject->Read(&bridge()));
  }
  const auto evaluate = [&](ExprId expr) -> absl::StatusOr<Value> {
    if (in_hand.has_value()) {
      return EvaluateWith(expr, *in_hand);
    }
    return Evaluate(expr);
  };

  actions::LogOptions options;
  options.level = tail.level;
  // The flow's name rather than the step's: a consumer filtering logs wants the
  // flow they came from, and the line already says which statement it was.
  options.channel = runner_->plan().name;
  if (tail.line > 0) {
    options.lineno = tail.line;
  }

  if (tail.has_format) {
    std::vector<Value> arguments;
    arguments.reserve(tail.arguments.size());
    for (const ExprId expr : tail.arguments) {
      ABSL_ASSIGN_OR_RETURN(Value value, evaluate(expr));
      arguments.push_back(std::move(value));
    }
    return Logged(runner_->action()->Log(
        [&] {
          const auto& formats = runner_->prepared().formats;
          const auto format = formats.find(tail.format);
          return format == formats.end()
                     ? Strformat(Value::String(tail.format), arguments)
                     : StrformatCompiled(format->second, arguments);
        }(),
        options));
  }

  if (!tail.arguments.empty()) {
    ABSL_ASSIGN_OR_RETURN(const Value value, evaluate(tail.arguments.front()));
    return Logged(LogValue(value, /*carried=*/nullptr, options));
  }

  // `| log` with nothing written logs the value going past, which is what the
  // stage is for.
  if (subject != nullptr) {
    return Logged(LogValue(*in_hand, subject.get(), options));
  }
  return absl::OkStatus();
}

/// Log one value as the value it is.
///
/// The one rule, and the same one every language's `log` follows: a string is
/// text, and everything else keeps its own representation. `carried` is the
/// item the value was read out of, where there is one -- a chunk that arrived
/// on a port is logged as the bytes and mimetype it arrived with rather than
/// decoded and re-encoded, so audio stays audio and msgpack stays msgpack.
absl::Status Scope::LogValue(const Value& value,
                             const Item* absl_nullable carried,
                             const actions::LogOptions& options) {
  const std::shared_ptr<actions::Action>& action = runner_->action();
  if (value.kind() == Value::Kind::kString) {
    // Built here rather than asked of the host: a host is entitled to answer a
    // request for `text/plain` with the JSON spelling of the string the native
    // bridge does and a logged string is the characters, not a quoted.
    data::Chunk chunk;
    chunk.metadata =
        data::ChunkMetadata{.mimetype = std::string(data::kTextMimetype)};
    chunk.data = value.text();
    return action->Log(std::move(chunk), options);
  }
  // A chunk the flow never opened: log the producer's own bytes and mimetype.
  if (value.kind() == Value::Kind::kChunk &&
      !data::IsStatusChunk(value.chunk())) {
    return action->Log(value.chunk(), options);
  }
  if (carried != nullptr && carried->chunk().has_value() &&
      !data::IsStatusChunk(*carried->chunk())) {
    return action->Log(*carried->chunk(), options);
  }
  ABSL_ASSIGN_OR_RETURN(const data::Chunk chunk,
                        bridge().ToChunk(value, /*mimetype=*/{}));
  return action->Log(chunk, options);
}

/// Turn a refused log into the flow's failure, and anything else into nothing.
absl::Status Scope::Logged(const absl::Status& logged) {
  if (logged.ok()) {
    return absl::OkStatus();
  }
  return Fail(absl::StrCat("This log could not be written: ", logged.message()),
              logged.code());
}

/// Remember how a nested body went, for a name bound to it to read.
///
/// A block and a loop both have an outcome of their own rather than a call's,
/// so both record it here and `StatusItem` reads it back after `StepDone`. One
/// helper because the two must agree: a reader waiting on the wake this does
/// is waiting on the same condition either way.
void Scope::Record(StepId step, const absl::Status& outcome) {
  {
    thread::MutexLock lock(&monitor().mu());
    Step(step).outcome = outcome;
  }
  monitor().Wake();
}

void Scope::PublishVariables(const graph::Step& step, const Scope& child) {
  thread::MutexLock lock(&monitor().mu());
  for (const auto& update : step.variable_updates) {
    const auto id = runner_layout().joined_captures[update.result];
    const auto* value = child.Captured(id);
    if (value != nullptr) {
      Capture(id) = *value;
    }
  }
}

void Scope::PresetVariables(
    const graph::Step& step, BodyId body,
    absl::flat_hash_map<RefId, std::vector<ItemPtr>>& presets) {
  thread::MutexLock lock(&monitor().mu());
  for (const auto& update : step.variable_updates) {
    const auto* found =
        Captured(runner_layout().joined_captures[update.result]);
    if (found == nullptr) {
      continue;
    }
    for (const auto& [owner, input] : update.inputs) {
      if (owner == body) {
        presets[input] = {Item::Of(*found)};
      }
    }
  }
}

/// Whether a loop's `until`/`while` condition holds of the pass that just ran.
///
/// One implementation for `repeat` and `for`, because the condition means the
/// same thing to both: it is asked at the *tail* of a pass, so the body always
/// runs at least once however the condition starts out. Each stream the
/// condition reads was captured inside the pass -- the streams themselves are
/// gone by the time the question is asked -- which is what the `condition:`
/// slots hold.
absl::StatusOr<bool> Scope::PassCondition(const graph::Step& one,
                                          const Scope& pass) {
  const graph::Expr& condition = graph().exprs[one.condition];
  absl::flat_hash_map<const syntax::Node*, Value> bound;
  for (const auto& [node, ref] : condition.bound) {
    const auto* value = pass.Captured(runner_layout().condition_captures[ref]);
    bound[node] = value == nullptr ? Value::Null() : *value;
  }
  EvalContext context;
  context.prepared = &runner_->prepared();
  context.bound = &bound;
  context.bridge = &bridge();
  context.shapes = &shapes();
  ABSL_ASSIGN_OR_RETURN(const Value holds,
                        flow::Evaluate(*condition.node, context));
  return Truthy(holds);
}

absl::Status Scope::RunForEach(StepId step) {
  const graph::Step& one = graph().steps[step];
  const BodyId body = one.bodies.front();
  ABSL_ASSIGN_OR_RETURN(const int parallel,
                        Parallelism(one.parallel, one.parallel_expression));
  ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(one.source));
  Group group(monitor(), work_);
  std::int64_t index = 0;
  Monitor& monitor = this->monitor();
  auto running = std::make_shared<int>(0);
  while (true) {
    ABSL_ASSIGN_OR_RETURN(const ItemPtr item, reader->Next());
    if (item == nullptr) {
      break;
    }
    absl::flat_hash_map<RefId, std::vector<ItemPtr>> presets;
    if (one.item != kNone) {
      presets[one.item] = {item};
    }
    if (one.index != kNone) {
      presets[one.index] = {Item::Of(Value::Integer(index))};
    }
    if (index == std::numeric_limits<int64_t>::max()) {
      return absl::ResourceExhaustedError("Flow loop index exhausted.");
    }
    ++index;
    if (parallel == 1) {
      PresetVariables(one, body, presets);
      // Named rather than a temporary, because an `until` reads what the pass
      // captured and a temporary's captures are gone by the semicolon.
      Scope pass(*runner_, body, this, std::move(presets));
      ABSL_RETURN_IF_ERROR(pass.Run());
      PublishVariables(one, pass);
      if (one.condition != kNone) {
        ABSL_ASSIGN_OR_RETURN(const bool holds, PassCondition(one, pass));
        if (holds == one.stop_when) {
          // Stop reading, as `| first n` does. It does not cancel whatever is
          // producing -- see Reader::Stop for why that is deliberate -- so a
          // step with more to say still finishes on its own terms.
          reader->Stop();
          break;
        }
      }
      continue;
    }
    // Admission before the fiber, so a wide `parallel` does not turn a long
    // stream into a pile of pending passes.
    {
      thread::MutexLock lock(&monitor.mu());
      ABSL_RETURN_IF_ERROR(
          monitor.Wait([running, parallel] { return *running < parallel; }));
      ++*running;
    }
    group.Spawn([this, body, presets = std::move(presets), running,
                 &monitor]() mutable -> absl::Status {
      const absl::Status status =
          Scope(*runner_, body, this, std::move(presets)).Run();
      {
        thread::MutexLock lock(&monitor.mu());
        --*running;
      }
      monitor.Wake();
      return status;
    });
  }
  return group.Join();
}

absl::Status Scope::RunRepeat(StepId step) {
  const graph::Step& one = graph().steps[step];
  const BodyId body = one.bodies.front();
  Value carried = Value::Of(one.start);
  // Bounded by the author's `max` where there is one, and otherwise only by the
  // condition: a loop that says `until` means it, however many passes that
  // takes.
  for (int64_t index = 0;
       !one.max_iterations.has_value() || index < *one.max_iterations;
       ++index) {
    absl::flat_hash_map<RefId, std::vector<ItemPtr>> presets;
    if (one.carry != kNone) {
      presets[one.carry] = {Item::Of(carried)};
    }
    if (one.index != kNone) {
      presets[one.index] = {Item::Of(Value::Integer(index))};
    }
    PresetVariables(one, body, presets);
    Scope pass(*runner_, body, this, std::move(presets));
    ABSL_RETURN_IF_ERROR(pass.Run());
    PublishVariables(one, pass);
    if (one.condition != kNone) {
      ABSL_ASSIGN_OR_RETURN(const bool holds, PassCondition(one, pass));
      if (holds == one.stop_when) {
        return absl::OkStatus();
      }
    }
    if (one.carry_source != kNone) {
      const auto* value = pass.Captured(runner_layout().carry_capture);
      carried = value == nullptr ? Value::Null() : *value;
    }
    if (index == std::numeric_limits<int64_t>::max()) {
      return absl::ResourceExhaustedError("Flow repeat index exhausted.");
    }
  }
  return absl::OkStatus();
}

}  // namespace a11::flow::internal
