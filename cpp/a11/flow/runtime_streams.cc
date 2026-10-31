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

#include <optional>

#include <absl/cleanup/cleanup.h>

#include "a11/flow/internal/execution.h"

namespace a11::flow::internal {

absl::StatusOr<ItemPtr> Scope::BoundItem(RefId ref) {
  absl::Cleanup retire = [this, ref] {
    for (const RefId source : graph().ValueRefs(ref)) {
      RetireValueRef(source);
    }
  };
  const auto& one = graph().refs[ref];
  if (one.role == "initialized-value") {
    ABSL_RETURN_IF_ERROR(StepDone(one.bound_by));
    Value value;
    {
      thread::MutexLock lock(&monitor().mu());
      const auto* found = Captured(runner_layout().step_captures[one.bound_by]);
      if (found == nullptr) {
        return Fail("An initialized value is unavailable.");
      }
      value = *found;
    }
    return Item::Of(std::move(value));
  }
  if (one.role == "joined-value") {
    ABSL_RETURN_IF_ERROR(StepDone(one.bound_by));
    std::optional<Value> joined;
    {
      thread::MutexLock lock(&monitor().mu());
      const auto* found = Captured(runner_layout().joined_captures[ref]);
      if (found != nullptr) {
        joined = *found;
      }
    }
    if (joined.has_value()) {
      return Item::Of(std::move(*joined));
    }
  } else if (one.role != "scope-input") {
    return Fail("A bound value is unavailable in this scope.");
  }
  ABSL_ASSIGN_OR_RETURN(Value value, ValueOf(one.source));
  return Item::Of(std::move(value));
}

absl::Status Scope::Produce(RefId ref, const Sink& sink) {
  absl::Cleanup retire = [this, ref] {
    if (graph().refs[ref].kind != RefKind::kBound) {
      for (const RefId source : graph().ValueRefs(ref)) {
        RetireValueRef(source);
      }
    }
  };
  if (graph().refs[ref].phase_gate != kNone) {
    ABSL_RETURN_IF_ERROR(StepDone(graph().refs[ref].phase_gate));
  }
  const graph::Ref& one = graph().refs[ref];
  // One place, upstream of the bus that fans the stream out, so the values
  // `skip n` spoke for are the same ones every reader misses.
  long long taken = 0;
  const long long skip = one.skip;
  const auto pass = [&](ItemPtr item) -> absl::Status {
    if (taken < skip) {
      ++taken;
      return absl::OkStatus();
    }
    return sink.One(std::move(item));
  };
  const Sink skipping(pass);
  const Sink& onward = skip > 0 ? skipping : sink;
  switch (one.kind) {
    case RefKind::kSink:
      return Fail("A continuation sink cannot be read as a stream.");
    case RefKind::kBound: {
      ABSL_ASSIGN_OR_RETURN(ItemPtr item, BoundItem(ref));
      return onward.One(std::move(item));
    }
    case RefKind::kDerived:
      return ProduceStage(ref, onward);
    case RefKind::kZip:
      return ProduceZip(ref, onward);
    case RefKind::kMerge:
      return ProduceMerge(ref, onward);
    case RefKind::kExpr: {
      ABSL_ASSIGN_OR_RETURN(const Value value, Evaluate(one.expr));
      return onward.One(Item::Of(value));
    }
    case RefKind::kHeader: {
      ABSL_ASSIGN_OR_RETURN(const std::optional<data::Bytes> raw,
                            runner_->action()->GetHeader(one.header));
      if (!raw.has_value()) {
        return absl::OkStatus();
      }
      return onward.One(Item::Of(Value::String(*raw)));
    }
    case RefKind::kNodeId: {
      ABSL_ASSIGN_OR_RETURN(const NodePtr node, LocalNode(one.subject));
      ABSL_ASSIGN_OR_RETURN(const std::string id, node->GetId());
      return onward.One(Item::Of(Value::String(id)));
    }
    case RefKind::kNode: {
      ABSL_ASSIGN_OR_RETURN(const NodePtr node, LocalNode(ref));
      return ReadNode(node, onward);
    }
    case RefKind::kCallPort: {
      ABSL_ASSIGN_OR_RETURN(CallHandle * handle, Call(one.call));
      const auto node = handle->Node(one.name, one.direction);
      if (!node.ok()) {
        return node.status();
      }
      const auto status = ReadNode(*node, onward);
      const auto cause = handle->error();
      return absl::IsCancelled(status) && !cause.ok() ? cause : status;
    }
    case RefKind::kFlowPort: {
      ABSL_ASSIGN_OR_RETURN(const NodePtr node,
                            runner_->action()->GetInput(one.name));
      return ReadNode(node, onward);
    }
    default:
      return Fail(
          absl::StrCat(one.label, " is not something a flow can read."));
  }
}

// Read several streams in step, as one stream of tuples. A fiber per source
// would buy nothing and would put three more lifetimes on the run's monitor.
/// Read several streams in step, as one stream of tuples.
///
/// **No fiber of its own per source, and that is not a shortcut.** A tuple is
/// not complete until every source has answered, so asking them one after
/// another finishes at exactly the moment asking them at once would -- and
/// every reader here is an ordinary subscription, which already blocks, already
/// relays a producer's failure, and already has the buffering the analysis
/// arranged. A fiber per source would buy nothing and would put three more
/// lifetimes on the run's monitor.
///
/// A source that ends **well** is latched: from then on it contributes a null
/// to every tuple, which is what lets a short stream be zipped against a long
/// one without either being padded by its author. A source that ends **badly**
/// ends the whole thing with its status, because a tuple missing a value for a
/// reason nobody has been told about is worse than no tuple at all. It stops
/// when every source has ended.
absl::Status Scope::ProduceZip(RefId ref, const Sink& sink) {
  const graph::Ref& one = graph().refs[ref];
  std::vector<ReaderPtr> readers;
  readers.reserve(one.sources.size());
  for (const RefId source : one.sources) {
    ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(source));
    readers.push_back(std::move(reader));
  }
  std::vector<bool> ended(readers.size(), false);
  size_t running = readers.size();
  HostBridge& host = bridge();

