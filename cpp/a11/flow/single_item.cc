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

#include "a11/flow/single_item.h"

#include <algorithm>
#include <utility>

#include <absl/container/flat_hash_set.h>

namespace a11::flow {
namespace {
using namespace graph;

class Inference {
 public:
  Inference(const FlowGraph& flow, CompilationBudget* budget, size_t max_work)
      : flow_(flow), budget_(budget), remaining_(max_work) {
    result_.refs.resize(flow.refs.size());
  }

  SingleItemAnalysis Run() {
    if (flow_.refs.size() + flow_.steps.size() + flow_.exprs.size() +
            flow_.bodies.size() >
        remaining_ / 4) {
      result_.complete = false;
      for (auto& fact : result_.refs) {
        fact.reason = "analysis-budget";
      }
      return std::move(result_);
    }
    if (!Tick(flow_.refs.size() + flow_.steps.size() + flow_.exprs.size() +
              flow_.bodies.size())) {
      for (auto& fact : result_.refs) {
        fact.reason = "analysis-budget";
      }
      return std::move(result_);
    }
    if (!CheckSingleItemStructure(flow_)) {
      result_.complete = false;
      return std::move(result_);
    }
    bool sensitive = false;
    for (const auto& step : flow_.steps) {
      if (!Tick()) {
        break;
      }
      sensitive |= step.parallel > 1 || step.parallel_expression != kNone ||
                   step.race || step.deadline != kNone ||
                   step.timeout_value != kNone || step.timeout.has_value();
    }
    for (const auto& ref : flow_.refs) {
      if (!Tick()) {
        break;
      }
      sensitive |=
          ref.kind == RefKind::kMerge ||
          (ref.kind == RefKind::kDerived &&
           (ref.stage.name == "timeout" || ref.stage.name == "pace" ||
            ref.stage.parallel > 1 || ref.stage.parallel_expression != kNone ||
            !ref.stage.ordered));
    }
    for (const auto& expr : flow_.exprs) {
      if (!Tick()) {
        break;
      }
      if (expr.node != nullptr) {
        syntax::VisitSubtree(*expr.node, [&](const syntax::Node& node) {
          if (!Tick()) {
            return;
          }
          const auto* builtin = syntax::As<syntax::Builtin>(&node);
          sensitive |= builtin != nullptr && builtin->name == "now";
        });
      }
    }
    for (RefId id = 0; id < flow_.refs.size() && Tick(); ++id) {
      auto& fact = result_.refs[id];
      const auto& ref = flow_.refs[id];
      if (ref.kind == RefKind::kNode) {
        fact.reason = NodeBoundary(id);
        if (!fact.reason.empty()) {
          continue;
        }
      }
      Infer(id);
    }
    for (RefId id = 0; id < flow_.refs.size() && Tick(); ++id) {
      auto& fact = result_.refs[id];
      const auto& ref = flow_.refs[id];
      fact.eligible =
          fact.upper.has_value() && *fact.upper <= 1 &&
          (ref.kind == RefKind::kExpr || ref.kind == RefKind::kDerived ||
           (ref.kind == RefKind::kNode && NodeBoundary(id).empty()));
      if (!fact.eligible) {
        continue;
      }
      fact.reason = "bounded-producer";
      if (ref.kind == RefKind::kExpr) {
        if (sensitive) {
          fact.pump_reason = "timing-or-race-sensitive-flow";
        } else {
          fact.pump_free = fact.literal || ExclusiveCapture(id);
          fact.pump_reason = fact.literal     ? "literal-on-demand"
                             : fact.pump_free ? "exclusive-initializer-capture"
                                              : "shared-or-unverified-consumer";
        }
      }
      if (fact.pump_free && fact.literal) {
        size_t uses = 0;
        bool serial = false;
        for (const auto& step : flow_.steps) {
          if (!Tick()) {
            break;
          }
          const auto step_id = static_cast<StepId>(&step - flow_.steps.data());
          for (const auto source : flow_.Sources(step_id)) {
            uses += source == id;
          }
          for (const auto source : flow_.ValueSources(step_id)) {
            uses += source == id;
          }
          serial |= step.kind == StepKind::kPipe &&
                    TransferSources(step).size() > 1 &&
                    step.body == ref.owner &&
                    std::find(TransferSources(step).begin(),
                              TransferSources(step).end(),
                              id) != TransferSources(step).end();
        }
        for (RefId other = 0; other < flow_.refs.size() && Tick(); ++other) {
          for (const auto up : flow_.Upstreams(other)) {
            uses += up == id;
          }
          for (const auto up : flow_.ValueRefs(other)) {
            uses += up == id;
          }
          uses += flow_.refs[other].subject == id;
        }
        for (const auto& expression : flow_.exprs) {
          if (!Tick()) {
            break;
          }
          for (const auto source : expression.refs) {
            uses += source == id;
          }
        }
        fact.direct_transfer = serial && uses == 1;
      }
      if (ref.kind == RefKind::kNode && !VerifyWriters(id)) {
        fact.eligible = false;
        fact.reason = "writer-proof-rejected";
      }
    }
    if (!result_.complete) {
      for (auto& fact : result_.refs) {
        fact.eligible = false;
        fact.pump_free = false;
        fact.direct_transfer = false;
        fact.reason = "analysis-budget";
      }
    }
    return std::move(result_);
  }

