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

namespace a11::flow {

using graph::kNone;
using graph::RefKind;
using graph::StepKind;
using internal::BuildRuntimeLayouts;
using internal::Fail;
using internal::MakeRuntimeHandler;

absl::StatusOr<std::shared_ptr<CompiledProgram>> CompiledProgram::Compile(
    std::string source, std::string source_name, const ModuleSources& modules,
    ModuleResolver resolver, CompilationLimits limits, bool single_item_slots) {
  CompilationBudget budget(limits);
  auto compiled = std::shared_ptr<CompiledProgram>(new CompiledProgram());
  compiled->source_ = std::move(source);
  compiled->source_name_ = std::move(source_name);
  LinkedModule linked =
      LinkModules(compiled->source_, compiled->source_name_, modules,
                  std::move(resolver), limits, &budget);
  compiled->linked_source_ = linked.source;
  compiled->members_ = linked.members;
  compiled->parsed_ = std::move(linked.parsed);
  compiled->resolved_ = Resolve(compiled->linked_source_, compiled->parsed_,
                                /*build_graph=*/true, nullptr, limits, &budget);
  linked.Locate(compiled->resolved_.diagnostics);
  compiled->resolved_.program.source_name = compiled->source_name_;
  if (const Diagnostic* first = compiled->resolved_.FirstError();
      first != nullptr) {
    const auto message =
        absl::StrCat(!first->source.empty()           ? first->source
                     : compiled->source_name_.empty() ? "<flow>"
                                                      : compiled->source_name_,
                     ":", first->range.start.line, ":",
                     first->range.start.column, ": ", first->message);
    return a11::MakeStatus(
        absl::StatusCode::kInvalidArgument, message,
        nlohmann::json::array(
            {{{"type", "flow.diagnostic/v1"},
              {"diagnostic", DiagnosticToJsonValue(*first)}}}));
  }
  compiled->single_item_slots_ = single_item_slots;
  size_t preparation_bytes = 0;
  const auto prepare_text = [&](std::string_view kind, std::string_view text) {
    if (text.size() > 4096 || preparation_bytes + text.size() > 65536 ||
        !budget.TryOptional(text.size() + 1)) {
      return;
    }
    if (kind == "match" && compiled->prepared_.patterns.find(text) ==
                               compiled->prepared_.patterns.end()) {
      auto pattern = pattern::Compile(text);
      if (pattern.ok()) {
        const auto cost =
            text.size() * 4 +
            pattern.pattern.pieces.capacity() *
                sizeof(pattern::Pattern::Piece) +
            pattern.pattern.holes.capacity() * sizeof(pattern::Hole);
        if (cost <= 65536 - preparation_bytes) {
          compiled->prepared_.patterns.emplace(std::string(text),
                                               std::move(pattern.pattern));
          preparation_bytes += cost;
        }
      }
    } else if (kind == "strformat" && compiled->prepared_.formats.find(text) ==
                                          compiled->prepared_.formats.end()) {
      auto format = CompileStrformat(text);
      const auto cost =
          text.size() * 4 + format.capacity() * sizeof(FormatPart);
      if (cost <= 65536 - preparation_bytes) {
        compiled->prepared_.formats.emplace(std::string(text),
                                            std::move(format));
        preparation_bytes += cost;
      }
    }
  };
  for (const auto& declaration : compiled->parsed_.flows) {
    syntax::VisitSubtree(*declaration, [&](const syntax::Node& node) {
      if (!budget.TryOptional()) {
        return;
      }
      if (const auto* builtin = syntax::As<syntax::Builtin>(&node);
          builtin != nullptr && !builtin->args.empty()) {
        if (const auto* literal =
                syntax::As<syntax::Literal>(builtin->args[0].get());
            literal != nullptr &&
            literal->value.kind == syntax::Constant::Kind::kString) {
          prepare_text(builtin->name, literal->value.text);
        }
      }
      if ((node.kind == syntax::NodeKind::kUnary ||
           node.kind == syntax::NodeKind::kBinary) &&
          node.extent.end - node.extent.start <= 4096 &&
          compiled->prepared_.folded.size() < 256 &&
          syntax::IsPureLiteralExpression(node)) {
        auto value = flow::Evaluate(node, {});
        if (value.ok() && value->kind() == Value::Kind::kBool) {
          compiled->prepared_.folded.emplace(&node, *std::move(value));
        }
      }
    });
  }
  for (const auto& flow : compiled->flows()) {
    for (const auto& ref : flow.graph.refs) {
      if (ref.kind == graph::RefKind::kDerived) {
        prepare_text(ref.stage.name, ref.stage.text);
        if (ref.stage.log.has_format) {
          prepare_text("strformat", ref.stage.log.format);
        }
      }
    }
    for (const auto& step : flow.graph.steps) {
      if (step.log.has_format) {
        prepare_text("strformat", step.log.format);
      }
    }
  }
  if (single_item_slots) {
    for (const auto& flow : compiled->flows()) {
      if (flow.plan.interface_only) {
        continue;
      }
      compiled->single_items_.emplace(flow.graph.name,
                                      AnalyseSingleItems(flow.graph, &budget));
    }
  }
  ABSL_ASSIGN_OR_RETURN(compiled->layouts_, BuildRuntimeLayouts(*compiled));
  return compiled;
}

const SingleItemAnalysis* CompiledProgram::SingleItems(
    std::string_view flow) const {
  const auto found = single_items_.find(flow);
  return found == single_items_.end() ? nullptr : &found->second;
}

nlohmann::json CompiledProgram::Optimizations() const {
  nlohmann::json flows = nlohmann::json::array();
  for (const auto& flow : this->flows()) {
    if (flow.plan.interface_only) {
      continue;
    }
    const auto* analysis = SingleItems(flow.graph.name);
    flows.push_back(SingleItemsToJson(
        flow.graph,
        analysis == nullptr ? AnalyseSingleItems(flow.graph) : *analysis,
        single_item_slots_));
  }
  return {{"format", "flow.optimizations/v1"},
          {"single_item_slots", single_item_slots_},
          {"flows", flows}};
}

std::vector<std::string> CompiledProgram::ExternalActions() const {
  std::vector<std::string> actions;
  absl::flat_hash_set<std::string> seen;
  for (const auto& flow : resolved_.flows) {
    if (flow.plan.interface_only) {
      continue;
    }
    for (const auto& step : flow.graph.steps) {
      if (step.kind != graph::StepKind::kCall) {
        continue;
      }
      const auto* implementation = Flow(step.action);
      if (implementation != nullptr && !implementation->plan.interface_only) {
        continue;
      }
      auto name = syntax::RuntimeActionName(step.action);
      if (seen.insert(name).second) {
        actions.push_back(std::move(name));
      }
    }
  }
  return actions;
}

const ResolvedFlow* absl_nullable CompiledProgram::Flow(
    std::string_view name) const {
  // As Program::Flow does: an empty name reaches nothing, so the entry flow is
  // not addressable by a `run` or a `call`.
  if (name.empty()) {
    return nullptr;
  }
  std::string target(name);
  for (const auto& member : members_) {
    if (!member.selection && !member.scoped && !member.type &&
        member.name == name) {
      target = member.target;
      break;
    }
  }
  for (const ResolvedFlow& flow : resolved_.flows) {
    if (!flow.plan.entry && flow.plan.name == target) {
      return &flow;
    }
  }
  return nullptr;
}

const ResolvedFlow* absl_nullable CompiledProgram::Entry() const {
  for (const ResolvedFlow& flow : resolved_.flows) {
    if (flow.plan.entry) {
      return &flow;
    }
  }
  return nullptr;
}

absl::StatusOr<actions::ActionHandler> MakeHandler(
    std::shared_ptr<const CompiledProgram> program, std::string_view flow,
    RunOptions options) {
  if (program == nullptr) {
    return Fail("There is no program to run.");
  }
  const ResolvedFlow* found = program->Flow(flow);
  if (found == nullptr) {
    std::vector<std::string> known;
    for (const ResolvedFlow& one : program->flows()) {
      known.push_back(one.plan.name);
    }
    std::sort(known.begin(), known.end());
    return Fail(
        absl::StrCat("No flow named '", flow, "' in ",
                     program->source_name().empty() ? "this program"
                                                    : program->source_name(),
                     " (declared: ",
                     known.empty() ? "none" : absl::StrJoin(known, ", "), ")."),
        absl::StatusCode::kNotFound);
  }
  return MakeRuntimeHandler(std::move(program), *found, std::move(options));
}

absl::StatusOr<actions::ActionHandler> MakeEntryHandler(
    std::shared_ptr<const CompiledProgram> program, RunOptions options) {
  if (program == nullptr) {
    return Fail("There is no program to run.");
  }
  if (program->Entry() == nullptr) {
    return Fail(
        absl::StrCat(
            program->source_name().empty() ? "This program"
                                           : program->source_name(),
            " declares no entry flow. A file that is meant to be run declares "
            "one as `flow { ... }` -- with no name, because an entry point is "
            "not something anything else calls."),
        absl::StatusCode::kNotFound);
  }
  const auto* entry = program->Entry();
  return MakeRuntimeHandler(std::move(program), *entry, std::move(options));
}

}  // namespace a11::flow