  while (running > 0) {
    std::vector<Value> tuple;
    tuple.reserve(readers.size());
    for (size_t index = 0; index < readers.size(); ++index) {
      if (ended[index]) {
        tuple.push_back(Value::Null());
        continue;
      }
      absl::StatusOr<ItemPtr> item = readers[index]->Next();
      if (!item.ok()) {
        // Give up on the rest rather than leave them reading into a buffer
        // nothing will ever take from.
        for (ReaderPtr& reader : readers) {
          reader->Stop();
        }
        return item.status();
      }
      if (*item == nullptr) {
        ended[index] = true;
        --running;
        tuple.push_back(Value::Null());
        continue;
      }
      ABSL_ASSIGN_OR_RETURN(Value value, (*item)->Read(&host));
      tuple.push_back(std::move(value));
    }
    // The round in which the last source ended produced nothing but the nulls
    // that say so, and a tuple of nothing but nulls is not a value anybody
    // wrote.
    if (running == 0) {
      break;
    }
    ABSL_RETURN_IF_ERROR(sink.One(Item::Of(Value::List(std::move(tuple)))));
  }
  return absl::OkStatus();
}

/// Read several streams at once, as one stream of their values.
///
/// **A fiber per source, unlike a zip, and for the opposite reason.** A tuple
/// is not complete until every source has answered, so asking them one after
/// another finishes when asking them at once would. An interleaved value is
/// complete as soon as *one* source has answered, so reading them in turn would
/// make a fast stream wait behind a slow one -- which is the whole thing this
/// exists to avoid. The fibers are the sources the author named, and they are
/// what "in the order values arrive" costs.
///
/// Publishing takes the bus lock, so concurrent values are ordered by lock
/// acquisition. A successful source end contributes no value; a failed source
/// ends the merged stream with its status.
absl::Status Scope::ProduceMerge(RefId ref, const Sink& sink) {
  const graph::Ref& one = graph().refs[ref];
  std::vector<ReaderPtr> readers;
  readers.reserve(one.sources.size());
  for (const RefId source : one.sources) {
    ABSL_ASSIGN_OR_RETURN(ReaderPtr reader, Subscribe(source));
    readers.push_back(std::move(reader));
  }
  Group group(monitor(), work_);
  const auto drain = [&sink](Reader* absl_nonnull reader) -> absl::Status {
    std::vector<ItemPtr> batch;
    batch.reserve(kQueueDepth);
    while (true) {
      batch.clear();
      ABSL_RETURN_IF_ERROR(reader->NextMany(batch, kQueueDepth));
      if (batch.empty()) {
        return absl::OkStatus();
      }
      ABSL_RETURN_IF_ERROR(sink.Many(batch));
    }
  };
  // One of them on this fiber, so `interleave(a, b)` costs one extra fiber and
  // not two.
  for (size_t index = 1; index < readers.size(); ++index) {
    Reader* reader = readers[index].get();
    group.Spawn([&drain, reader]() -> absl::Status { return drain(reader); });
  }
  absl::Status mine;
  if (!readers.empty()) {
    mine = drain(readers.front().get());
  }
  absl::Status joined = group.Join();
  ABSL_RETURN_IF_ERROR(mine);
  return joined;
}