 private:
  bool Tick(size_t work = 1) {
    if (remaining_ < work ||
        (budget_ != nullptr && !budget_->TryOptional(work))) {
      result_.complete = false;
      return false;
    }
    remaining_ -= work;
    return true;
  }

  std::string NodeBoundary(RefId id) {
    const auto& node = flow_.refs[id];
    if (node.node_map.empty()) {
      return "observable-node-map";
    }
    if (node.id_expr != kNone) {
      return "external-node-boundary";
    }
    for (const auto& ref : flow_.refs) {
      if (!Tick()) {
        return "analysis-budget";
      }
      if (ref.kind == RefKind::kNode && ref.id_expr != kNone &&
          ref.node_map == node.node_map) {
        return "possible-external-writer";
      }
      if (ref.kind == RefKind::kNodeId && ref.subject == id) {
        return "escaping-node-identity";
      }
      if (ref.kind == RefKind::kDerived && ref.stage.failures == id) {
        return "failure-sink";
      }
    }
    for (const auto& step : flow_.steps) {
      if (!Tick()) {
        return "analysis-budget";
      }
      if (step.kind == StepKind::kCall && step.node_map == node.node_map) {
        return "possible-external-writer";
      }
    }
    return {};
  }

  std::optional<int> Infer(RefId id) {
    if (id == kNone || !Tick()) {
      return std::nullopt;
    }
    auto& fact = result_.refs[id];
    if (done_.find(id) != done_.end()) {
      return fact.upper;
    }
    if (active_.size() >= 128) {
      return std::nullopt;
    }
    if (!active_.insert(id).second) {
      return std::nullopt;
    }
    const auto& ref = flow_.refs[id];
    switch (ref.kind) {
      case RefKind::kExpr:
        fact.upper = 1;
        fact.exactly_one_on_success = true;
        {
          const auto& expression = flow_.exprs[ref.expr];
          fact.literal =
              expression.activation == kNone && expression.refs.empty() &&
              expression.node != nullptr &&
              (syntax::As<syntax::Literal>(expression.node) != nullptr ||
               (expression.node->extent.end - expression.node->extent.start <=
                    4096 &&
                syntax::ConstantValue(expression.node).has_value()));
        }
        break;
      case RefKind::kBound:
        if (ref.role != "fold") {
          fact.upper = 1;
        }
        break;
      case RefKind::kHeader:
      case RefKind::kNodeId:
      case RefKind::kStatus:
      case RefKind::kWinner:
        fact.upper = 1;
        break;
      case RefKind::kDerived: {
        const auto& stage = ref.stage;
        if (stage.name == "consume" ||
            vocabulary::ReducingStages().find(stage.name) !=
                vocabulary::ReducingStages().end()) {
          fact.upper = 1;
        } else if (stage.name == "first" && stage.count >= 0 &&
                   stage.count <= 1) {
          fact.upper = static_cast<int>(stage.count);
        } else if (GraphBuilder::StagePreservesCount(stage)) {
          fact.upper = Infer(ref.source);
        }
        break;
      }
      case RefKind::kNode:
        if (NodeBoundary(id).empty()) {
          fact.upper = BodyBound(ref.owner, id);
          for (StepId step = 0; step < flow_.steps.size() && Tick(); ++step) {
            const auto written = flow_.Destinations(step);
            if (flow_.steps[step].kind == StepKind::kPipe &&
                std::find(written.begin(), written.end(), id) !=
                    written.end()) {
              fact.writers.push_back(step);
            }
          }
        }
        break;
      default:
        fact.reason = "external-or-unsupported-producer";
        break;
    }
    if (ref.skip > 0) {
      fact.exactly_one_on_success = false;
    }
    active_.erase(id);
    done_.insert(id);
    if (fact.upper.has_value() && *fact.upper > 1) {
      fact.reason = "multiple-items";
      fact.upper.reset();
    }
    return fact.upper;
  }

