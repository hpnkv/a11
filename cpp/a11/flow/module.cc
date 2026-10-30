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
#include <tuple>
#include <utility>

#include <absl/container/flat_hash_set.h>
#include <absl/strings/str_cat.h>

#include "a11/flow/resolve.h"
#include "a11/utf8.h"

namespace a11::flow {
namespace {
using syntax::Node;
using syntax::NodeKind;

struct Alias {
  size_t unit = 0;
  size_t location = 0;
  std::string member;
  bool local = false;
  std::vector<std::pair<size_t, std::string>> contributions;
};

using Aliases = absl::flat_hash_map<std::string, Alias>;

struct NameBudget {
  size_t remaining = 0;
  std::vector<Diagnostic> diagnostics;
};

struct Unit {
  ModuleSource document;
  ParseResult parsed;
  Aliases imports;
  absl::flat_hash_map<size_t, size_t> import_targets;
  absl::flat_hash_set<size_t> used_imports;
  std::vector<size_t> dependencies;
  std::string prefix;
  absl::flat_hash_set<std::string> scoped_names;
  size_t depth = 0;
  size_t start = 0;
  std::shared_ptr<NameBudget> names;
};

std::string Prefix(std::string_view identity) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string prefix = "module_";
  for (const auto byte : identity) {
    const auto value = static_cast<unsigned char>(byte);
    prefix += hex[value >> 4];
    prefix += hex[value & 15];
  }
  prefix += '.';
  return prefix;
}

std::string Path(std::string_view reference, std::string_view importer) {
  std::filesystem::path ref(reference);
  if (ref.is_relative()) {
    ref = std::filesystem::path(importer).parent_path() / ref;
  }
  return ref.lexically_normal().generic_string();
}

void Problem(Unit& unit, std::string code, std::string message,
             const syntax::Location& location,
             Severity severity = Severity::kError) {
  Diagnostic d;
  d.source = unit.document.identity;
  d.code = std::move(code);
  d.message = std::move(message);
  d.family = d.code.find("flow.name.") == 0 ? Family::kName : Family::kForm;
  d.severity = severity;
  d.range =
      LineIndex(unit.document.source).Between(location.start, location.end);
  unit.parsed.diagnostics.push_back(std::move(d));
}

bool Exported(const Unit& unit, std::string_view name, bool type) {
  if (type) {
    for (const auto& d : unit.parsed.dtos) {
      if (d->name.text == name && d->exported) {
        return true;
      }
    }
  } else {
    for (const auto& f : unit.parsed.flows) {
      if (f->name.text == name && f->exported) {
        return true;
      }
    }
  }
  return false;
}

const syntax::Word* absl_nullable DeclarationOf(const Unit& unit,
                                                std::string_view name,
                                                bool type) {
  if (type) {
    for (const auto& d : unit.parsed.dtos) {
      if (d->name.text == name) {
        return &d->name;
      }
    }
  } else {
    for (const auto& f : unit.parsed.flows) {
      if (f->name.text == name && !f->entry) {
        return &f->name;
      }
    }
  }
  return nullptr;
}

bool Defined(const Unit& unit, std::string_view name, bool type) {
  return DeclarationOf(unit, name, type) != nullptr;
}

std::string Target(const Unit& unit, std::string_view name, bool type) {
  const bool scoped =
      unit.scoped_names.find(std::string(name)) != unit.scoped_names.end();
  if (!type && !scoped) {
    bool implementation = false;
    for (const auto& f : unit.parsed.flows) {
      if (f->name.text == name && !f->interface_only) {
        implementation = true;
      }
    }
    if (!implementation) {
      return std::string(name);
    }
  }
  if ((!unit.prefix.empty() || scoped) && unit.names != nullptr) {
    const size_t length = (scoped ? 0 : unit.prefix.size()) + name.size();
    if (length > unit.names->remaining) {
      if (unit.names->diagnostics.empty()) {
        Diagnostic diagnostic;
        diagnostic.code = "flow.form.import-context";
        diagnostic.source = unit.document.identity;
        diagnostic.family = Family::kForm;
        diagnostic.message =
            "Qualified module names exceed the linking memory budget.";
        unit.names->diagnostics.push_back(std::move(diagnostic));
      }
      return std::string(name);
    }
    unit.names->remaining -= length;
  }
  return scoped ? std::string(name) : absl::StrCat(unit.prefix, name);
}

std::string ResolveName(std::string name, bool type, Unit& unit,
                        const Aliases& aliases,
                        const std::vector<std::unique_ptr<Unit>>& units,
                        const syntax::Location& location) {
  if (const auto found = aliases.find(name);
      found != aliases.end() && !found->second.member.empty()) {
    const auto& alias = found->second;
    if (Defined(*units[alias.unit], alias.member, type)) {
      if (!alias.local) {
        unit.used_imports.insert(alias.location);
      }
      return Target(*units[alias.unit], alias.member, type);
    }
  }
  const size_t dot = name.find('.');
  if (dot != std::string::npos) {
    const auto found = aliases.find(name.substr(0, dot));
    if (found != aliases.end() && found->second.member.empty()) {
      const Unit& dependency = *units[found->second.unit];
      unit.used_imports.insert(found->second.location);
      const std::string member = name.substr(dot + 1);
      const auto* declaration = DeclarationOf(dependency, member, type);
      if (declaration == nullptr) {
        Problem(unit, "flow.name.unknown",
                absl::StrCat("Module '", dependency.document.identity,
                             "' has no member '", member, "'."),
                location);
        return name;
      }
      if (!Exported(dependency, member, type)) {
        Problem(unit, "flow.name.private-member",
                absl::StrCat("Module member '", name, "' exists in '",
                             dependency.document.identity,
                             "' but is private. Mark its declaration "
                             "'exported' to use it here."),
                location);
        unit.parsed.diagnostics.back().related.push_back(
            {dependency.document.identity,
             LineIndex(dependency.document.source)
                 .Between(declaration->location.start,
                          declaration->location.end),
             type ? "Private struct declared here."
                  : "Private callable declared here."});
      }
      return Target(dependency, member, type);
    }
  }
  return Defined(unit, name, type) ? Target(unit, name, type) : name;
}

void CheckPublicType(const syntax::TypeExpression& type, Unit& unit) {
  if (!type.quoted && Defined(unit, type.name, true) &&
      !Exported(unit, type.name, true)) {
    Problem(unit, "flow.form.private-type-exposed",
            "An exported interface names a private struct; mark that struct "
            "'exported'.",
            type.location);
  }
  for (const auto& parameter : type.parameters) {
    CheckPublicType(parameter, unit);
  }
}

void RewriteType(syntax::TypeExpression& type, Unit& unit,
                 const Aliases& aliases,
                 const std::vector<std::unique_ptr<Unit>>& units) {
  if (!type.quoted) {
    type.name =
        ResolveName(type.name, true, unit, aliases, units, type.location);
  }
  for (auto& parameter : type.parameters) {
    RewriteType(parameter, unit, aliases, units);
  }
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
    }
    if (auto* l = syntax::As<syntax::Let>(&node); l != nullptr) {
      for (auto& w : l->names) {
        Shift(w.location, base, lines);
      }
    }
    if (auto* m = syntax::As<syntax::CallModifiers>(&node); m != nullptr) {
      Shift(m->node_map.location, base, lines);
      for (auto& w : m->after) {
        Shift(w.location, base, lines);
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
    if (auto* item = syntax::As<syntax::Drain>(&node); item != nullptr) {
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
    if (auto* item = syntax::As<syntax::Advance>(&node); item != nullptr) {
      Shift(item->name.location, base, lines);
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
                         ModuleLimits limits) {
  if (identity.empty()) {
    identity = "<flow>";
  }
  std::vector<std::unique_ptr<Unit>> units;
  absl::flat_hash_map<std::string, size_t> loaded;
  auto names = std::make_shared<NameBudget>();
  names->remaining = limits.max_linked_name_bytes;
  auto root = std::make_unique<Unit>();
  root->names = names;
  root->document = {identity, std::move(source)};
  if (root->document.source.size() > limits.max_source_bytes ||
      limits.max_modules == 0 ||
      (!utf8::IsValid(identity) || identity.size() > 4096)) {
    Problem(*root, "flow.form.import-context",
            "The root module exceeds the loading budget or has an invalid "
            "identity.",
            {});
  } else {
    root->parsed = Parse(root->document.source);
  }
  size_t bytes = root->document.source.size();
  units.push_back(std::move(root));
  loaded.emplace(identity, 0);
  const auto load = [&](Unit& unit, const syntax::Import& import,
                        Aliases& aliases) {
    const std::string relative = Path(import.source, unit.document.identity);
    absl::StatusOr<ModuleSource> result = absl::NotFoundError(
        absl::StrCat("Module '", import.source, "' is not supplied."));
    const auto pinned = loaded.find(relative);
    if (pinned != loaded.end()) {
      result = ModuleSource{relative, {}};
    }
    auto provided = sources.find(relative);
    if (provided == sources.end()) {
      provided = sources.find(import.source);
    }
    if (pinned != loaded.end()) {
    } else if (provided != sources.end()) {
      result = ModuleSource{provided->first, provided->second};
    } else if (resolver) {
      result = resolver(import.source, unit.document.identity);
    }
    if (!result.ok()) {
      Problem(unit, "flow.name.module-not-found",
              std::string(result.status().message()), import.location);
      return;
    }
    if (result->identity.empty() ||
        (!utf8::IsValid(result->identity) || result->identity.size() > 4096)) {
      Problem(unit, "flow.form.import-context",
              "A module resolver must return a nonempty UTF-8 identity.",
              import.location);
      return;
    }
    auto existing = loaded.find(result->identity);
    size_t index;
    if (existing != loaded.end()) {
      index = existing->second;
    } else {
      if (units.size() >= limits.max_modules ||
          unit.depth >= limits.max_depth ||
          result->source.size() >
              limits.max_source_bytes -
                  std::min(bytes, limits.max_source_bytes)) {
        Problem(unit, "flow.form.import-context",
                "Module loading limit exceeded.", import.location);
        return;
      }
      auto next = std::make_unique<Unit>();
      next->names = names;
      next->document = *std::move(result);
      next->parsed = Parse(next->document.source);
      next->prefix = Prefix(next->document.identity);
      next->depth = unit.depth + 1;
      bytes += next->document.source.size();
      index = units.size();
      loaded.emplace(next->document.identity, index);
      units.push_back(std::move(next));
    }
    unit.dependencies.push_back(index);
    unit.import_targets.insert_or_assign(import.location.start, index);
    const auto bind = [&](const syntax::Word& name, std::string member) {
      Alias next{index, name.location.start, member, false, {}};
      if (!member.empty()) {
        next.contributions.emplace_back(index, member);
        const bool type = Defined(*units[index], member, true);
        const auto* declaration = DeclarationOf(*units[index], member, type);
        if (declaration == nullptr) {
          Problem(unit, "flow.name.unknown",
                  absl::StrCat("Module '", units[index]->document.identity,
                               "' has no symbol '", member, "'."),
                  name.location);
          return;
        }
        if (!Exported(*units[index], member, type)) {
          Problem(unit, "flow.name.private-member",
                  absl::StrCat("Module symbol '", member,
                               "' exists but is private. Mark its declaration "
                               "'exported' to import it."),
                  name.location);
          unit.parsed.diagnostics.back().related.push_back(
              {units[index]->document.identity,
               LineIndex(units[index]->document.source)
                   .Between(declaration->location.start,
                            declaration->location.end),
               "Private symbol declared here."});
        }
      }
      const auto [at, added] = aliases.emplace(name.text, next);
      if (added) {
        return;
      }
      auto& previous = at->second;
      if (previous.unit == index && previous.member == member) {
        Problem(unit, "flow.sequence.redundant-import",
                "This binding already imports the same symbol or namespace.",
                name.location, Severity::kInformation);
      } else if (!member.empty() && !previous.member.empty() &&
                 !Defined(*units[index], member, true) &&
                 !Defined(*units[previous.unit], previous.member, true)) {
        previous.contributions.emplace_back(index, member);
        const auto implemented = [&](const Alias& alias) {
          for (const auto& flow : units[alias.unit]->parsed.flows) {
            if (flow->name.text == alias.member && !flow->interface_only) {
              return true;
            }
          }
          return false;
        };
        if (!implemented(previous) && implemented(next)) {
          previous.unit = next.unit;
          previous.member = next.member;
        }
      } else {
        Problem(
            unit, "flow.name.import-alias-conflict",
            "This local name already binds a different symbol or namespace. "
            "Use 'as' to choose another name.",
            name.location);
      }
    };
    if (import.selective) {
      for (const auto& symbol : import.symbols) {
        if (!symbol.name.text.empty()) {
          bind(symbol.alias, symbol.name.text);
        }
      }
    } else {
      bind(import.alias, {});
    }
  };
  std::vector<absl::flat_hash_map<const syntax::FlowDeclaration*,
                                  std::shared_ptr<const Aliases>>>
      local;
  std::vector<std::tuple<syntax::DtoDeclaration*, size_t,
                         std::shared_ptr<const Aliases>>>
      local_types;
  for (size_t index = 0; index < units.size(); ++index) {
    Unit& unit = *units[index];
    for (const auto& import : unit.parsed.imports) {
      load(unit, import, unit.imports);
    }
    local.emplace_back();
    for (const auto& flow : unit.parsed.flows) {
      Aliases aliases = unit.imports;
      Aliases own;
      for (const auto& import : flow->imports) {
        if (!import.selective &&
            aliases.find(import.alias.text) != aliases.end()) {
          Problem(unit, "flow.name.import-shadowing",
                  "Flow-local namespace shadows a module namespace.",
                  import.location, Severity::kWarning);
        }
        load(unit, import, own);
      }
      for (const auto& [name, target] : own) {
        aliases.insert_or_assign(name, target);
      }
      const auto add_local = [&](auto& declaration, bool type) {
        const std::string visible = declaration->name.text;
        const auto prefix =
            unit.prefix.empty() ? Prefix(unit.document.identity) : unit.prefix;
        const size_t length = prefix.size() + visible.size() + 40;
        if (length > unit.names->remaining) {
          Problem(unit, "flow.form.import-context",
                  "Flow-local declarations exceed the linking memory budget.",
                  declaration->name.location);
          return;
        }
        unit.names->remaining -= length;
        declaration->name.text =
            absl::StrCat(prefix, "$scope_", flow->location.start, ".", visible);
        unit.scoped_names.insert(declaration->name.text);
        declaration->exported = false;
        std::string member = declaration->name.text;
        if (!type) {
          for (const auto& candidate : unit.parsed.flows) {
            if (candidate->name.text == visible && !candidate->interface_only) {
              member = visible;
              break;
            }
          }
        }
        const Alias target{
            index, declaration->name.location.start, member, true, {}};
        if (const auto found = own.find(visible); found != own.end()) {
          if (type || found->second.member.empty()) {
            Problem(unit, "flow.name.import-alias-conflict",
                    "This flow-local name already binds another symbol.",
                    declaration->name.location);
          }
        } else {
          aliases.insert_or_assign(visible, target);
        }
      };
      for (auto& declaration : flow->declarations) {
        declaration->binding_name = declaration->name.text;
        declaration->imported = true;
        add_local(declaration, false);
      }
      for (auto& declaration : flow->structs) {
        add_local(declaration, true);
      }
      local[index].emplace(flow.get(),
                           std::make_shared<const Aliases>(std::move(aliases)));
    }
    std::vector<syntax::FlowDeclarationPtr> declarations;
    for (auto& flow : unit.parsed.flows) {
      const auto aliases = local[index].at(flow.get());
      for (auto& declaration : flow->declarations) {
        local[index].emplace(declaration.get(), aliases);
        declarations.push_back(std::move(declaration));
      }
      flow->declarations.clear();
      for (auto& shape : flow->structs) {
        // Field types use the containing flow's lexical imports and structs.
        // Rewriting runs after every module has been loaded.
        local_types.emplace_back(shape.get(), index, aliases);
        unit.parsed.dtos.push_back(std::move(shape));
      }
      flow->structs.clear();
    }
    for (auto& declaration : declarations) {
      unit.parsed.flows.push_back(std::move(declaration));
    }
  }
  LinkedModule linked;
  const auto members = [&](const Aliases& aliases, size_t start, size_t end,
                           bool scoped) {
    for (const auto& [alias, index] : aliases) {
      const Unit& dependency = *units[index.unit];
      for (const auto& flow : dependency.parsed.flows) {
        if (!flow->entry &&
            ((index.local && flow->name.text == index.member) ||
             (flow->exported &&
              (index.member.empty() || flow->name.text == index.member)))) {
          linked.members.push_back(
              {index.member.empty() ? absl::StrCat(alias, ".", flow->name.text)
                                    : alias,
               Target(dependency, flow->name.text, false),
               dependency.document.identity, flow->name.location, start, end,
               scoped, false});
        }
      }
      for (const auto& dto : dependency.parsed.dtos) {
        if ((index.local && dto->name.text == index.member) ||
            (dto->exported &&
             (index.member.empty() || dto->name.text == index.member))) {
          linked.members.push_back(
              {index.member.empty() ? absl::StrCat(alias, ".", dto->name.text)
                                    : alias,
               Target(dependency, dto->name.text, true),
               dependency.document.identity, dto->name.location, start, end,
               scoped, true});
        }
      }
    }
  };
  members(units[0]->imports, 0, units[0]->document.source.size(), false);
  for (const auto& flow : units[0]->parsed.flows) {
    members(*local[0].at(flow.get()), flow->extent.start, flow->extent.end,
            true);
  }
  const auto selections = [&](const std::vector<syntax::Import>& imports,
                              bool scoped) {
    for (const auto& import : imports) {
      if (!import.selective) {
        continue;
      }
      size_t end = units[0]->document.source.find('\n', import.selection_start);
      if (end == std::string::npos) {
        end = units[0]->document.source.size();
      }
      linked.selections.push_back({import.selection_start, end});
      const auto dependency =
          units[0]->import_targets.find(import.location.start);
      if (dependency == units[0]->import_targets.end()) {
        continue;
      }
      Aliases available;
      const auto& source_unit = *units[dependency->second];
      for (const auto& flow : source_unit.parsed.flows) {
        if (flow->exported && !flow->entry) {
          available.try_emplace(flow->name.text, Alias{dependency->second,
                                                       import.location.start,
                                                       flow->name.text,
                                                       false,
                                                       {}});
        }
      }
      for (const auto& shape : source_unit.parsed.dtos) {
        if (shape->exported) {
          available.try_emplace(shape->name.text, Alias{dependency->second,
                                                        import.location.start,
                                                        shape->name.text,
                                                        false,
                                                        {}});
        }
      }
      const auto begin = linked.members.size();
      members(available, import.selection_start, end, scoped);
      for (size_t i = begin; i < linked.members.size(); ++i) {
        linked.members[i].selection = true;
      }
    }
  };
  selections(units[0]->parsed.imports, false);
  for (const auto& flow : units[0]->parsed.flows) {
    selections(flow->imports, true);
  }
  for (auto& unit : units) {
    unit->start = linked.source.size();
    linked.source += unit->document.source;
    linked.documents.push_back(
        {unit->document.identity, unit->start, linked.source.size()});
    linked.source += '\n';
  }
  const LineIndex lines(linked.source);
  // Resolve every name before mutating declaration names in any module.
  const bool root_imported =
      std::any_of(units.begin(), units.end(), [](const auto& unit) {
        return std::find(unit->dependencies.begin(), unit->dependencies.end(),
                         size_t{0}) != unit->dependencies.end();
      });
  for (size_t index = 0; index < units.size(); ++index) {
    Unit& unit = *units[index];
    const bool imported_interface = index != 0 || root_imported;
    for (auto& dto : unit.parsed.dtos) {
      for (auto& field : dto->fields) {
        if (imported_interface && dto->exported) {
          CheckPublicType(field->type, unit);
        }
        const Aliases* aliases = &unit.imports;
        for (const auto& [shape, owner, scope] : local_types) {
          if (shape == dto.get()) {
            aliases = scope.get();
            break;
          }
        }
        RewriteType(field->type, unit, *aliases, units);
      }
    }
    for (auto& flow : unit.parsed.flows) {
      const Aliases& aliases = *local[index].at(flow.get());
      syntax::VisitMutableSubtree(*flow, [&](Node& node) {
        if (auto* c = syntax::As<syntax::CallExpression>(&node); c != nullptr) {
          c->action = ResolveName(c->action, false, unit, aliases, units,
                                  c->action_location);
        }
        if (auto* p = syntax::As<syntax::PortDeclaration>(&node);
            p != nullptr) {
          if (imported_interface && flow->exported) {
            CheckPublicType(p->type, unit);
            const auto check_local =
                [&](const auto& self,
                    const syntax::TypeExpression& type) -> void {
              const auto found = aliases.find(type.name);
              if (!type.quoted && found != aliases.end() &&
                  found->second.local &&
                  Defined(unit, found->second.member, true)) {
                Problem(unit, "flow.form.private-type-exposed",
                        "An exported interface names a flow-local struct. Move "
                        "the struct to module scope and mark it 'exported'.",
                        type.location);
              }
              for (const auto& parameter : type.parameters) {
                self(self, parameter);
              }
            };
            check_local(check_local, p->type);
          }
          RewriteType(p->type, unit, aliases, units);
        }
        if (auto* v = syntax::As<syntax::TypedValue>(&node); v != nullptr) {
          RewriteType(v->type, unit, aliases, units);
        }
      });
    }
  }
  const auto evidence = [&](size_t root) {
    std::vector<size_t> pending{root};
    absl::flat_hash_set<size_t> seen;
    while (!pending.empty()) {
      const size_t index = pending.back();
      pending.pop_back();
      if (!seen.insert(index).second) {
        continue;
      }
      for (const auto& flow : units[index]->parsed.flows) {
        if (flow->interface_only) {
          return true;
        }
      }
      for (const auto next : units[index]->dependencies) {
        pending.push_back(next);
      }
    }
    return false;
  };
  for (size_t i = 0; i < units.size(); ++i) {
    Unit& unit = *units[i];
    const auto unused = [&](const syntax::Import& import,
                            const Aliases& aliases) {
      if (import.selective) {
        for (const auto& symbol : import.symbols) {
          if (!unit.used_imports.contains(symbol.alias.location.start)) {
            // Contracts are checked even if the imported action is not run.
            const auto binding = aliases.find(symbol.alias.text);
            if (binding != aliases.end() &&
                !unit.used_imports.contains(binding->second.location) &&
                binding->second.contributions.size() <= 1 &&
                !evidence(binding->second.unit)) {
              Problem(unit, "flow.unused.import",
                      "This imported symbol is not referenced.",
                      symbol.alias.location, Severity::kWeakWarning);
            }
          }
        }
        return;
      }
      const auto binding = aliases.find(import.alias.text);
      if (binding == aliases.end() ||
          binding->second.location != import.alias.location.start ||
          unit.used_imports.contains(import.alias.location.start) ||
          evidence(binding->second.unit)) {
        return;
      }
      Problem(
          unit, "flow.unused.import",
          "This import contributes no referenced symbol or callable contract.",
          import.alias.location, Severity::kWeakWarning);
    };
    for (const auto& import : unit.parsed.imports) {
      unused(import, unit.imports);
    }
    for (const auto& flow : unit.parsed.flows) {
      for (const auto& import : flow->imports) {
        unused(import, *local[i].at(flow.get()));
      }
    }
  }

  struct Contribution {
    const syntax::FlowDeclaration* declaration;
    std::string target;
    size_t unit;
  };

  std::vector<Contribution> contributions;
  absl::flat_hash_set<std::pair<const syntax::FlowDeclaration*, std::string>>
      scheduled;
  bool contribution_limit = false;
  const auto collect = [&](size_t index, const Aliases& aliases,
                           const syntax::FlowDeclaration* owner) {
    Unit& unit = *units[index];
    if (contribution_limit) {
      return;
    }
    for (const auto& [name, alias] : aliases) {
      if (alias.member.empty() ||
          Defined(*units[alias.unit], alias.member, true)) {
        continue;
      }
      const std::string target =
          Target(*units[alias.unit], alias.member, false);
      const auto add = [&](const syntax::FlowDeclaration* declaration,
                           size_t source_index) {
        if (contribution_limit) {
          return;
        }
        if (contributions.size() >= 65536) {
          contribution_limit = true;
          Problem(unit, "flow.form.import-context",
                  "Callable contract linking exceeds 65536 contributions.",
                  declaration->name.location);
          return;
        }
        if (Target(*units[source_index], declaration->name.text, false) !=
                target &&
            scheduled.emplace(declaration, target).second) {
          contributions.push_back({declaration, target, source_index});
        }
      };
      for (const auto& [source_index, member] : alias.contributions) {
        for (const auto& declaration : units[source_index]->parsed.flows) {
          if (declaration->name.text == member) {
            add(declaration.get(), source_index);
          }
        }
      }
      for (const auto& declaration : unit.parsed.flows) {
        if (declaration->name.text == name && !declaration->entry) {
          add(declaration.get(), index);
        }
      }
      if (owner != nullptr) {
        const auto scoped = local[index].find(owner);
        if (scoped != local[index].end()) {
          for (const auto& declaration : unit.parsed.flows) {
            if (declaration->interface_only && declaration->imported &&
                declaration->binding_name == name &&
                owner->extent.start <= declaration->extent.start &&
                declaration->extent.end <= owner->extent.end) {
              add(declaration.get(), index);
            }
          }
        }
      }
    }
  };
  for (size_t index = 0; index < units.size(); ++index) {
    collect(index, units[index]->imports, nullptr);
    for (const auto& flow : units[index]->parsed.flows) {
      collect(index, *local[index].at(flow.get()), flow.get());
    }
  }
  for (auto& unit : units) {
    absl::flat_hash_map<std::string, std::string> targets;
    for (const auto& flow : unit->parsed.flows) {
      targets.try_emplace(flow->name.text,
                          Target(*unit, flow->name.text, false));
    }
    for (auto& flow : unit->parsed.flows) {
      if (flow->binding_name.empty()) {
        flow->binding_name = flow->name.text;
      }
      if (!flow->entry) {
        flow->name.text = targets.at(flow->name.text);
      }
      flow->module = unit->document.identity;
      flow->imported |= unit->start != 0;
    }
    for (auto& dto : unit->parsed.dtos) {
      dto->name.text = Target(*unit, dto->name.text, true);
      dto->module = unit->document.identity;
    }
    for (auto& d : unit->parsed.diagnostics) {
      if (d.source.empty()) {
        d.source = unit->document.identity;
      }
      linked.parsed.diagnostics.push_back(std::move(d));
    }
    for (auto& flow : unit->parsed.flows) {
      if (unit->start != 0 && flow->entry) {
        continue;
      }
      ShiftTree(*flow, unit->start, lines);
      linked.parsed.flows.push_back(std::move(flow));
    }
    for (auto& dto : unit->parsed.dtos) {
      ShiftTree(*dto, unit->start, lines);
      linked.parsed.dtos.push_back(std::move(dto));
    }
    for (size_t brace : unit->parsed.value_braces) {
      linked.parsed.value_braces.push_back(brace + unit->start);
    }
    for (size_t brace : unit->parsed.tagged_braces) {
      linked.parsed.tagged_braces.push_back(brace + unit->start);
    }
  }
  absl::flat_hash_set<std::pair<const syntax::FlowDeclaration*, std::string>>
      copied;
  for (const auto& contribution : contributions) {
    if (!copied.emplace(contribution.declaration, contribution.target).second) {
      continue;
    }
    const auto& original = *contribution.declaration;
    auto copy = std::make_unique<syntax::FlowDeclaration>();
    copy->name = original.name;
    copy->name.text = contribution.target;
    copy->location = original.location;
    copy->extent = original.extent;
    copy->interface_only = original.interface_only;
    copy->imported = true;
    copy->module = original.module;
    copy->binding_name = original.binding_name;
    copy->description = original.description;
    for (const auto& port : original.ports) {
      copy->ports.push_back(std::make_unique<syntax::PortDeclaration>(*port));
    }
    for (const auto& header : original.headers) {
      copy->headers.push_back(
          std::make_unique<syntax::HeaderDeclaration>(*header));
    }
    linked.parsed.flows.push_back(std::move(copy));
  }
  for (auto& diagnostic : names->diagnostics) {
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

catalogue::Catalogue LinkedModule::CatalogueAt(size_t offset,
                                               bool import_selection) const {
  const auto resolved = Resolve(source, parsed);
  const bool scoped = std::any_of(
      parsed.flows.begin(), parsed.flows.end(), [&](const auto& flow) {
        return !flow->imported && flow->extent.start <= offset &&
               offset <= flow->extent.end;
      });
  const bool selecting = import_selection && SelectingImport(offset);
  const auto visible_type = [&](std::string text) {
    std::vector<std::pair<std::string, std::string>> names;
    for (const auto& member : members) {
      if (member.selection == selecting && member.type &&
          member.scoped == scoped && member.scope_start <= offset &&
          offset <= member.scope_end) {
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
  absl::flat_hash_set<std::pair<std::string, bool>> offered;
  for (const auto& member : members) {
    if (member.selection != selecting || member.scoped != scoped ||
        offset < member.scope_start || offset > member.scope_end ||
        !offered.emplace(member.name, member.type).second) {
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
  return catalogue::Catalogue::Of(std::move(actions), std::move(types));
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