/// Read a derived ref's source and reshape it with one stage.
absl::Status Scope::ProduceStage(RefId ref, const Sink& sink) {
  const graph::Ref& one = graph().refs[ref];
  std::optional<graph::Stage> configured;
  if (one.stage.parallel_expression != kNone) {
    configured = one.stage;
    ABSL_ASSIGN_OR_RETURN(
        configured->parallel,
        Parallelism(one.stage.parallel, one.stage.parallel_expression));
  }
  const graph::Stage& stage = configured.has_value() ? *configured : one.stage;
  ABSL_ASSIGN_OR_RETURN(ReaderPtr source, Subscribe(one.source));
  HostBridge& host = bridge();
  const std::string& name = stage.name;

  if (name == "consume") {
    ABSL_ASSIGN_OR_RETURN(ItemPtr item, source->Next());
    if (item == nullptr && stage.optional) {
      return absl::OkStatus();
    }
    if (item == nullptr) {
      return absl::FailedPreconditionError(
          "'consume' requires exactly one item; the stream is empty.");
    }
    ABSL_ASSIGN_OR_RETURN(ItemPtr extra, source->Next());
    if (extra != nullptr) {
      source->Stop();
      return absl::FailedPreconditionError(
          "'consume' requires exactly one item; a second item arrived.");
    }
    return sink.One(std::move(item));
  }

  // For a stage that gathers: one value at a time, and whatever it publishes it
  // publishes itself. Reading is batched even here, since the source hands back
  // what it already had.
  std::vector<ItemPtr> pending;
  pending.reserve(kQueueDepth);
  const auto each = [&](absl::FunctionRef<absl::Status(const ItemPtr&)> body)
      -> absl::Status {
    while (true) {
      pending.clear();
      ABSL_RETURN_IF_ERROR(source->NextMany(pending, kQueueDepth));
      if (pending.empty()) {
        return absl::OkStatus();
      }
      for (const ItemPtr& item : pending) {
        ABSL_RETURN_IF_ERROR(body(item));
      }
    }
  };

  /// For a stage that reshapes each value: a batch in, a batch out.
  ///
  /// The input batch is only ever what the source *already had*, so nothing is
  /// held back waiting for company -- a stage handed one value publishes one
  /// value, and a pipeline still paces itself value by value. What the batch
  /// saves is the handover: one lock and one broadcast at each end for up to
  /// `kQueueDepth` values instead of one of each per value, which on a moving
  /// pipeline was most of what a value cost.
  ///
  /// A body that fails publishes what it had produced first, so a reader that
  /// had seen three of five values sees three values and then the failure --
  /// the same order the one-at-a-time path gave it.
  /// What a `try` does with a value the stage could not do.
  ///
  /// The failure is a value like any other: written to the stream `into` named,
  /// as the same status record `status x` yields, or -- with nowhere to send it
  /// -- logged once at warning, which is the least a language whose failures
  /// are values should do with one it was told to tolerate.
  const auto tolerate = [&](const ItemPtr& item,
                            const absl::Status& why) -> absl::Status {
    if (stage.failures != kNone) {
      ABSL_ASSIGN_OR_RETURN(Destination * destination,
                            DestinationOf(stage.failures));
      return destination->Write(Item::Of(StatusRecord(why)), &host);
    }
    actions::LogOptions options;
    options.level = "warning";
    return LogValue(Value::String(absl::StrCat(
                        one.label, " skipped a value: ", why.message())),
                    item.get(), options);
  };

  const auto per_value =
      [&](absl::FunctionRef<absl::Status(const ItemPtr&, std::vector<ItemPtr>&)>
              body,
          bool reads_values = true) -> absl::Status {
    if (stage.parallel > 1) {
      return InParallel(stage, *source, sink, body, reads_values);
    }
    std::vector<ItemPtr> in;
    std::vector<ItemPtr> out;
    in.reserve(kQueueDepth);
    out.reserve(kQueueDepth);
    while (true) {
      in.clear();
      ABSL_RETURN_IF_ERROR(source->NextMany(in, kQueueDepth));
      if (in.empty()) {
        return absl::OkStatus();
      }
      if (reads_values) {
        PrimeBatch(in, &host);
      }
      for (const ItemPtr& item : in) {
        absl::Status ran = body(item, out);
        if (!ran.ok()) {
          if (!stage.tolerant) {
            sink.Many(out).IgnoreError();
            return ran;
          }
          // Emit completed values, account for this failure, and continue the
          // stream according to the stage's `try` semantics.
          ABSL_RETURN_IF_ERROR(sink.Many(out));
          ABSL_RETURN_IF_ERROR(tolerate(item, ran));
        }
      }
      ABSL_RETURN_IF_ERROR(sink.Many(out));
    }
  };

  if (name == "at") {
    const Value key =
        stage.indexed ? Value::Integer(stage.index) : Value::String(stage.text);
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
          ABSL_ASSIGN_OR_RETURN(Value selected, LookupChecked(value, key));
          out.push_back(Item::Of(std::move(selected)));
          return absl::OkStatus();
        });
  }
  if (name == "map") {
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
          ABSL_ASSIGN_OR_RETURN(Value mapped, EvaluateWith(stage.expr, value));
          out.push_back(Item::Of(std::move(mapped)));
          return absl::OkStatus();
        });
  }
  if (name == "take while" || name == "take until") {
    while (true) {
      ABSL_ASSIGN_OR_RETURN(ItemPtr item, source->Next());
      if (item == nullptr) {
        return absl::OkStatus();
      }
      const auto condition = [&]() -> absl::StatusOr<Value> {
        ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
        return EvaluateWith(stage.expr, value);
      }();
      if (!condition.ok()) {
        if (!stage.tolerant) {
          return condition.status();
        }
        ABSL_RETURN_IF_ERROR(tolerate(item, condition.status()));
        continue;
      }
      if (Truthy(*condition) == (name == "take until")) {
        source->Stop();
        return absl::OkStatus();
      }
      ABSL_RETURN_IF_ERROR(sink.One(std::move(item)));
    }
  }
  if (name == "where") {
    return per_value([&](const ItemPtr& item,
                         std::vector<ItemPtr>& out) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      ABSL_ASSIGN_OR_RETURN(const Value kept, EvaluateWith(stage.expr, value));
      if (Truthy(kept)) {
        out.push_back(item);
      }
      return absl::OkStatus();
    });
  }
  if (name == "log" || name == "logf") {
    // Shaped like `where`, not like `map`: the value is read so the log can say
    // something about it, and then emitted *as the item it was* bytes, mimetype
    // and all so dropping a log into a pipeline changes nothing about.
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          ABSL_RETURN_IF_ERROR(WriteLog(stage.log, item));
          out.push_back(item);
          return absl::OkStatus();
        });
  }
  if (name == "match") {
    const auto& patterns = runner_->prepared().patterns;
    const auto cached = patterns.find(stage.text);
    std::optional<pattern::Compiled> fallback;
    const pattern::Pattern* compiled;
    if (cached != patterns.end()) {
      compiled = &cached->second;
    } else {
      fallback = pattern::Compile(stage.text);
      if (!fallback->ok()) {
        return Fail(absl::StrCat("The pattern '", stage.text,
                                 "' cannot be read: ", fallback->error));
      }
      compiled = &fallback->pattern;
    }
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
          Value found = MatchCompiled(*compiled, AsText(value));
          // A value the pattern does not fit is dropped, which is what makes
          // this a combined `where` and `map` stage.
          if (!found.IsNull()) {
            out.push_back(Item::Of(std::move(found)));
          }
          return absl::OkStatus();
        });
  }
  if (name == "flatten") {
    // The inverse of `batch`: a list becomes its own values, and anything else
    // is one value however it is looked at, so a mixed stream is flattened
    // rather than refused.
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
          if (value.kind() != Value::Kind::kList) {
            out.push_back(item);
            return absl::OkStatus();
          }
          for (const Value& held : value.items()) {
            out.push_back(Item::Of(held));
          }
          return absl::OkStatus();
        });
  }
  if (name == "pace") {
    absl::Duration duration = stage.duration;
    if (stage.expr != graph::kNone) {
      ABSL_ASSIGN_OR_RETURN(const Value value, Evaluate(stage.expr));
      if (value.kind() != Value::Kind::kDuration) {
        return Fail("Pace requires a std::duration value.");
      }
      duration = value.duration();
    }
    if (duration <= absl::ZeroDuration()) {
      return Fail("Pace requires a positive duration.");
    }
    // Spaced out, not sampled: every value goes on, later than it arrived. The
    // producer is held back behind the queue rather than being asked to stop,
    // which is what makes this a rate limit and not a drop.
    absl::Time next = absl::InfinitePast();
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          const absl::Time now = absl::Now();
          if (now < next) {
            thread::SleepFor(next - now);
          }
          next = std::max(now, next) + duration;
          out.push_back(item);
          return absl::OkStatus();
        },
        /*reads_values=*/false);
  }
  if (name == "timeout") {
    absl::Duration duration = stage.duration;
    if (stage.expr != graph::kNone) {
      ABSL_ASSIGN_OR_RETURN(const Value value, Evaluate(stage.expr));
      if (value.kind() != Value::Kind::kDuration) {
        return Fail("A timeout requires a std::duration value.");
      }
      duration = value.duration();
    }
    if (duration <= absl::ZeroDuration()) {
      return Fail("A timeout must be positive.");
    }
    // The gap between values, not the total: a stream that keeps arriving runs
    // as long as it likes. Read one at a time, because a batch that waits for
    // the first value and then takes four more would be timing the batch.
    while (true) {
      absl::StatusOr<ItemPtr> item = source->NextUntil(absl::Now() + duration);
      if (!item.ok()) {
        if (!absl::IsDeadlineExceeded(item.status())) {
          return item.status();
        }
        return Fail(
            absl::StrCat("Nothing arrived on ", graph().refs[one.source].label,
                         " for ", absl::FormatDuration(duration), "."),
            absl::StatusCode::kDeadlineExceeded);
      }
      if (*item == nullptr) {
        return absl::OkStatus();
      }
      ABSL_RETURN_IF_ERROR(sink.One(*std::move(item)));
    }
  }
  if (name == "mime") {
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          if (Matches(item->Mimetype(), stage.text)) {
            out.push_back(item);
          }
          return absl::OkStatus();
        },
        /*reads_values=*/false);
  }
  if (name == "first") {
    if (stage.count <= 0) {
      return absl::OkStatus();
    }
    long long taken = 0;
    while (taken < stage.count) {
      ABSL_ASSIGN_OR_RETURN(const ItemPtr item, source->Next());
      if (item == nullptr) {
        return absl::OkStatus();
      }
      ABSL_RETURN_IF_ERROR(sink.One(item));
      ++taken;
    }
    source->Stop();
    return absl::OkStatus();
  }
  if (name == "last") {
    std::deque<ItemPtr> tail;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      tail.push_back(item);
      if (static_cast<long long>(tail.size()) > stage.count) {
        tail.pop_front();
      }
      return absl::OkStatus();
    }));
    for (const ItemPtr& item : tail) {
      ABSL_RETURN_IF_ERROR(sink.One(item));
    }
    return absl::OkStatus();
  }
  if (name == "drop") {
    long long seen = 0;
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          if (++seen > stage.count) {
            out.push_back(item);
          }
          return absl::OkStatus();
        },
        /*reads_values=*/false);
  }
  if (name == "destructure") {
    return per_value([&](const ItemPtr& item,
                         std::vector<ItemPtr>& out) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      if (value.kind() != Value::Kind::kList &&
          value.kind() != Value::Kind::kObject) {
        return Fail("Destructuring requires an object or a list.");
      }
      if (value.kind() == Value::Kind::kList &&
          value.items().size() < stage.destructured.size()) {
        return Fail(
            "The list has fewer values than the destructuring pattern.");
      }
      Value::Pairs fields;
      for (size_t index = 0; index < stage.destructured.size(); ++index) {
        const auto& name = stage.destructured[index];
        const auto* selected = value.kind() == Value::Kind::kList
                                   ? &value.items()[index]
                                   : value.Get(name);
        if (selected == nullptr) {
          return Fail(absl::StrCat("Missing destructured field '", name, "'."));
        }
        Value converted = *selected;
        if (index < stage.destructured_types.size() &&
            stage.destructured_types[index].has_value()) {
          ABSL_ASSIGN_OR_RETURN(
              converted,
              Coerce(converted, *stage.destructured_types[index],
                     CoerceContext{.bridge = &host, .shapes = &shapes()}));
        }
        fields.emplace_back(name, std::move(converted));
      }
      out.push_back(Item::Of(Value::Object(std::move(fields))));
      return absl::OkStatus();
    });
  }
  if (name == "coerce" && stage.type.has_value()) {
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          ABSL_ASSIGN_OR_RETURN(Value value, item->Read(&host));
          ABSL_ASSIGN_OR_RETURN(
              Value converted,
              Coerce(value, *stage.type,
                     CoerceContext{.bridge = &host, .shapes = &shapes()}));
          out.push_back(Item::Of(std::move(converted)));
          return absl::OkStatus();
        });
  }
  if (name == "truncate") {
    return per_value(
        [&](const ItemPtr& item, std::vector<ItemPtr>& out) -> absl::Status {
          ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
          out.push_back(Item::Of(Truncate(value, stage.count)));
          return absl::OkStatus();
        });
  }
  if (name == "batch") {
    std::vector<Value> group;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      group.push_back(value);
      if (static_cast<long long>(group.size()) < stage.count) {
        return absl::OkStatus();
      }
      ABSL_RETURN_IF_ERROR(sink.One(Item::Of(Value::List(std::move(group)))));
      group.clear();
      return absl::OkStatus();
    }));
    if (group.empty()) {
      return absl::OkStatus();
    }
    return sink.One(Item::Of(Value::List(std::move(group))));
  }
  if (name == "window") {
    // The overlapping counterpart of `batch`.
    if (stage.count <= 0) {
      return Fail(absl::StrCat("'window ", stage.count,
                               "' is not a width; a window holds at least one "
                               "value."));
    }
    const auto width = static_cast<size_t>(stage.count);
    // A deque of at most `width`, so the cost is the window and not the stream:
    // one over something endless is what this is for.
    std::deque<Value> held;
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      held.push_back(value);
      if (held.size() < width) {
        // Nothing yet: a window narrower than it was asked for is not a window,
        // and a stream shorter than one yields nothing at all. That is the
        // deliberate difference from `batch`, whose last list may be short.
        return absl::OkStatus();
      }
      if (held.size() > width) {
        held.pop_front();
      }
      return sink.One(
          Item::Of(Value::List(std::vector<Value>(held.begin(), held.end()))));
    });
  }
  if (name == "chunk") {
    // The other direction from `value`: one value, cut into pieces of a size
    // somebody downstream cares about.
    if (stage.count <= 0) {
      return Fail(absl::StrCat("'chunk ", stage.count,
                               "' is not a size; a chunk holds at least one "
                               "byte."));
    }
    const auto size = static_cast<size_t>(stage.count);
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      if (!value.IsTextlike()) {
        // Nothing to cut. A list is `batch`'s business and everything else is
        // one value however it is looked at, so it goes through unchanged
        // rather than being refused.
        return sink.One(Item::Of(value));
      }
      const std::string& text = value.text();
      const bool bytes = value.kind() == Value::Kind::kBytes;
      size_t at = 0;
      while (at < text.size()) {
        size_t take = std::min(size, text.size() - at);
        if (!bytes) {
          // Back off to the start of the character this would have split.
          while (take > 1 && at + take < text.size() &&
                 utf8::IsContinuation(text[at + take])) {
            --take;
          }
        }
        std::string piece = text.substr(at, take);
        ABSL_RETURN_IF_ERROR(
            sink.One(Item::Of(bytes ? Value::Bytes(std::move(piece))
                                    : Value::String(std::move(piece)))));
        at += take;
      }
      return absl::OkStatus();
    });
  }
  if (name == "then") {
    // All of this one, then all of that one. Two writers to a node interleave
    // by arrival, which is fine for pages and wrong for a conversation; this is
    // how a flow says which comes first.
    const auto forward = [&](const ItemPtr& item,
                             std::vector<ItemPtr>& out) -> absl::Status {
      out.push_back(item);
      return absl::OkStatus();
    };
    ABSL_RETURN_IF_ERROR(per_value(forward, /*reads_values=*/false));
    ABSL_ASSIGN_OR_RETURN(source, Subscribe(stage.stream));
    return per_value(forward, /*reads_values=*/false);
  }
  if (name == "group") {
    // `batch`, closed by a question rather than by a count: values pile up
    // until one of them says the group is finished, and the group goes on as a
    // list.
    std::vector<Value> gathered;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      gathered.push_back(value);
      ABSL_ASSIGN_OR_RETURN(const Value closes,
                            EvaluateWith(stage.expr, value));
      if (!Truthy(closes)) {
        return absl::OkStatus();
      }
      ABSL_RETURN_IF_ERROR(
          sink.One(Item::Of(Value::List(std::move(gathered)))));
      gathered.clear();
      return absl::OkStatus();
    }));
    if (gathered.empty()) {
      return absl::OkStatus();
    }
    return sink.One(Item::Of(Value::List(std::move(gathered))));
  }
  if (name == "collect") {
    std::vector<Value> collected;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      collected.push_back(value);
      return absl::OkStatus();
    }));
    return sink.One(Item::Of(Value::List(std::move(collected))));
  }
  if (name == "count") {
    std::int64_t total = 0;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr&) -> absl::Status {
      ++total;
      return absl::OkStatus();
    }));
    return sink.One(Item::Of(Value::Integer(total)));
  }
  if (name == "sum" || name == "avg") {
    // `sum` with no expression adds the values themselves; with one it adds
    // what the expression makes of each, which is what `| sum it.price` says.
    Value total = Value::Integer(0);
    std::int64_t seen = 0;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, StageValue(stage, item, host));
      ABSL_ASSIGN_OR_RETURN(total, Add(total, value));
      ++seen;
      return absl::OkStatus();
    }));
    if (name == "sum") {
      return sink.One(Item::Of(std::move(total)));
    }
    // The mean of nothing is not zero: an empty stream yields nothing, the same
    // way `min` of nothing does.
    if (seen == 0) {
      return absl::OkStatus();
    }
    const Value counted = Value::Double(static_cast<double>(seen));
    ABSL_ASSIGN_OR_RETURN(const Value mean, Divide(total, counted));
    return sink.One(Item::Of(mean));
  }
  if (name == "min" || name == "max") {
    // The smallest or largest of no values is not a value, so an empty stream
    // yields nothing rather than a zero somebody would have to know to ignore.
    const int wanted = name == "min" ? -1 : 1;
    std::optional<Value> best;
    ItemPtr keep;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, StageValue(stage, item, host));
      if (!best.has_value() || Order(value, *best) == wanted) {
        best = value;
        keep = item;
      }
      return absl::OkStatus();
    }));
    if (!best.has_value()) {
      return absl::OkStatus();
    }
    // The item as it arrived where the stage compared the values themselves, so
    // a pipe that only moves them still re-writes the producer's own bytes; the
    // computed value where an expression chose it.
    if (stage.expr == kNone && keep != nullptr) {
      return sink.One(keep);
    }
    return sink.One(Item::Of(*std::move(best)));
  }
  if (name == "fold") {
    // Two things bound rather than one: `it` is the value in hand and the name
    // the author chose is what the last pass produced.
    Value carried = Value::Of(stage.start);
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      ABSL_ASSIGN_OR_RETURN(carried, EvaluateFold(stage, carried, value));
      return absl::OkStatus();
    }));
    return sink.One(Item::Of(std::move(carried)));
  }
  if (name == "scan") {
    // `fold`, with the values published as they are computed rather than only
    // the last one.
    Value carried = Value::Of(stage.start);
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      ABSL_ASSIGN_OR_RETURN(carried, EvaluateFold(stage, carried, value));
      // Copied, not moved: the next value needs the state this one produced.
      return sink.One(Item::Of(carried));
    });
  }
  if (name == "sort") {
    // The whole stream, because which value comes first is not knowable until
    // the last one has arrived.
    std::vector<std::pair<Value, ItemPtr>> keyed;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value key, StageValue(stage, item, host));
      keyed.emplace_back(key, item);
      return absl::OkStatus();
    }));
    const bool descending = stage.descending;
    std::stable_sort(keyed.begin(), keyed.end(),
                     [descending](const std::pair<Value, ItemPtr>& left,
                                  const std::pair<Value, ItemPtr>& right) {
                       const int order = Order(left.first, right.first);
                       return descending ? order > 0 : order < 0;
                     });
    std::vector<ItemPtr> ordered;
    ordered.reserve(keyed.size());
    for (auto& [key, item] : keyed) {
      ordered.push_back(std::move(item));
    }
    return sink.Many(ordered);
  }
  if (name == "distinct") {
    absl::flat_hash_set<std::string> seen;
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      if (!seen.insert(AsText(value)).second) {
        return absl::OkStatus();
      }
      return sink.One(item);
    });
  }
  if (name == "join") {
    std::vector<std::string> pieces;
    ABSL_RETURN_IF_ERROR(each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      pieces.push_back(AsText(value));
      return absl::OkStatus();
    }));
    return sink.One(Item::Of(Value::String(absl::StrJoin(pieces, stage.text))));
  }
  if (name == "strformat") {
    // The one-value shorthand: `| strformat "took %s"` is exactly
    // `| map strformat("took %s", it)`, which is the shape almost every use of
    // it has. The full builtin is there when more than one value goes in.
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      const Value arguments[] = {value};
      return sink.One(Item::Of(Value::String([&] {
        const auto& formats = runner_->prepared().formats;
        const auto format = formats.find(stage.text);
        return format == formats.end()
                   ? Strformat(Value::String(stage.text), arguments)
                   : StrformatCompiled(format->second, arguments);
      }())));
    });
  }
  if (name == "text") {
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      return sink.One(Item::Of(Value::String(AsText(value))));
    });
  }
  if (name == "json") {
    return each([&](const ItemPtr& item) -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      return sink.One(Item::Of(AsJson(value)));
    });
  }
  if (name == "packb") {
    // An item read from a node keeps the producer's bytes, so a value that
    // arrived packed is passed on untouched, tag and all.
    return each([&](const ItemPtr& item) -> absl::Status {
      if (item->chunk().has_value() &&
          BaseMimetype(item->Mimetype()) == data::kMsgpackMimetype) {
        return sink.One(item);
      }
      ABSL_ASSIGN_OR_RETURN(const Value value, item->Read(&host));
      ABSL_ASSIGN_OR_RETURN(data::Chunk chunk,
                            host.ToChunk(value, data::kMsgpackMimetype));
      return sink.One(Item::OfChunk(std::move(chunk)));
    });
  }
  return Fail(absl::StrCat("Unknown stage '", name, "'."));
}