  std::optional<int> BodyBound(BodyId body, RefId target) {
    int total = 0;
    for (const auto id : flow_.bodies[body].steps) {
      if (!Tick()) {
        return std::nullopt;
      }
      const auto& step = flow_.steps[id];
      std::optional<int> bound = 0;
      if (step.kind == StepKind::kPipe && [&] {
            const auto writes = flow_.Destinations(id);
            return std::find(writes.begin(), writes.end(), target) !=
                   writes.end();
          }()) {
        bound = Infer(step.source);
        if (graph::TransferSources(step).size() > 1) {
          int sum = 0;
          for (const auto source : graph::TransferSources(step)) {
            const auto amount = Infer(source);
            if (!amount.has_value()) {
              bound.reset();
              break;
            }
            sum = std::min(2, sum + *amount);
            bound = sum;
          }
        }
      }
      if (!step.bodies.empty()) {
        int nested = 0;
        for (const auto child : step.bodies) {
          const auto amount = BodyBound(child, target);
          if (!amount.has_value()) {
            return std::nullopt;
          }
          nested = step.kind == StepKind::kIf ? std::max(nested, *amount)
                                              : std::min(2, nested + *amount);
        }
        if ((step.kind == StepKind::kForEach ||
             step.kind == StepKind::kRepeat) &&
            nested > 0) {
          return std::nullopt;
        }
        if (!bound.has_value()) {
          return std::nullopt;
        }
        bound = std::min(2, *bound + nested);
      }
      if (!bound.has_value()) {
        return std::nullopt;
      }
      total = std::min(2, total + *bound);
    }
    return total;
  }

  bool VerifyWriters(RefId id) {
    const auto& writers = result_.refs[id].writers;
    std::vector<std::vector<std::pair<StepId, BodyId>>> paths;
    for (const auto writer : writers) {
      if (!Tick()) {
        return false;
      }
      const auto source = flow_.steps[writer].source;
      if (!result_.refs[source].upper.has_value() ||
          *result_.refs[source].upper > 1) {
        return false;
      }
      auto body = flow_.steps[writer].body;
      std::vector<std::pair<StepId, BodyId>> alternatives;
      while (body != flow_.refs[id].owner && Tick()) {
        const auto parent = flow_.bodies[body].owner_step;
        if (parent == kNone || flow_.steps[parent].kind == StepKind::kForEach ||
            flow_.steps[parent].kind == StepKind::kRepeat) {
          return false;
        }
        if (flow_.steps[parent].kind == StepKind::kIf) {
          alternatives.emplace_back(parent, body);
        }
        body = flow_.bodies[body].parent;
        if (body == kNone) {
          return false;
        }
      }
      if (*result_.refs[source].upper == 0) {
        continue;
      }
      for (const auto& previous : paths) {
        bool exclusive = false;
        for (const auto& [choice, child] : alternatives) {
          for (const auto& [other_choice, other_child] : previous) {
            if (!Tick()) {
              return false;
            }
            exclusive |= choice == other_choice && child != other_child;
          }
        }
        if (!exclusive) {
          return false;
        }
      }
      paths.push_back(std::move(alternatives));
    }
    return true;
  }

