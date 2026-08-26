// Copyright 2026 The A11 Authors.

#include "a11/flow/graph.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace a11::flow::graph {
namespace {

void Push(std::vector<RefId>& into, RefId ref) {
  if (ref != kNone) {
    into.push_back(ref);
  }
}

/// Whether a ref could ever be written by this flow, which is what makes it a
/// destination rather than something finished by being read to its end.
bool Writable(const FlowGraph& flow, RefId ref) {
  return ref != kNone && flow.refs[ref].writable;
}

}  // namespace

std::string_view StepKindName(StepKind kind) {
  switch (kind) {
    case StepKind::kCall:
      return "call";
    case StepKind::kPipe:
      return "pipe";
    case StepKind::kSkip:
      return "skip";
    case StepKind::kWait:
      return "wait";
    case StepKind::kDrain:
      return "drain";
    case StepKind::kCancel:
      return "cancel";
    case StepKind::kAbort:
      return "abort";
    case StepKind::kFail:
      return "fail";
    case StepKind::kLog:
      return "log";
    case StepKind::kCapture:
      return "capture";
    case StepKind::kForEach:
      return "for";
    case StepKind::kRepeat:
      return "repeat";
    case StepKind::kIf:
      return "if";
    case StepKind::kBlock:
      return "block";
  }
  return "pipe";
}

// --- What a ref reads --------------------------------------------------------

std::vector<RefId> FlowGraph::Upstreams(RefId ref) const {
  std::vector<RefId> found;
  if (ref == kNone) {
    return found;
  }
  const Ref& one = refs[ref];
  if (one.kind == RefKind::kDerived) {
    Push(found, one.source);
    // `then` takes a stream rather than a value, and the plan has to know it is
    // read so that whatever produces it is run and counted.
    Push(found, one.stage.stream);
  }
  // A zip reads every one of its sources, so each is a reader slot on whatever
  // produces it -- which is what makes the existing materialise-and-replay
  // analysis apply to one without knowing what a zip is.
  if (one.kind == RefKind::kZip || one.kind == RefKind::kMerge) {
    for (const RefId source : one.sources) {
      Push(found, source);
    }
  }
  return found;
}

std::vector<RefId> FlowGraph::ValueRefs(RefId ref) const {
  std::vector<RefId> found;
  if (ref == kNone) {
    return found;
  }
  const Ref& one = refs[ref];
  if (one.kind == RefKind::kExpr && one.expr != kNone) {
    for (const RefId read : exprs[one.expr].refs) {
      Push(found, read);
    }
  }
  if (one.kind == RefKind::kDerived && one.stage.expr != kNone) {
    for (const RefId read : exprs[one.stage.expr].refs) {
      Push(found, read);
    }
  }
  // A `| log`/`| logf` stage reads whatever it prints, the same way the
  // statement does: `x | logf "%s at %s" it, started` is a value read of
  // `started` per value going past, and one uncounted read of it is a stage
  // told the stream has no reader slot left.
  if (one.kind == RefKind::kDerived) {
    for (const ExprId expr : one.stage.log.arguments) {
      if (expr == kNone) {
        continue;
      }
      for (const RefId read : exprs[expr].refs) {
        Push(found, read);
      }
    }
  }
  return found;
}

// --- What a step reads and writes --------------------------------------------

std::vector<RefId> FlowGraph::Sources(StepId step) const {
  std::vector<RefId> found;
  const Step& one = steps[step];
  switch (one.kind) {
    case StepKind::kPipe:
    case StepKind::kCapture:
      Push(found, one.source);
      break;
    case StepKind::kSkip:
      // A counted `skip` is not a reader at all: the values are already gone,
      // taken where the stream is produced, so reading here would claim a slot
      // this step was never counted for.
      if (!one.count.has_value()) {
        Push(found, one.source);
      }
      break;
    case StepKind::kWait:
    case StepKind::kDrain:
      // A `wait first of` / `wait all of` reads one outcome per subject, and
      // each is a reader of whatever produces it: counted here, or the second
      // one is told the stream has more readers than the plan accounted for.
      // `outcome` *is* the first subject, so the two forms do not double-count.
      if (one.subjects.empty()) {
        Push(found, one.outcome);
      } else {
        for (const RefId subject : one.subjects) {
          Push(found, subject);
        }
      }
      break;
    case StepKind::kForEach:
      Push(found, one.source);
      break;
    default:
      break;
  }
  return found;
}

