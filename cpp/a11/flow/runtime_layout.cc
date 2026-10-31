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

#include "a11/flow/internal/runtime_layout.h"

namespace a11::flow::internal {

absl::StatusOr<std::shared_ptr<const RuntimeLayouts>> BuildRuntimeLayouts(
    const CompiledProgram& program) {
  auto layouts = std::make_shared<RuntimeLayouts>();
  for (const auto& resolved : program.flows()) {
    if (resolved.plan.interface_only) {
      continue;
    }
    const auto& graph = resolved.graph;
    auto& flow = layouts->flows[graph.name];
    flow.step_slots.resize(graph.steps.size(), kNone);
    flow.step_captures.resize(graph.steps.size(), kNone);
    flow.joined_captures.resize(graph.refs.size(), kNone);
    flow.condition_captures.resize(graph.refs.size(), kNone);
    flow.fold_slots.resize(graph.refs.size(), kNone);
    absl::flat_hash_map<std::string, size_t> captures;
    const auto capture_id = [&](std::string name) {
      const auto [found, inserted] =
          captures.try_emplace(std::move(name), captures.size());
      return found->second;
    };
    flow.carry_capture = capture_id("carry");
    flow.result_capture = capture_id("$result");
    for (size_t id = 0; id < graph.refs.size(); ++id) {
      flow.joined_captures[id] = capture_id(absl::StrCat("merge:", id));
      flow.condition_captures[id] = capture_id(absl::StrCat("condition:", id));
    }
    absl::flat_hash_set<RefId> cursors;
    for (const auto& expression : graph.exprs) {
      cursors.insert(expression.refs.begin(), expression.refs.end());
    }
    for (const auto& ref : graph.refs) {
      if (ref.kind == RefKind::kBound && ref.source != kNone) {
        cursors.insert(ref.source);
      }
    }
    const auto* singles = program.SingleItems(graph.name);
    for (const auto& body : graph.bodies) {
      const size_t id = flow.bodies.size();
      auto layout = std::make_unique<RuntimeLayouts::Body>();
      layout->analysis = graph::Analyse(graph, id);
      for (size_t local = 0; local < body.steps.size(); ++local) {
        const auto step = body.steps[local];
        flow.step_slots[step] = local;
        const auto& one = graph.steps[step];
        if (one.kind == StepKind::kCall) {
          layout->calls.Add(step);
        }
        if (one.kind == StepKind::kCapture) {
          const auto capture = capture_id(one.slot);
          flow.step_captures[step] = capture;
          layout->capture_slots.try_emplace(capture,
                                            layout->capture_slots.size());
        }
        for (const auto& update : one.variable_updates) {
          layout->capture_slots.try_emplace(flow.joined_captures[update.result],
                                            layout->capture_slots.size());
        }
      }
      for (const auto ref : layout->analysis.refs) {
        const auto& one = graph.refs[ref];
        const auto readers = layout->analysis.readers.find(ref);
        if (readers == layout->analysis.readers.end() || readers->second <= 0) {
          continue;
        }
        if (one.kind == RefKind::kBound && one.role == "fold") {
          continue;
        }
        if (one.kind == RefKind::kBound || one.kind == RefKind::kStatus ||
            one.kind == RefKind::kWinner) {
          layout->lazies.Add(ref);
        } else if (singles == nullptr || !singles->refs[ref].eligible) {
          layout->buses.Add(ref);
          if (layout->analysis.materialise.find(ref) !=
              layout->analysis.materialise.end()) {
            layout->buffers.Add(ref);
          }
        }
      }
      for (const auto ref : layout->analysis.destinations) {
        if (graph.refs[ref].kind == RefKind::kSink) {
          layout->writers.Add(ref);
        } else {
          layout->destinations.Add(ref);
        }
      }
      for (const auto ref : cursors) {
        if (graph.refs[ref].owner == id) {
          layout->cursors.Add(ref);
        }
      }
      size_t folds = 0;
      for (const auto ref : layout->analysis.refs) {
        if (graph.refs[ref].kind == RefKind::kBound &&
            graph.refs[ref].role == "fold") {
          flow.fold_slots[ref] = folds++;
        }
      }
      ABSL_ASSIGN_OR_RETURN(
          layout->steps,
          layout->storage.Add<RuntimeLayouts::StepState>(body.steps.size()));
      ABSL_ASSIGN_OR_RETURN(layout->captures,
                            layout->storage.Add<std::optional<Value>>(
                                layout->capture_slots.size()));
      ABSL_ASSIGN_OR_RETURN(layout->folds,
                            layout->storage.Add<std::optional<Value>>(folds));
      ABSL_ASSIGN_OR_RETURN(
          layout->value_consumers,
          layout->storage.Add<int>(layout->cursors.index.size()));
      ABSL_RETURN_IF_ERROR(layout->calls.Finalize(layout->storage));
      ABSL_RETURN_IF_ERROR(layout->lazies.Finalize(layout->storage));
      ABSL_RETURN_IF_ERROR(layout->buses.Finalize(layout->storage));
      ABSL_RETURN_IF_ERROR(layout->buffers.Finalize(layout->storage));
      ABSL_RETURN_IF_ERROR(layout->destinations.Finalize(layout->storage));
      ABSL_RETURN_IF_ERROR(layout->writers.Finalize(layout->storage));
      ABSL_RETURN_IF_ERROR(layout->cursors.Finalize(layout->storage));
      layout->storage.Seal();
      flow.bodies.push_back(std::move(layout));
    }
  }
  return layouts;
}

}  // namespace a11::flow::internal