  bool ExclusiveCapture(RefId id) {
    size_t uses = 0;
    bool capture = false;
    for (const auto& step : flow_.steps) {
      if (!Tick()) {
        return false;
      }
      if (step.kind == StepKind::kPipe && TransferSources(step).size() > 1) {
        const auto sources = TransferSources(step);
        uses +=
            static_cast<size_t>(std::count(sources.begin(), sources.end(), id));
      }
      if (step.source == id) {
        ++uses;
        capture = step.kind == StepKind::kCapture &&
                  step.slot.starts_with("initialize:") &&
                  step.body == flow_.refs[id].owner &&
                  step.phase_gate == flow_.refs[id].phase_gate;
      }
    }
    for (RefId ref = 0; ref < flow_.refs.size(); ++ref) {
      if (!Tick()) {
        return false;
      }
      for (const auto up : flow_.Upstreams(ref)) {
        uses += up == id;
      }
      uses += flow_.refs[ref].subject == id;
      // Initialized bound refs retain their source as provenance, not a reader.
      for (const auto up : flow_.ValueRefs(ref)) {
        uses += up == id;
      }
    }
    for (const auto& expr : flow_.exprs) {
      for (const auto ref : expr.refs) {
        if (!Tick()) {
          return false;
        }
        uses += ref == id;
      }
    }
    return capture && uses == 1;
  }