std::vector<RefId> FlowGraph::ValueSources(StepId step) const {
  std::vector<RefId> found;
  const Step& one = steps[step];
  auto add = [&](ExprId expr) {
    if (expr == kNone) {
      return;
    }
    for (const RefId read : exprs[expr].refs) {
      Push(found, read);
    }
  };
  for (const auto& [header, expr] : one.headers) {
    add(expr);
  }
  add(one.action_id);
  add(one.code);
  add(one.message);
  add(one.condition);
  // What a `log`/`logf` prints. Not `message`: a log keeps its arguments in its
  // own tail, so a `logf info "took %s" started` read nothing as far as the
  // analysis could see and the stream it named got no reader slot.
  for (const ExprId expr : one.log.arguments) {
    add(expr);
  }
  return found;
}

std::vector<RefId> FlowGraph::Destinations(StepId step) const {
  std::vector<RefId> found;
  const Step& one = steps[step];
  if (one.kind == StepKind::kPipe) {
    Push(found, one.destination);
  }
  return found;
}

std::vector<RefId> FlowGraph::StageDestinations(StepId step) const {
  std::vector<RefId> found;
  // Every ref the step reads, and everything those read, since a stage may be
  // several deep: `items | try map f into bad | where it.ok`. The chain is
  // finite and built bottom-up, so walking it needs no cycle guard.
  std::vector<RefId> pending = Sources(step);
  for (const RefId ref : ValueSources(step)) {
    pending.push_back(ref);
  }
  while (!pending.empty()) {
    const RefId ref = pending.back();
    pending.pop_back();
    if (ref == kNone || ref >= refs.size()) {
      continue;
    }
    if (refs[ref].kind == RefKind::kDerived) {
      Push(found, refs[ref].stage.failures);
    }
    for (const RefId up : Upstreams(ref)) {
      pending.push_back(up);
    }
  }
  return found;
}

std::vector<RefId> FlowGraph::Observed(StepId step) const {
  std::vector<RefId> found;
  const Step& one = steps[step];
  if (one.kind != StepKind::kWait && one.kind != StepKind::kDrain) {
    return found;
  }
  // A barrier on a node watches it without writing it: the node still has to be
  // ended, and this is the statement that says when.
  if (one.outcome == kNone) {
    return found;
  }
  const Ref& outcome = refs[one.outcome];
  if (outcome.kind == RefKind::kStatus) {
    Push(found, outcome.subject);
  }
  return found;
}

std::vector<BodyId> FlowGraph::NestedBodies(BodyId body) const {
  std::vector<BodyId> found;
  if (body == kNone) {
    return found;
  }
  for (const StepId step : bodies[body].steps) {
    for (const BodyId nested : steps[step].bodies) {
      found.push_back(nested);
      for (const BodyId deeper : NestedBodies(nested)) {
        found.push_back(deeper);
      }
    }
  }
  return found;
}

// --- The analysis ------------------------------------------------------------

