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

#include "a11/flow/module.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

#include <absl/container/flat_hash_set.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_replace.h>

#include "a11/flow/resolve.h"
#include "a11/flow/vocabulary.h"
#include "a11/utf8.h"

namespace a11::flow {
namespace {
using syntax::Node;
using syntax::NodeKind;

enum class MemberKind { kModule, kFlow, kType, kIntrinsic };

struct Binding {
  size_t unit = static_cast<size_t>(-1);
  std::string member;
  MemberKind kind = MemberKind::kModule;
  bool exported = false;
  syntax::Location location;
  bool implemented = false;
};

struct Unit {
  ModuleSource document;
  ParseResult parsed;
  absl::flat_hash_map<std::string, Binding> members;
  absl::flat_hash_map<std::string, std::string> parents;
  absl::flat_hash_map<size_t, size_t> dependencies;
  absl::flat_hash_map<std::string, std::vector<Binding>> contracts;
  size_t depth = 0;
  size_t start = 0;
};

constexpr size_t kBuiltin = static_cast<size_t>(-1);

std::string Parent(std::string_view path) {
  const auto at = path.rfind("::");
  return at == std::string_view::npos ? "" : std::string(path.substr(0, at));
}

std::string Leaf(std::string_view path) {
  const auto at = path.rfind("::");
  return std::string(path.substr(at == std::string_view::npos ? 0 : at + 2));
}

std::string Join(std::string_view scope, std::string_view name) {
  return scope.empty() ? std::string(name) : absl::StrCat(scope, "::", name);
}

std::string Prefix(std::string_view identity) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string result = "$module_";
  for (const char raw : identity) {
    const auto byte = static_cast<unsigned char>(raw);
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result + "::";
}

void Problem(Unit& unit, std::string_view code, std::string message,
             const syntax::Location& location) {
  Diagnostic diagnostic;
  diagnostic.code = std::string(code);
  diagnostic.message = std::move(message);
  diagnostic.source = unit.document.identity;
  const auto* info = FindCode(code);
  diagnostic.family = info == nullptr ? Family::kForm : info->family;
  diagnostic.severity = Severity::kError;
  diagnostic.range =
      LineIndex(unit.document.source).Between(location.start, location.end);
  unit.parsed.diagnostics.push_back(std::move(diagnostic));
}

std::optional<Binding> Standard(std::string_view name) {
  if (!name.starts_with("std::")) {
    return std::nullopt;
  }
  const auto member = name.substr(5);
  if (vocabulary::TypeNames().contains(member) ||
      vocabulary::Builtins().contains(member) || member == "status" ||
      member == "option" || member == "stream" || member == "null" ||
      member == "zip" || member == "interleave") {
    return Binding{
        kBuiltin, std::string(name), MemberKind::kIntrinsic, true, {}};
  }
  return std::nullopt;
}

std::optional<Binding> Lookup(const std::vector<std::unique_ptr<Unit>>& units,
                              size_t owner, std::string scope,
                              std::string_view path) {
  if (path.starts_with("::")) {
    path.remove_prefix(2);
    scope.clear();
  }
  const auto lexical_parent = units[owner]->parents.find(scope);
  const std::string access_scope = lexical_parent == units[owner]->parents.end()
                                       ? scope
                                       : lexical_parent->second;
  const auto split = path.find("::");
  const std::string first(path.substr(0, split));
  std::optional<Binding> result;
  if (first == "std") {
    result = Binding{kBuiltin, "std", MemberKind::kModule, true, {}};
  } else {
    while (true) {
      const auto found = units[owner]->members.find(Join(scope, first));
      if (found != units[owner]->members.end()) {
        result = found->second;
        break;
      }
      if (scope.empty()) {
        break;
      }
      const auto parent = units[owner]->parents.find(scope);
      scope = parent == units[owner]->parents.end() ? Parent(scope)
                                                    : parent->second;
    }
    if (!result.has_value() && vocabulary::IsPrimitiveType(first)) {
      result = Standard(absl::StrCat("std::", first));
    }
  }
  if (!result.has_value() || split == std::string_view::npos) {
    return result;
  }
  size_t at = split + 2;
  while (at <= path.size()) {
    if (result->kind != MemberKind::kModule) {
      return std::nullopt;
    }
    const auto end = path.find("::", at);
    const auto component = path.substr(
        at, end == std::string_view::npos ? path.size() - at : end - at);
    if (result->unit == kBuiltin) {
      result = Standard(Join(result->member, component));
    } else {
      const auto& target = *units[result->unit];
      const auto found = target.members.find(Join(result->member, component));
      if (found == target.members.end() ||
          (!found->second.exported &&
           (result->unit != owner ||
            (!Parent(found->first).empty() &&
             access_scope != Parent(found->first) &&
             !access_scope.starts_with(Parent(found->first) + "::"))))) {
        return std::nullopt;
      }
      result = found->second;
    }
    if (!result.has_value() || end == std::string_view::npos) {
      return result;
    }
    at = end + 2;
  }
  return result;
}

std::string Target(const std::vector<std::unique_ptr<Unit>>& units,
                   const Binding& binding) {
  if (binding.unit == kBuiltin) {
    return binding.member.substr(5);
  }
  if (binding.kind == MemberKind::kFlow) {
    const auto& flows = units[binding.unit]->parsed.flows;
    const bool implemented =
        std::any_of(flows.begin(), flows.end(), [&](const auto& flow) {
          return flow->name.text == binding.member && !flow->interface_only;
        });
    if (!implemented) {
      for (const auto& flow : flows) {
        if (flow->name.text == binding.member && !flow->binding_name.empty()) {
          return flow->binding_name;
        }
      }
      return binding.member;
    }
  }
  return binding.unit == 0
             ? binding.member
             : Prefix(units[binding.unit]->document.identity) + binding.member;
}

bool Add(Unit& unit, std::string name, Binding binding) {
  if (Leaf(name) == "std" &&
      !(binding.unit == kBuiltin && binding.member == "std")) {
    Problem(unit, "flow.name.taken", "The builtin 'std' binding is reserved.",
            binding.location);
    return false;
  }
  if (binding.kind == MemberKind::kFlow) {
    auto& contracts = unit.contracts[name];
    const bool known =
        std::any_of(contracts.begin(), contracts.end(), [&](const auto& prior) {
          return prior.unit == binding.unit && prior.member == binding.member;
        });
    if (!known) {
      contracts.push_back(binding);
    }
  }
  const auto found = unit.members.find(name);
  if (found == unit.members.end()) {
    unit.members.emplace(std::move(name), std::move(binding));
    return true;
  }
  if (found->second.unit == binding.unit &&
      found->second.member == binding.member &&
      found->second.kind == binding.kind) {
    const bool changed = binding.exported && !found->second.exported;
    found->second.exported |= binding.exported;
    return changed;
  }
  if (found->second.kind == MemberKind::kFlow &&
      binding.kind == MemberKind::kFlow) {
    if (!found->second.implemented && binding.implemented) {
      found->second = std::move(binding);
      return true;
    }
    if (!binding.implemented) {
      return false;
    }
  }
  const bool reported =
      std::any_of(unit.parsed.diagnostics.begin(),
                  unit.parsed.diagnostics.end(), [&](const auto& d) {
                    return d.code == "flow.name.import-alias-conflict" &&
                           d.range.start.offset == binding.location.start;
                  });
  if (!reported) {
    Problem(unit, "flow.name.import-alias-conflict",
            absl::StrCat("Conflicting symbol '", name, "'. Use an alias."),
            binding.location);
  }
  return false;
}

void Shift(syntax::Location& location, size_t base, const LineIndex& lines) {
  if (location.start == 0 && location.end == 0) {
    return;
  }
  const Range range = lines.Between(location.start + base, location.end + base);
  location = {range.start.offset, range.end.offset, range.start.line,
              range.start.column};
}

void ShiftType(syntax::TypeExpression& type, size_t base,
               const LineIndex& lines) {
  Shift(type.location, base, lines);
  for (auto& parameter : type.parameters) {
    ShiftType(parameter, base, lines);
  }
}

void ShiftTree(Node& tree, size_t base, const LineIndex& lines) {
  syntax::VisitMutableSubtree(tree, [&](Node& node) {
    Shift(node.location, base, lines);
    Shift(node.extent, base, lines);
    for (auto& group : node.groups) {
      Shift(group, base, lines);
    }
    if (auto* f = syntax::As<syntax::FlowDeclaration>(&node); f != nullptr) {
      Shift(f->name.location, base, lines);
    }
    if (auto* d = syntax::As<syntax::DtoDeclaration>(&node); d != nullptr) {
      Shift(d->name.location, base, lines);
    }
    if (auto* p = syntax::As<syntax::PortDeclaration>(&node); p != nullptr) {
      Shift(p->name.location, base, lines);
      ShiftType(p->type, base, lines);
    }
    if (auto* f = syntax::As<syntax::FieldDeclaration>(&node); f != nullptr) {
      Shift(f->name.location, base, lines);
      ShiftType(f->type, base, lines);
    }
    if (auto* v = syntax::As<syntax::TypedValue>(&node); v != nullptr) {
      ShiftType(v->type, base, lines);
    }
    if (auto* b = syntax::As<syntax::Bind>(&node); b != nullptr) {
      Shift(b->name.location, base, lines);
      for (auto& name : b->names) {
        Shift(name.location, base, lines);
      }
    }
    if (auto* c = syntax::As<syntax::CallExpression>(&node); c != nullptr) {
      Shift(c->action_location, base, lines);
      for (auto& suffix : c->target_suffixes) {
        Shift(suffix.location, base, lines);
      }
      for (auto& a : c->args) {
        Shift(a.port.location, base, lines);
      }
    }
    const auto words = [&](auto& values) {
      for (auto& word : values) {
        Shift(word.location, base, lines);
      }
    };
    if (auto* item = syntax::As<syntax::Block>(&node); item != nullptr) {
      words(item->after);
    }
    if (auto* item = syntax::As<syntax::Pipe>(&node); item != nullptr) {
      words(item->after);
    }
    if (auto* item = syntax::As<syntax::Skip>(&node); item != nullptr) {
      words(item->after);
    }
    if (auto* item = syntax::As<syntax::Wait>(&node); item != nullptr) {
      words(item->after);
    }
    if (auto* item = syntax::As<syntax::Close>(&node); item != nullptr) {
      words(item->after);
    }
    if (auto* item = syntax::As<syntax::Cancel>(&node); item != nullptr) {
      words(item->after);
    }
    if (auto* item = syntax::As<syntax::Abort>(&node); item != nullptr) {
      words(item->after);
    }
    if (auto* item = syntax::As<syntax::Fail>(&node); item != nullptr) {
      words(item->after);
    }
    if (auto* item = syntax::As<syntax::Log>(&node); item != nullptr) {
      words(item->after);
    }
    if (auto* item = syntax::As<syntax::ForEach>(&node); item != nullptr) {
      words(item->after);
    }
    if (auto* item = syntax::As<syntax::Repeat>(&node); item != nullptr) {
      words(item->after);
    }
    if (auto* item = syntax::As<syntax::Cancel>(&node); item != nullptr) {
      Shift(item->name.location, base, lines);
    }
    if (auto* item = syntax::As<syntax::Repeat>(&node); item != nullptr) {
      Shift(item->variable.location, base, lines);
    }
    if (auto* item = syntax::As<syntax::Carry>(&node); item != nullptr) {
      Shift(item->name.location, base, lines);
    }
    if (auto* item = syntax::As<syntax::Nodes>(&node); item != nullptr) {
      Shift(item->name.location, base, lines);
    }
    if (auto* item = syntax::As<syntax::NodeExpression>(&node);
        item != nullptr) {
      Shift(item->node_map.location, base, lines);
    }
    if (auto* item = syntax::As<syntax::HeaderDeclaration>(&node);
        item != nullptr) {
      Shift(item->alias.location, base, lines);
    }
    if (auto* item = syntax::As<syntax::ForEach>(&node); item != nullptr) {
      words(item->variables);
    }
    if (auto* item = syntax::As<syntax::Stage>(&node); item != nullptr) {
      Shift(item->carried.location, base, lines);
    }
  });
}

}  // namespace

LinkedModule LinkModules(std::string source, std::string identity,
                         const ModuleSources& sources, ModuleResolver resolver,
                         ModuleLimits limits, CompilationBudget* budget) {
  CompilationBudget local_budget(limits);
  if (budget == nullptr) {
    budget = &local_budget;
  }
  if (identity.empty()) {
    identity = "<flow>";
  }
  std::vector<std::unique_ptr<Unit>> units;
  absl::flat_hash_map<std::string, size_t> loaded;
  auto root = std::make_unique<Unit>();
  root->document = {identity, std::move(source)};
  root->parsed = Parse(root->document.source, limits, budget);
  size_t bytes = root->document.source.size();
  if (bytes > limits.max_source_bytes || limits.max_modules == 0) {
    Problem(*root, "flow.form.import-context", "Module loading limit exceeded.",
            {});
  }
  units.push_back(std::move(root));
  loaded.emplace(identity, 0);
  for (size_t index = 0; index < units.size(); ++index) {
    if (!budget->Check()) {
      break;
    }
    Unit& unit = *units[index];
    const auto load = [&](const syntax::Import& import) {
      if (!budget->Check()) {
        return;
      }
      if (import.namespace_source) {
        return;
      }
      if (import.source == "std") {
        unit.dependencies[import.location.start] = kBuiltin;
        return;
      }
      const auto path = std::filesystem::path(import.source);
      const auto relative =
          (path.is_absolute()
               ? path
               : std::filesystem::path(unit.document.identity).parent_path() /
                     path)
              .lexically_normal()
              .generic_string();
      auto existing = loaded.find(relative);
      auto given = sources.find(relative);
      if (given == sources.end()) {
        given = sources.find(import.source);
      }
      absl::StatusOr<ModuleSource> document = absl::PermissionDeniedError(
          "Source imports are disallowed in the current runtime: no source "
          "roots are approved.");
      if (existing != loaded.end()) {
        document = ModuleSource{relative, {}};
      } else if (given != sources.end()) {
        document = ModuleSource{given->first, given->second};
      } else if (resolver) {
        document = resolver(import.source, unit.document.identity);
      }
      if (!budget->Check()) {
        return;
      }
      if (!document.ok()) {
        Problem(unit, "flow.name.module-not-found",
                std::string(document.status().message()), import.location);
        return;
      }
      if (document->identity.empty() || document->identity.size() > 4096 ||
          !utf8::IsValid(document->identity)) {
        Problem(unit, "flow.form.import-context", "Invalid module identity.",
                import.location);
        return;
      }
      existing = loaded.find(document->identity);
      size_t target;
      if (existing != loaded.end()) {
        target = existing->second;
      } else {
        if (units.size() >= limits.max_modules ||
            unit.depth >= limits.max_depth ||
            document->source.size() >
                limits.max_source_bytes -
                    std::min(bytes, limits.max_source_bytes)) {
          Problem(unit, "flow.form.import-context",
                  "Module loading limit exceeded.", import.location);
          return;
        }
        auto dependency = std::make_unique<Unit>();
        dependency->document = *std::move(document);
        dependency->depth = unit.depth + 1;
        dependency->parsed = Parse(dependency->document.source, limits, budget);
        bytes += dependency->document.source.size();
        target = units.size();
        loaded.emplace(dependency->document.identity, target);
        units.push_back(std::move(dependency));
      }
      unit.dependencies[import.location.start] = target;
    };
    for (const auto& import : unit.parsed.imports) {
      load(import);
    }
    for (const auto& flow : unit.parsed.flows) {
      for (const auto& import : flow->imports) {
        load(import);
      }
    }
  }

  std::vector<absl::flat_hash_map<const syntax::FlowDeclaration*, std::string>>
      environments(units.size());
  for (size_t index = 0; index < units.size(); ++index) {
    auto& unit = *units[index];
    for (const auto& module : unit.parsed.modules) {
      if (Leaf(module.name.text) == "std") {
        Problem(unit, "flow.name.taken",
                "The builtin 'std' module is reserved.", module.name.location);
        continue;
      }
      Add(unit, module.name.text,
          Binding{index, module.name.text, MemberKind::kModule, module.exported,
                  module.name.location});
    }
    const auto owned_module = [&](const std::string& scope,
                                  const syntax::Location& location) {
      if (scope.empty() || unit.parents.find(scope) != unit.parents.end()) {
        return;
      }
      const auto module = unit.members.find(scope);
      if (module == unit.members.end() ||
          module->second.kind != MemberKind::kModule ||
          module->second.unit != index) {
        Problem(
            unit, "flow.name.unknown",
            "A qualified definition requires an existing locally owned module.",
            location);
      }
    };
    std::vector<syntax::FlowDeclarationPtr> contracts;
    for (auto& flow : unit.parsed.flows) {
      flow->scope = Parent(flow->name.text);
      if (!flow->interface_only) {
        owned_module(flow->scope, flow->name.location);
      }
      const auto env = absl::StrCat("$flow_", flow->location.start);
      unit.parents[env] = flow->scope;
      environments[index][flow.get()] = env;
      for (auto& import : flow->imports) {
        import.scope = env;
        if (import.exported) {
          Problem(unit, "flow.form.import-context",
                  "A flow-local import is private.", import.location);
        }
      }
      for (auto& contract : flow->declarations) {
        contract->binding_name = contract->name.text;
        contract->name.text = Join(env, contract->name.text);
        contract->scope = env;
        contract->imported = true;
        environments[index][contract.get()] = env;
        contracts.push_back(std::move(contract));
      }
      flow->declarations.clear();
      for (auto& type : flow->structs) {
        type->scope = env;
        type->name.text = Join(env, type->name.text);
        unit.parsed.dtos.push_back(std::move(type));
      }
      flow->structs.clear();
    }
    for (auto& contract : contracts) {
      unit.parsed.flows.push_back(std::move(contract));
    }
    for (const auto& type : unit.parsed.dtos) {
      owned_module(type->scope, type->name.location);
      Add(unit, type->name.text,
          Binding{index, type->name.text, MemberKind::kType, type->exported,
                  type->name.location});
    }
    for (const auto& flow : unit.parsed.flows) {
      if (!flow->entry) {
        Add(unit, flow->name.text,
            Binding{index, flow->name.text, MemberKind::kFlow, flow->exported,
                    flow->name.location, !flow->interface_only});
      }
    }
  }

  const auto imports_of = [&](Unit& unit) {
    std::vector<const syntax::Import*> imports;
    for (const auto& import : unit.parsed.imports) {
      imports.push_back(&import);
    }
    for (const auto& flow : unit.parsed.flows) {
      for (const auto& import : flow->imports) {
        imports.push_back(&import);
      }
    }
    return imports;
  };
  const auto imported =
      [&](size_t index,
          const syntax::Import& import) -> std::optional<Binding> {
    if (import.namespace_source) {
      return Lookup(units, index, import.scope, import.source);
    }
    const auto dependency =
        units[index]->dependencies.find(import.location.start);
    if (dependency == units[index]->dependencies.end()) {
      return std::nullopt;
    }
    return Binding{dependency->second,
                   dependency->second == kBuiltin ? "std" : "",
                   MemberKind::kModule, import.exported, import.location};
  };
  const auto candidates = [&](const Binding& module) {
    std::vector<std::pair<std::string, Binding>> result;
    if (module.unit == kBuiltin) {
      absl::flat_hash_set<std::string> names;
      for (auto name : vocabulary::OrderedBuiltins()) {
        names.emplace(name);
      }
      for (auto name : vocabulary::OrderedTypeNames()) {
        names.emplace(name);
      }
      for (auto name :
           {"status", "option", "stream", "null", "zip", "interleave"}) {
        names.emplace(name);
      }
      for (const auto& name : names) {
        result.emplace_back(name, *Standard(Join("std", name)));
      }
      return result;
    }
    for (const auto& [name, member] : units[module.unit]->members) {
      if (!budget->Check()) {
        break;
      }
      if (member.exported && Parent(name) == module.member) {
        result.emplace_back(Leaf(name), member);
      }
    }
    return result;
  };

  while (budget->Check()) {
    bool changed = false;
    for (size_t index = 0; index < units.size(); ++index) {
      auto& unit = *units[index];
      for (const auto* import : imports_of(unit)) {
        auto module = imported(index, *import);
        if (!module.has_value() || module->kind != MemberKind::kModule) {
          continue;
        }
        if (!import->selective) {
          auto binding = *module;
          binding.exported = import->exported;
          binding.location = import->alias.location;
          changed |=
              Add(unit, Join(import->scope, import->alias.text), binding);
          continue;
        }
        for (const auto& selection : import->symbols) {
          auto available = candidates(*module);
          for (const auto& [name, original] : available) {
            if (selection.name.text != "*" && selection.name.text != name) {
              continue;
            }
            auto binding = original;
            binding.exported = import->exported;
            binding.location = selection.alias.location;
            const auto alias =
                selection.name.text == "*" ? name : selection.alias.text;
            changed |=
                Add(unit, Join(import->scope, alias), std::move(binding));
          }
        }
      }
      for (const auto& export_ : unit.parsed.exports) {
        auto binding = Lookup(units, index, export_.scope, export_.symbol.text);
        if (!binding.has_value()) {
          continue;
        }
        binding->exported = true;
        const auto alias = export_.alias.Empty() ? Leaf(export_.symbol.text)
                                                 : export_.alias.text;
        changed |= Add(unit, Join(export_.scope, alias), *binding);
      }
    }
    for (size_t index = 0; index < units.size(); ++index) {
      auto& unit = *units[index];
      std::vector<std::pair<std::string, Binding>> inherited_contracts;
      for (const auto& [name, binding] : unit.members) {
        const auto parent = unit.parents.find(Parent(name));
        if (binding.kind != MemberKind::kFlow || parent == unit.parents.end()) {
          continue;
        }
        const auto outer = Lookup(units, index, parent->second, Leaf(name));
        if (outer.has_value() && outer->kind == MemberKind::kFlow &&
            !outer->implemented) {
          inherited_contracts.emplace_back(name, *outer);
        }
      }
      for (const auto& [name, binding] : inherited_contracts) {
        Add(unit, name, binding);
      }
      for (const auto& declaration : unit.parsed.flows) {
        if (!declaration->interface_only || declaration->binding_name.empty()) {
          continue;
        }
        const auto parent = unit.parents.find(declaration->scope);
        if (parent == unit.parents.end()) {
          continue;
        }
        const auto candidate =
            Lookup(units, index, parent->second, declaration->binding_name);
        if (candidate.has_value() && candidate->kind == MemberKind::kFlow) {
          changed |= Add(unit, declaration->name.text, *candidate);
        }
      }
    }
    size_t names = 0;
    for (const auto& unit : units) {
      for (const auto& [name, binding] : unit->members) {
        names += name.size() + binding.member.size();
      }
    }
    if (names > limits.max_linked_name_bytes) {
      Problem(*units[0], "flow.form.import-context",
              "Module names exceed the linking budget.", {});
      break;
    }
    if (!changed) {
      break;
    }
  }

  if (budget->exhausted()) {
    Problem(*units[0], "flow.form.resource-limit",
            "Compilation exceeded the configured work or time limit.", {});
  }
  LinkedModule linked;
  for (size_t index = 0; index < units.size(); ++index) {
    auto& unit = *units[index];
    for (const auto* import : imports_of(unit)) {
      auto module = imported(index, *import);
      if (!module.has_value() || module->kind != MemberKind::kModule) {
        Problem(unit, "flow.name.module-not-found",
                "The import does not name an available module.",
                import->location);
        continue;
      }
      if (import->selective) {
        const auto available = candidates(*module);
        if (index == 0) {
          const auto line_end =
              unit.document.source.find('\n', import->selection_start);
          const auto end = line_end == std::string::npos
                               ? unit.document.source.size()
                               : std::max(line_end, import->extent.end);
          linked.selections.push_back({import->selection_start, end});
          for (const auto& [name, binding] : available) {
            linked.members.push_back(
                {name, Target(units, binding),
                 binding.unit == kBuiltin
                     ? "std"
                     : units[binding.unit]->document.identity,
                 binding.location, import->selection_start, end, false,
                 binding.kind == MemberKind::kType, true,
                 binding.unit == kBuiltin ? Target(units, binding) : "",
                 binding.kind == MemberKind::kModule});
          }
        }
        for (const auto& selection : import->symbols) {
          if (selection.name.text == "*") {
            continue;
          }
          const auto found = std::find_if(
              available.begin(), available.end(), [&](const auto& candidate) {
                return candidate.first == selection.name.text;
              });
          if (found == available.end()) {
            Problem(unit, "flow.name.private-member",
                    "The imported member is missing or private.",
                    selection.name.location);
          } else {
            const auto target =
                unit.members.find(Join(import->scope, selection.alias.text));
            if (target != unit.members.end() &&
                found->second.kind != MemberKind::kFlow &&
                (target->second.unit != found->second.unit ||
                 target->second.member != found->second.member)) {
              Problem(unit, "flow.name.import-alias-conflict",
                      "The imported name conflicts with another symbol.",
                      selection.alias.location);
            }
          }
        }
      }
    }
    for (const auto& export_ : unit.parsed.exports) {
      const auto binding =
          Lookup(units, index, export_.scope, export_.symbol.text);
      if (!binding.has_value()) {
        Problem(unit, "flow.name.unknown",
                "The export does not name an accessible symbol.",
                export_.symbol.location);
      }
    }
    if (index == 0) {
      const auto emit = [&](std::string_view scope, size_t begin, size_t end,
                            bool scoped) {
        struct Pending {
          std::string name;
          Binding binding;
          size_t depth;
        };
        std::vector<Pending> pending;
        for (const auto& [name, binding] : unit.members) {
          if (Parent(name) == scope) {
            pending.push_back({Leaf(name), binding, 0});
          }
        }
        absl::flat_hash_set<std::pair<size_t, std::string>> visited;
        while (!pending.empty() && budget->Check()) {
          auto next = std::move(pending.back());
          pending.pop_back();
          const auto& binding = next.binding;
          const bool module = binding.kind == MemberKind::kModule;
          linked.members.push_back(
              {next.name, module ? binding.member : Target(units, binding),
               binding.unit == kBuiltin
                   ? "std"
                   : units[binding.unit]->document.identity,
               binding.location, begin, end, scoped,
               binding.kind == MemberKind::kType, false,
               binding.unit == kBuiltin && !module ? Target(units, binding)
                                                   : "",
               module});
          if (!module || next.depth >= limits.max_depth ||
              !visited.emplace(binding.unit, binding.member).second) {
            continue;
          }
          for (const auto& [leaf, child] : candidates(binding)) {
            pending.push_back({Join(next.name, leaf), child, next.depth + 1});
          }
        }
      };
      emit("", 0, unit.document.source.size(), false);
      for (const auto& flow : unit.parsed.flows) {
        emit(environments[index].at(flow.get()), flow->extent.start,
             flow->extent.end, true);
        if (!flow->scope.empty()) {
          emit(flow->scope, flow->extent.start, flow->extent.end, true);
        }
      }
    }
    unit.start = linked.source.size();
    linked.documents.push_back({unit.document.identity, unit.start,
                                unit.start + unit.document.source.size()});
    linked.source += unit.document.source;
    linked.source += '\n';
  }

  absl::flat_hash_map<std::pair<size_t, std::string>, std::string> targets;
  for (const auto& unit : units) {
    for (const auto& [name, binding] : unit->members) {
      targets.try_emplace({binding.unit, binding.member},
                          Target(units, binding));
    }
  }
  absl::flat_hash_map<std::string, std::string> canonical;
  absl::flat_hash_set<std::string> implementations;
  for (const auto& unit : units) {
    for (const auto& [name, binding] : unit->members) {
      const auto target = Target(units, binding);
      canonical.try_emplace(target, target);
      if (binding.implemented) {
        implementations.insert(target);
      }
    }
  }
  const auto canonical_target = [&](std::string target) {
    std::vector<std::string> path;
    while (budget->Check()) {
      const auto at = canonical.find(target);
      if (at == canonical.end() || at->second == target) {
        break;
      }
      path.push_back(target);
      target = at->second;
    }
    for (const auto& item : path) {
      canonical[item] = target;
    }
    return target;
  };
  for (const auto& unit : units) {
    for (const auto& [name, contracts] : unit->contracts) {
      const auto chosen = unit->members.find(name);
      if (chosen == unit->members.end()) {
        continue;
      }
      for (const auto& contract : contracts) {
        auto left = canonical_target(Target(units, chosen->second));
        auto right = canonical_target(Target(units, contract));
        if (left == right) {
          continue;
        }
        if ((!implementations.contains(left) &&
             implementations.contains(right)) ||
            (implementations.contains(left) ==
                 implementations.contains(right) &&
             right < left)) {
          std::swap(left, right);
        }
        canonical[right] = left;
      }
    }
  }
  for (auto& [key, target] : targets) {
    target = canonical_target(target);
  }
  for (auto& member : linked.members) {
    member.target = canonical_target(member.target);
  }
  const auto target_of = [&](const Binding& binding) {
    if (binding.unit == kBuiltin) {
      return binding.member.substr(5);
    }
    const auto found = targets.find({binding.unit, binding.member});
    return found == targets.end() ? binding.member : found->second;
  };
  const auto inaccessible = [&](size_t owner, const std::string& scope,
                                std::string_view path,
                                const syntax::Location& use) {
    const auto separator = path.rfind("::");
    if (separator == std::string_view::npos) {
      return false;
    }
    const auto module = Lookup(units, owner, scope, path.substr(0, separator));
    if (!module.has_value() || module->kind != MemberKind::kModule ||
        module->unit == kBuiltin) {
      return false;
    }
    const auto& target = *units[module->unit];
    const auto found =
        target.members.find(Join(module->member, path.substr(separator + 2)));
    if (found == target.members.end()) {
      return false;
    }
    Problem(*units[owner], "flow.name.private-member",
            "This member exists but is private; expose it with 'exported' or "
            "an export directive.",
            use);
    auto& diagnostic = units[owner]->parsed.diagnostics.back();
    RelatedDiagnostic related;
    related.message = "Private declaration.";
    related.source = target.document.identity;
    related.range =
        LineIndex(target.document.source)
            .Between(found->second.location.start, found->second.location.end);
    diagnostic.related.push_back(std::move(related));
    return true;
  };
  using PublicTypes =
      absl::flat_hash_map<std::string, absl::flat_hash_set<std::string>>;
  std::vector<PublicTypes> public_types(units.size());
  std::vector<
      absl::flat_hash_map<std::string, absl::flat_hash_set<std::string>>>
      exposures(units.size());
  for (size_t index = 0; index < units.size(); ++index) {
    const auto& unit = *units[index];
    for (const auto& [name, binding] : unit.members) {
      if (!binding.exported) {
        continue;
      }
      const auto target = target_of(binding);
      exposures[index][target].insert(Parent(name));
      if (binding.kind != MemberKind::kType) {
        continue;
      }
      auto scope = Parent(name);
      for (;;) {
        public_types[index][scope].insert(target);
        if (scope.empty()) {
          break;
        }
        const auto parent = unit.members.find(scope);
        if (parent == unit.members.end() || !parent->second.exported) {
          break;
        }
        scope = Parent(scope);
      }
    }
  }
  const auto resolve_type = [&](size_t index, std::string scope,
                                syntax::TypeExpression& type,
                                std::string_view exposed = {}) {
    std::vector<syntax::TypeExpression*> pending{&type};
    while (!pending.empty()) {
      if (!budget->Check()) {
        break;
      }
      auto* next = pending.back();
      pending.pop_back();
      for (auto& parameter : next->parameters) {
        pending.push_back(&parameter);
      }
      if (next->quoted) {
        continue;
      }
      const auto binding = Lookup(units, index, scope, next->name);
      if (binding.has_value()) {
        if (!exposed.empty() && binding->kind == MemberKind::kType) {
          for (const auto& public_scope :
               exposures[index][std::string(exposed)]) {
            if (!public_types[index][public_scope].contains(
                    target_of(*binding))) {
              Problem(*units[index], "flow.form.private-type-exposed",
                      "An exported interface exposes a private type; export "
                      "the type or a public alias in that module.",
                      next->location);
              break;
            }
          }
        }
        next->name = target_of(*binding);
      } else if (inaccessible(index, scope, next->name, next->location)) {
        next->name = "any";
      } else if (next->name.find("::") != std::string::npos &&
                 !next->name.starts_with("std::") &&
                 !Lookup(units, index, scope,
                         next->name.substr(0, next->name.find("::")))
                      .has_value()) {
        next->name = absl::StrReplaceAll(next->name, {{"::", "."}});
      } else if (vocabulary::TypeNames().contains(next->name) &&
                 !vocabulary::IsPrimitiveType(next->name) && !next->sugared) {
        Problem(*units[index], "flow.form.unknown-type",
                "Qualify this standard type with 'std::' or import it.",
                next->location);
      }
    }
  };
  const LineIndex lines(linked.source);
  for (size_t index = 0; index < units.size(); ++index) {
    auto& unit = *units[index];
    for (auto& type : unit.parsed.dtos) {
      for (auto& field : type->fields) {
        resolve_type(index, type->scope, field->type,
                     unit.members.contains(type->name.text)
                         ? target_of(unit.members.at(type->name.text))
                         : "");
      }
    }
    for (auto& flow : unit.parsed.flows) {
      const auto& env = environments[index].at(flow.get());
      syntax::VisitMutableSubtree(*flow, [&](Node& node) {
        if (!budget->Check()) {
          return;
        }
        if (auto* call = syntax::As<syntax::CallExpression>(&node);
            call != nullptr) {
          if (auto binding = Lookup(units, index, env, call->action);
              binding.has_value()) {
            if (binding->kind != MemberKind::kFlow) {
              Problem(unit, "flow.name.unknown",
                      "An action call requires a flow or action symbol.",
                      call->action_location);
            } else {
              call->action = target_of(*binding);
            }
          } else {
            inaccessible(index, env, call->action, call->action_location);
          }
        }
        if (auto* zip = syntax::As<syntax::Zip>(&node); zip != nullptr) {
          const auto binding = Lookup(units, index, env, zip->name);
          if (!binding.has_value() || binding->unit != kBuiltin) {
            Problem(unit, "flow.form.unknown-builtin",
                    "Qualify this standard function with 'std::' or import it.",
                    zip->location);
          } else {
            zip->name = binding->member.substr(5);
          }
        }
        if (auto* function = syntax::As<syntax::Builtin>(&node);
            function != nullptr) {
          const auto binding = Lookup(units, index, env, function->name);
          if (!binding.has_value() || binding->unit != kBuiltin) {
            Problem(unit, "flow.form.unknown-builtin",
                    "Qualify this standard function with 'std::' or import it.",
                    function->location);
          } else {
            function->name = binding->member.substr(5);
          }
        }
        if (auto* port = syntax::As<syntax::PortDeclaration>(&node);
            port != nullptr) {
          resolve_type(index, env, port->type,
                       unit.members.contains(flow->name.text)
                           ? target_of(unit.members.at(flow->name.text))
                           : "");
        }
        if (auto* value = syntax::As<syntax::TypedValue>(&node);
            value != nullptr) {
          resolve_type(index, env, value->type);
        }
        if (auto* bind = syntax::As<syntax::Bind>(&node);
            bind != nullptr && bind->type.has_value()) {
          resolve_type(index, env, *bind->type);
        }
      });
    }
    for (auto& type : unit.parsed.dtos) {
      const auto binding = unit.members.find(type->name.text);
      if (binding != unit.members.end()) {
        type->name.text = target_of(binding->second);
      }
      type->module = unit.document.identity;
      ShiftTree(*type, unit.start, lines);
      linked.parsed.dtos.push_back(std::move(type));
    }
    for (auto& flow : unit.parsed.flows) {
      if (flow->binding_name.empty()) {
        flow->binding_name = flow->name.text;
      }
      if (!flow->entry) {
        const auto binding = unit.members.find(flow->name.text);
        if (binding != unit.members.end()) {
          flow->exported |= binding->second.exported;
          flow->name.text = target_of(binding->second);
        }
      }
      flow->registerable = index == 0 && !flow->interface_only;
      if (!flow->scope.empty()) {
        flow->registerable &= flow->exported;
        for (auto prefix = flow->scope; !prefix.empty();
             prefix = Parent(prefix)) {
          const auto module = unit.members.find(prefix);
          flow->registerable &=
              module != unit.members.end() && module->second.exported;
        }
      }
      flow->module = unit.document.identity;
      flow->imported |= index != 0;
      if (index != 0 && flow->entry) {
        continue;
      }
      ShiftTree(*flow, unit.start, lines);
      linked.parsed.flows.push_back(std::move(flow));
    }
    for (auto& diagnostic : unit.parsed.diagnostics) {
      linked.parsed.diagnostics.push_back(std::move(diagnostic));
    }
    for (auto& module : unit.parsed.modules) {
      Shift(module.extent, unit.start, lines);
      Shift(module.name.location, unit.start, lines);
      linked.parsed.modules.push_back(std::move(module));
    }
    if (index == 0) {
      linked.parsed.imports = std::move(unit.parsed.imports);
      linked.parsed.exports = std::move(unit.parsed.exports);
    }
    for (auto brace : unit.parsed.value_braces) {
      linked.parsed.value_braces.push_back(brace + unit.start);
    }
    for (auto brace : unit.parsed.tagged_braces) {
      linked.parsed.tagged_braces.push_back(brace + unit.start);
    }
  }
  if (budget->exhausted() &&
      std::none_of(linked.parsed.diagnostics.begin(),
                   linked.parsed.diagnostics.end(), [](const auto& diagnostic) {
                     return diagnostic.code == "flow.form.resource-limit";
                   })) {
    Diagnostic diagnostic;
    diagnostic.code = "flow.form.resource-limit";
    diagnostic.family = Family::kForm;
    diagnostic.severity = Severity::kError;
    diagnostic.message =
        "Compilation exceeded the configured work or time limit.";
    diagnostic.source = identity;
    linked.parsed.diagnostics.push_back(std::move(diagnostic));
  }
  return linked;
}

bool LinkedModule::SelectingImport(size_t offset) const {
  return std::any_of(
      selections.begin(), selections.end(), [&](const auto& selection) {
        return selection.start <= offset && offset <= selection.end;
      });
}

catalogue::Catalogue LinkedModule::CatalogueAt(
    size_t offset, bool import_selection, CompilationLimits limits,
    CompilationBudget* budget) const {
  const auto resolved = Resolve(source, parsed, false, nullptr, limits, budget);
  const bool selecting = import_selection && SelectingImport(offset);
  const auto visible_type = [&](std::string text) {
    std::vector<std::pair<std::string, std::string>> names;
    for (const auto& member : members) {
      if (member.selection == selecting && member.type &&
          member.scope_start <= offset && offset <= member.scope_end) {
        names.emplace_back(member.target, member.name);
      }
    }
    std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) {
      return a.first.size() > b.first.size();
    });
    for (const auto& [target, name] : names) {
      size_t position = 0;
      while ((position = text.find(target, position)) != std::string::npos) {
        text.replace(position, target.size(), name);
        position += name.size();
      }
    }
    return text;
  };
  std::vector<catalogue::ActionInfo> actions;
  std::vector<catalogue::TypeInfo> types;
  absl::flat_hash_map<std::string, std::string> intrinsics;
  absl::flat_hash_set<std::string> offered;
  absl::flat_hash_set<std::string> shadowed;
  std::vector<const ModuleMember*> visible;
  for (const auto& member : members) {
    if (member.selection != selecting || offset < member.scope_start ||
        offset > member.scope_end) {
      continue;
    }
    visible.push_back(&member);
    if (member.scoped && !selecting) {
      shadowed.insert(member.name.substr(0, member.name.find("::")));
    }
  }
  std::stable_sort(visible.begin(), visible.end(),
                   [](const auto* left, const auto* right) {
                     return left->scoped > right->scoped;
                   });
  for (const auto* pointer : visible) {
    const auto& member = *pointer;
    if (!member.scoped && !selecting &&
        shadowed.contains(member.name.substr(0, member.name.find("::")))) {
      continue;
    }
    if (!offered.insert(member.name).second || member.module) {
      continue;
    }
    if (!member.intrinsic.empty()) {
      intrinsics[member.name] = member.intrinsic;
      continue;
    }
    const catalogue::Origin origin{member.source, member.location.line,
                                   member.location.column};
    if (member.type) {
      if (const auto* dto = resolved.program.Dto(member.target);
          dto != nullptr) {
        auto shape = *dto;
        shape.name = member.name;
        for (auto& field : shape.fields) {
          field.declared = visible_type(field.declared);
          field.type = visible_type(field.type);
          field.dto_name = visible_type(field.dto_name);
          field.element_dto_name = visible_type(field.element_dto_name);
        }
        types.push_back({member.name, std::move(shape), origin});
      }
    } else if (const auto* flow = resolved.program.Flow(member.target);
               flow != nullptr) {
      catalogue::ActionInfo action;
      action.name = member.name;
      action.description = flow->description;
      action.origin = origin;
      for (const auto& declaration : parsed.flows) {
        if (declaration->name.text != member.target) {
          continue;
        }
        for (const auto& document : documents) {
          const size_t start = declaration->name.location.start;
          if (start < document.start || start > document.end) {
            continue;
          }
          const auto position =
              LineIndex(std::string_view(source).substr(
                            document.start, document.end - document.start))
                  .At(start - document.start);
          const catalogue::Origin contributor{document.identity, position.line,
                                              position.column};
          if (std::find(action.schema_origins.begin(),
                        action.schema_origins.end(),
                        contributor) == action.schema_origins.end()) {
            action.schema_origins.push_back(contributor);
          }
          break;
        }
      }
      for (const auto& port : flow->ports) {
        catalogue::PortInfo info{
            port.name,
            visible_type(port.declared.empty() ? port.type : port.declared),
            port.description, port.required, port.unary};
        (port.direction == syntax::PortDirection::kInput ? action.inputs
                                                         : action.outputs)
            .push_back(std::move(info));
      }
      for (const auto& header : flow->headers) {
        action.headers.push_back(
            {header.name, "bytes", header.description, false, true});
      }
      actions.push_back(std::move(action));
    }
  }
  return catalogue::Catalogue::Of(std::move(actions), std::move(types),
                                  std::move(intrinsics));
}