  const FlowGraph& flow_;
  CompilationBudget* budget_;
  size_t remaining_;
  SingleItemAnalysis result_;
  absl::flat_hash_set<RefId> active_;
  absl::flat_hash_set<RefId> done_;
};
}  // namespace

bool CheckSingleItemStructure(const graph::FlowGraph& flow) {
  const auto ref_ok = [&](const auto id) {
    return id == graph::kNone || id < flow.refs.size();
  };
  const auto step_ok = [&](const auto id) {
    return id == graph::kNone || id < flow.steps.size();
  };
  std::vector<bool> seen(flow.steps.size());
  std::vector<size_t> depths(flow.bodies.size(), 0);
  for (size_t body = 0; body < flow.bodies.size(); ++body) {
    const auto& scope = flow.bodies[body];
    if ((scope.parent != graph::kNone && scope.parent >= body) ||
        !step_ok(scope.owner_step)) {
      return false;
    }
    if (scope.owner_step != graph::kNone) {
      const auto& owner = flow.steps[scope.owner_step];
      if (owner.body != scope.parent ||
          std::find(owner.bodies.begin(), owner.bodies.end(), body) ==
              owner.bodies.end()) {
        return false;
      }
    }
    if (scope.parent != graph::kNone) {
      depths[body] = depths[scope.parent] + 1;
      if (depths[body] > 128) {
        return false;
      }
    }
    for (const auto step : scope.steps) {
      if (step >= flow.steps.size() || seen[step] ||
          flow.steps[step].body != body) {
        return false;
      }
      seen[step] = true;
    }
  }
  for (const auto used : seen) {
    if (!used) {
      return false;
    }
  }
  for (const auto& step : flow.steps) {
    if (!ref_ok(step.source) || !ref_ok(step.destination) ||
        !ref_ok(step.item) || !step_ok(step.phase_gate) ||
        (step.parallel_expression != graph::kNone &&
         step.parallel_expression >= flow.exprs.size())) {
      return false;
    }
    if (!ref_ok(step.sink)) {
      return false;
    }
    for (const auto source : graph::TransferSources(step)) {
      if (!ref_ok(source)) {
        return false;
      }
    }
    for (const auto target : graph::TransferTargets(step)) {
      if (!ref_ok(target)) {
        return false;
      }
    }
    if (step.transfer != nullptr) {
      for (const auto& join : step.transfer->joins) {
        if (!ref_ok(join.source) ||
            (join.expression ? join.target >= flow.exprs.size()
                             : !step_ok(join.target))) {
          return false;
        }
      }
      for (const auto call : step.transfer->destination_calls) {
        if (!step_ok(call)) {
          return false;
        }
      }
    }
    for (const auto body : step.bodies) {
      if (body >= flow.bodies.size() || flow.bodies[body].parent != step.body ||
          flow.bodies[body].owner_step == graph::kNone ||
          &flow.steps[flow.bodies[body].owner_step] != &step) {
        return false;
      }
    }
  }
  for (const auto& ref : flow.refs) {
    if (ref.kind == graph::RefKind::kSink &&
        (ref.subject_step >= flow.steps.size() || !ref_ok(ref.subject) ||
         (ref.subject != graph::kNone &&
          (ref.subject >= static_cast<size_t>(&ref - flow.refs.data()) ||
           flow.refs[ref.subject].kind != graph::RefKind::kSink)))) {
      return false;
    }

    if ((ref.kind == graph::RefKind::kDerived &&
         (ref.source == graph::kNone ||
          (ref.stage.expr != graph::kNone &&
           ref.stage.expr >= flow.exprs.size()) ||
          (ref.stage.parallel_expression != graph::kNone &&
           ref.stage.parallel_expression >= flow.exprs.size()) ||
          !ref_ok(ref.stage.failures) || !ref_ok(ref.stage.stream))) ||
        (ref.kind == graph::RefKind::kNode && ref.id_expr != graph::kNone &&
         ref.id_expr >= flow.exprs.size()) ||
        (ref.kind == graph::RefKind::kCallPort &&
         ref.call >= flow.steps.size()) ||
        !step_ok(ref.bound_by)) {
      return false;
    }
    if (ref.owner >= flow.bodies.size() || !ref_ok(ref.source) ||
        !ref_ok(ref.subject) || !step_ok(ref.phase_gate) ||
        (ref.kind == graph::RefKind::kExpr && ref.expr >= flow.exprs.size())) {
      return false;
    }
    for (const auto source : ref.sources) {
      if (!ref_ok(source)) {
        return false;
      }
    }
  }
  for (const auto& expr : flow.exprs) {
    if (expr.activation != graph::kNone &&
        expr.activation >= flow.bodies.size()) {
      return false;
    }
    for (const auto ref : expr.refs) {
      if (!ref_ok(ref)) {
        return false;
      }
    }
  }
  return flow.root < flow.bodies.size();
}

SingleItemAnalysis AnalyseSingleItems(const graph::FlowGraph& flow,
                                      CompilationBudget* budget,
                                      size_t max_work) {
  return Inference(flow, budget, max_work).Run();
}

nlohmann::json SingleItemsToJson(const graph::FlowGraph& flow,
                                 const SingleItemAnalysis& analysis,
                                 bool enabled) {
  nlohmann::json decisions = nlohmann::json::array();
  for (size_t id = 0; id < analysis.refs.size(); ++id) {
    const auto& fact = analysis.refs[id];
    decisions.push_back(
        {{"ref", id},
         {"label", flow.refs[id].label},
         {"owner", flow.refs[id].owner},
         {"declared_unary", flow.refs[id].unary},
         {"expression", flow.refs[id].expr == graph::kNone
                            ? nlohmann::json(nullptr)
                            : nlohmann::json(flow.refs[id].expr)},
         {"source_ref", flow.refs[id].source == graph::kNone
                            ? nlohmann::json(nullptr)
                            : nlohmann::json(flow.refs[id].source)},
         {"upper", fact.upper.has_value() ? nlohmann::json(*fact.upper)
                                          : nlohmann::json(nullptr)},
         {"exactly_one_on_success", fact.exactly_one_on_success},
         {"known_literal", fact.literal},
         {"eligible", fact.eligible},
         {"status",
          fact.eligible ? (enabled ? "applied" : "eligible") : "rejected"},
         {"reason", fact.reason},
         {"writers", fact.writers},
         {"pump_free_eligible", fact.pump_free},
         {"direct_transfer_eligible", fact.direct_transfer},
         {"transport", enabled && fact.direct_transfer ? "direct-literal"
                       : enabled && fact.eligible      ? "slot"
                                                       : "baseline"},
         {"pump_reason", fact.pump_reason},
         {"producer_pump", fact.eligible
                               ? nlohmann::json(!(enabled && fact.pump_free))
                               : nlohmann::json(nullptr)}});
  }
  return {{"flow", flow.name},
          {"complete", analysis.complete},
          {"refs", decisions}};
}
}  // namespace a11::flow