namespace {

/// Every ref these bodies read, following derivations to their source.
absl::flat_hash_set<RefId> RefsUsedIn(const FlowGraph& flow,
                                      const std::vector<BodyId>& bodies) {
  absl::flat_hash_set<RefId> found;
  const std::function<void(RefId)> walk = [&](RefId ref) {
    if (ref == kNone || !found.insert(ref).second) {
      return;
    }
    for (const RefId up : flow.Upstreams(ref)) {
      walk(up);
    }
    for (const RefId value : flow.ValueRefs(ref)) {
      walk(value);
    }
  };
  for (const BodyId body : bodies) {
    for (const StepId step : flow.bodies[body].steps) {
      for (const RefId ref : flow.Sources(step)) {
        walk(ref);
      }
      for (const RefId ref : flow.ValueSources(step)) {
        walk(ref);
      }
    }
  }
  return found;
}

/// The nodes of its own that a body names, wherever it names them.
std::vector<RefId> NodeRefsIn(const FlowGraph& flow, BodyId body) {
  std::vector<RefId> found;
  absl::flat_hash_set<RefId> seen;
  absl::flat_hash_set<RefId> visited;
  const std::function<void(RefId)> walk = [&](RefId ref) {
    if (ref == kNone || !visited.insert(ref).second) {
      return;
    }
    const Ref& one = flow.refs[ref];
    if (one.kind == RefKind::kNode && one.owner == body) {
      if (seen.insert(ref).second) {
        found.push_back(ref);
      }
    }
    if (one.kind == RefKind::kNodeId) {
      walk(one.subject);
    }
    if (one.kind == RefKind::kStatus) {
      walk(one.subject);
    }
    for (const RefId up : flow.Upstreams(ref)) {
      walk(up);
    }
    for (const RefId value : flow.ValueRefs(ref)) {
      walk(value);
    }
  };
  for (const StepId step : flow.bodies[body].steps) {
    for (const RefId ref : flow.Sources(step)) {
      walk(ref);
    }
    for (const RefId ref : flow.ValueSources(step)) {
      walk(ref);
    }
    for (const RefId ref : flow.Destinations(step)) {
      walk(ref);
    }
    for (const RefId ref : flow.Observed(step)) {
      walk(ref);
    }
  }
  return found;
}

/// Every ref the given bodies write, once each.
std::vector<RefId> DestsWrittenIn(const FlowGraph& flow,
                                  const std::vector<BodyId>& bodies) {
  std::vector<RefId> found;
  absl::flat_hash_set<RefId> seen;
  for (const BodyId body : bodies) {
    for (const StepId step : flow.bodies[body].steps) {
      for (const RefId ref : flow.Destinations(step)) {
        if (seen.insert(ref).second) {
          found.push_back(ref);
        }
      }
    }
  }
  return found;
}

}  // namespace