void LinkedModule::Locate(std::vector<Diagnostic>& diagnostics) const {
  for (auto& diagnostic : diagnostics) {
    for (auto& related : diagnostic.related) {
      if (!related.source.empty()) {
        continue;
      }
      for (const auto& doc : documents) {
        if (doc.start <= related.range.start.offset &&
            related.range.start.offset <= doc.end) {
          related.source = doc.identity;
          related.range = LineIndex(std::string_view(source).substr(
                                        doc.start, doc.end - doc.start))
                              .Between(related.range.start.offset - doc.start,
                                       related.range.end.offset - doc.start);
          break;
        }
      }
    }
  }
  for (auto& d : diagnostics) {
    if (!d.source.empty()) {
      continue;
    }
    for (const auto& doc : documents) {
      if (d.range.start.offset < doc.start || d.range.start.offset > doc.end) {
        continue;
      }
      const LineIndex lines(
          std::string_view(source).substr(doc.start, doc.end - doc.start));
      d.source = doc.identity;
      d.range = lines.Between(d.range.start.offset - doc.start,
                              d.range.end.offset - doc.start);
      for (auto& fix : d.fixes) {
        for (auto& edit : fix.edits) {
          edit.start -= doc.start;
          edit.end -= doc.start;
        }
      }
      break;
    }
  }
}
}  // namespace a11::flow