absl::Status Scope::InParallel(
    const graph::Stage& stage, Reader& source, const Sink& sink,
    absl::FunctionRef<absl::Status(const ItemPtr&, std::vector<ItemPtr>&)> body,
    bool reads_values) {
  HostBridge& host = bridge();
  Monitor& monitor = this->monitor();
  // One condition for the whole stage: the workers wait on each other, and
  // there are as many of them as the author asked for.
  Monitor::Condition turn(monitor);

  struct Shared {
    /// Which value is read next, and which is published next.
    std::int64_t reading = 0;
    std::int64_t publishing = 0;
    bool ended = false;
    absl::Status failure;
    /// Whether somebody is inside the reader right now: one cursor, one reader.
    bool taking = false;
  };

  Shared shared;
  const bool ordered = stage.ordered;
  const bool tolerant = stage.tolerant;

  const auto worker = [&]() -> absl::Status {
    std::vector<ItemPtr> out;
    while (true) {
      // A turn at the reader, and the number this value keeps.
      ItemPtr item;
      std::int64_t index = 0;
      {
        thread::MutexLock lock(&monitor.mu());
        ABSL_RETURN_IF_ERROR(monitor.Wait(
            turn, [&shared] { return !shared.taking || shared.ended; }));
        if (shared.ended || !shared.failure.ok()) {
          return absl::OkStatus();
        }
        shared.taking = true;
      }
      absl::StatusOr<ItemPtr> read = source.Next();
      {
        thread::MutexLock lock(&monitor.mu());
        shared.taking = false;
        if (!read.ok()) {
          if (shared.failure.ok()) {
            shared.failure = read.status();
          }
          shared.ended = true;
        } else if (*read == nullptr) {
          shared.ended = true;
        } else {
          item = *std::move(read);
          index = shared.reading++;
        }
      }
      turn.Wake();
      if (item == nullptr) {
        return absl::OkStatus();
      }

      out.clear();
      if (reads_values) {
        (void)item->Read(&host);
      }
      const absl::Status ran = body(item, out);

      // Publishing. Ordered means waiting for the values before this one, which
      // is what puts the stream back in the order it was read in.
      {
        thread::MutexLock lock(&monitor.mu());
        if (ordered) {
          ABSL_RETURN_IF_ERROR(monitor.Wait(turn, [&shared, index] {
            return shared.publishing == index || !shared.failure.ok();
          }));
        }
        if (!shared.failure.ok()) {
          return absl::OkStatus();
        }
      }
      absl::Status published;
      if (ran.ok()) {
        published = sink.Many(out);
      } else if (tolerant) {
        published = Tolerated(stage, item, ran, host);
      } else {
        published = ran;
      }
      {
        thread::MutexLock lock(&monitor.mu());
        if (ordered) {
          ++shared.publishing;
        }
        if (!published.ok() && shared.failure.ok()) {
          shared.failure = published;
          shared.ended = true;
        }
      }
      turn.Wake();
      if (!published.ok()) {
        return absl::OkStatus();
      }
    }
  };

  Group group(monitor, work_);
  for (int index = 1; index < stage.parallel; ++index) {
    group.Spawn([&worker]() -> absl::Status { return worker(); });
  }
  // This fiber is one of the workers, so `parallel 2` costs one extra fiber and
  // not two.
  absl::Status mine = worker();
  absl::Status joined = group.Join();
  {
    thread::MutexLock lock(&monitor.mu());
    if (!shared.failure.ok()) {
      return shared.failure;
    }
  }
  ABSL_RETURN_IF_ERROR(mine);
  return joined;
}

/// What a `try` stage does with a value it could not do: see `tolerate` in
/// [Scope::ProduceStage], which this is the parallel path's copy of.
absl::Status Scope::Tolerated(const graph::Stage& stage, const ItemPtr& item,
                              const absl::Status& why, HostBridge& host) {
  if (stage.failures != kNone) {
    ABSL_ASSIGN_OR_RETURN(Destination * destination,
                          DestinationOf(stage.failures));
    return destination->Write(Item::Of(StatusRecord(why)), &host);
  }
  actions::LogOptions options;
  options.level = "warning";
  return LogValue(
      Value::String(absl::StrCat("a value was skipped: ", why.message())),
      item.get(), options);
}

absl::StatusOr<Value> Scope::StageValue(const graph::Stage& stage,
                                        const ItemPtr& item, HostBridge& host) {
  ABSL_ASSIGN_OR_RETURN(Value value, item->Read(&host));
  if (stage.expr == kNone) {
    return value;
  }
  return EvaluateWith(stage.expr, value);
}

}  // namespace a11::flow::internal