Analysis Analyse(const FlowGraph& flow, BodyId body) {
  Analysis analysis;
  analysis.body = body;
  if (body == kNone) {
    return analysis;
  }

  const std::vector<BodyId> nested_bodies = flow.NestedBodies(body);
  const absl::flat_hash_set<RefId> nested = RefsUsedIn(flow, nested_bodies);

  // How many times this body itself reads each ref it owns, and which refs it
  // owns at all.
  absl::flat_hash_map<RefId, int> local;
  std::vector<RefId> owned;
  absl::flat_hash_set<RefId> is_owned;
  auto note = [&](RefId ref) {
    if (ref == kNone || flow.refs[ref].owner != body) {
      return;
    }
    if (is_owned.insert(ref).second) {
      owned.push_back(ref);
    }
    ++local[ref];
  };
  auto own = [&](RefId ref) {
    if (ref == kNone || flow.refs[ref].owner != body) {
      return;
    }
    if (is_owned.insert(ref).second) {
      owned.push_back(ref);
    }
  };

  // A *value* read of a ref does not get a reader of its own. Every value read
  // of one ref in one body shares a single cursor over it (see
  // `Scope::ValueOf`), because that is what makes two `let`s on one node two
  // different values rather than two names for the first: they take turns on
  // one view of the stream. So the slots are one per *stream* read plus at most
  // one for all the value reads together.
  //
  // Counting one each instead would allocate slots nobody takes, and an untaken
  // slot fills to `kQueueDepth` and then stops the producer for everybody: this
  // is the count that has to be exactly right or the flow hangs.
  absl::flat_hash_set<RefId> valued;
  auto value_read = [&](RefId ref) {
    if (ref == kNone) {
      return;
    }
    own(ref);
    if (valued.insert(ref).second) {
      note(ref);
    }
  };

  for (const StepId step : flow.bodies[body].steps) {
    for (const RefId ref : flow.Sources(step)) {
      note(ref);
    }
    for (const RefId ref : flow.ValueSources(step)) {
      value_read(ref);
    }
  }

  // A node's id is computed once per pass however many steps name the node, so
  // its readers are counted here rather than per mention.
  analysis.nodes = NodeRefsIn(flow, body);
  for (const RefId node : analysis.nodes) {
    const ExprId id_expr = flow.refs[node].id_expr;
    if (id_expr == kNone) {
      continue;
    }
    for (const RefId ref : flow.exprs[id_expr].refs) {
      note(ref);
    }
  }

  for (const RefId ref : nested) {
    own(ref);
  }

  // A derived or computed stream reads its own upstream, so the upstream needs
  // a reader for it. A ref is always built from refs created before it, so
  // taking the highest id still to do settles the whole chain -- including the
  // ones only reached through another derivation.
  absl::flat_hash_set<RefId> settled;
  while (true) {
    RefId next = kNone;
    for (const RefId ref : owned) {
      if (settled.contains(ref)) {
        continue;
      }
      if (next == kNone || ref > next) {
        next = ref;
      }
    }
    if (next == kNone) {
      break;
    }
    settled.insert(next);
    const auto found = local.find(next);
    const int reads = found == local.end() ? 0 : found->second;
    if (reads == 0 && !nested.contains(next)) {
      continue;
    }
    for (const RefId up : flow.Upstreams(next)) {
      note(up);
    }
    // A stage's own expression reads for a value, and shares the one cursor
    // with every other value read of the same ref.
    for (const RefId value : flow.ValueRefs(next)) {
      value_read(value);
    }
    // `owned` may have grown; the loop picks the new highest next time round.
  }

  std::vector<RefId> ordered = owned;
  std::sort(ordered.begin(), ordered.end());
  for (const RefId ref : ordered) {
    analysis.refs.push_back(ref);
    if (nested.contains(ref)) {
      // Read from inside a loop or a branch: buffered once here, replayed per
      // pass, so the buffer is the one reader of the underlying stream.
      analysis.materialise.insert(ref);
      analysis.readers[ref] = 1;
    } else {
      const auto found = local.find(ref);
      analysis.readers[ref] = found == local.end() ? 0 : found->second;
    }
  }

  absl::flat_hash_set<RefId> is_destination;
  auto count_writer = [&](RefId ref) {
    if (ref == kNone || flow.refs[ref].owner != body) {
      return;
    }
    if (is_destination.insert(ref).second) {
      analysis.destinations.push_back(ref);
    }
    ++analysis.writers[ref];
  };

  for (const StepId step : flow.bodies[body].steps) {
    std::vector<RefId> held;
    for (const RefId ref : flow.Destinations(step)) {
      if (flow.refs[ref].owner == body) {
        held.push_back(ref);
      }
      count_writer(ref);
    }
    // A `try ... into failures` writes from inside a stream this step reads, so
    // the stream's failures hold that node open exactly as the step's own
    // destination does.
    for (const RefId ref : flow.StageDestinations(step)) {
      if (flow.refs[ref].owner == body) {
        held.push_back(ref);
      }
      count_writer(ref);
    }
    for (const RefId ref : flow.Observed(step)) {
      // Only a ref this flow could write becomes a destination; a readable one
      // is finished by being read to its end instead.
      if (flow.refs[ref].owner == body && Writable(flow, ref)) {
        if (is_destination.insert(ref).second) {
          analysis.destinations.push_back(ref);
        }
      }
    }
    const std::vector<BodyId>& inner = flow.steps[step].bodies;
    if (!inner.empty()) {
      std::vector<BodyId> deep;
      for (const BodyId one : inner) {
        deep.push_back(one);
        for (const BodyId deeper : flow.NestedBodies(one)) {
          deep.push_back(deeper);
        }
      }
      // A loop or a branch writing an outer node counts as one writer of it for
      // as long as it runs, however many of its passes write.
      for (const RefId ref : DestsWrittenIn(flow, deep)) {
        if (flow.refs[ref].owner == body) {
          held.push_back(ref);
          count_writer(ref);
        }
      }
    }
    analysis.held[step] = std::move(held);
  }
  return analysis;
}

}  // namespace a11::flow::graph
