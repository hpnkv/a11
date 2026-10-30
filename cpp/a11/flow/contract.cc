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

#include "a11/flow/contract.h"

#include <algorithm>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_replace.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/flow/emit_json.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow {
absl::StatusOr<PipelinePorts> SelectPipelinePorts(
    const FlowPlan& flow, absl::Span<const std::string> supplied) {
  const PortPlan* input = nullptr;
  const PortPlan* output = nullptr;
  size_t inputs = 0;
  size_t outputs = 0;
  for (const auto& port : flow.ports) {
    if (port.direction == syntax::PortDirection::kOutput) {
      output = &port;
      ++outputs;
    } else if (std::find(supplied.begin(), supplied.end(), port.name) ==
               supplied.end()) {
      input = &port;
      ++inputs;
    }
  }
  if (inputs != 1 || outputs != 1) {
    return absl::InvalidArgumentError(
        absl::StrCat("A pipeline invocation requires exactly one unsupplied "
                     "input (including optional inputs) and one output; found ",
                     inputs, " unsupplied inputs and ", outputs,
                     " outputs. Supply configuration inputs explicitly or use "
                     "an adapter flow."));
  }
  return PipelinePorts{input, output};
}

namespace {
std::string Normal(std::string_view name) {
  std::string decoded;
  if (!name.empty() && name.front() == '"') {
    const auto quoted = nlohmann::json::parse(name, nullptr, false);
    if (quoted.is_string()) {
      decoded = quoted.get<std::string>();
      name = decoded;
    }
  }
  if (name.starts_with("std::")) {
    name.remove_prefix(5);
  }
  const auto canonical = vocabulary::Canonical(name);
  if (vocabulary::TypeNames().contains(canonical)) {
    name = canonical;
  }
  if (name == "str" || name == "text") {
    return "string";
  }
  if (name == "int") {
    return "integer";
  }
  if (name == "float") {
    return "number";
  }
  if (name == "bool") {
    return "boolean";
  }
  if (name == "dict") {
    return "object";
  }
  if (name == "array") {
    return "list";
  }
  if (name == "application/json") {
    return "json";
  }
  if (name == "a11.Time" || name == "a11::Time") {
    return "time";
  }
  if (name == "a11.Duration" || name == "a11::Duration") {
    return "duration";
  }
  return std::string(name);
}

std::pair<std::string, std::string> Container(std::string_view text) {
  if (text.size() > 2 && text.substr(text.size() - 2) == "[]") {
    return {"list", std::string(text.substr(0, text.size() - 2))};
  }
  const size_t bracket = text.find('[');
  if (bracket == std::string_view::npos || text.back() != ']') {
    return {Normal(text), ""};
  }
  return {Normal(text.substr(0, bracket)),
          std::string(text.substr(bracket + 1, text.size() - bracket - 2))};
}

std::vector<std::string> Parameters(std::string_view text) {
  std::vector<std::string> result;
  size_t start = 0;
  int depth = 0;
  for (size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '[') {
      ++depth;
    }
    if (text[index] == ']') {
      --depth;
    }
    if (text[index] == ',' && depth == 0) {
      result.emplace_back(
          absl::StripAsciiWhitespace(text.substr(start, index - start)));
      start = index + 1;
    }
  }
  result.emplace_back(absl::StripAsciiWhitespace(text.substr(start)));
  return result;
}

std::string Canonical(std::string_view type, int depth) {
  if (depth > 128) {
    return std::string(type);
  }
  const auto [base, element] = Container(type);
  if (element.empty()) {
    return base;
  }
  auto parameters = Parameters(element);
  std::string output = base + "[";
  for (size_t index = 0; index < parameters.size(); ++index) {
    if (index != 0) {
      output += ',';
    }
    output += Canonical(parameters[index], depth + 1);
  }
  return output + "]";
}

std::optional<int> CompareBounds(const syntax::Constant& a,
                                 const syntax::Constant& b) {
  const auto numeric = [](auto kind) {
    return kind == syntax::Constant::Kind::kInteger ||
           kind == syntax::Constant::Kind::kDouble;
  };
  if (a.kind == syntax::Constant::Kind::kInteger && b.kind == a.kind) {
    return a.integer < b.integer ? -1 : a.integer > b.integer ? 1 : 0;
  }
  if (numeric(a.kind) && numeric(b.kind) && (a.kind != b.kind) &&
      ((a.kind == syntax::Constant::Kind::kInteger &&
        (a.integer > 9007199254740992LL || a.integer < -9007199254740992LL)) ||
       (b.kind == syntax::Constant::Kind::kInteger &&
        (b.integer > 9007199254740992LL || b.integer < -9007199254740992LL)))) {
    return std::nullopt;
  }
  if (numeric(a.kind) && numeric(b.kind)) {
    return a.AsDouble() < b.AsDouble()   ? -1
           : a.AsDouble() > b.AsDouble() ? 1
                                         : 0;
  }
  if (a.kind == syntax::Constant::Kind::kDuration && b.kind == a.kind) {
    return a.duration < b.duration ? -1 : a.duration > b.duration ? 1 : 0;
  }
  return std::nullopt;
}

Refinement Refines(std::string_view actual, std::string_view expected,
                   const Program& program,
                   absl::flat_hash_set<std::string>& visiting, int depth) {
  if (depth > 128) {
    return Refinement::kUnknown;
  }
  const auto [a, ae] = Container(actual);
  const auto [e, ee] = Container(expected);
  if (a == e) {
    if (ee.empty()) {
      return Refinement::kYes;
    }
    if (ae.empty()) {
      return Refinement::kNo;
    }
    auto actual_parameters = Parameters(ae),
         expected_parameters = Parameters(ee);
    if (actual_parameters.size() != expected_parameters.size()) {
      return Refinement::kUnknown;
    }
    Refinement result = Refinement::kYes;
    for (size_t index = 0; index < actual_parameters.size(); ++index) {
      const auto nested =
          Refines(actual_parameters[index], expected_parameters[index], program,
                  visiting, depth + 1);
      if (nested == Refinement::kNo) {
        return nested;
      }
      if (nested == Refinement::kUnknown) {
        result = nested;
      }
    }
    return result;
  }
  if (e == "any" || (a == "integer" && e == "number")) {
    return Refinement::kYes;
  }
  const DtoPlan* shape = program.Dto(a);
  if (e == "object" && shape != nullptr) {
    return Refinement::kYes;
  }
  if (e == "json" && (a == "object" || a == "list" || a == "string" ||
                      a == "integer" || a == "number" || a == "boolean" ||
                      a == "null" || (shape != nullptr && !shape->binary))) {
    return Refinement::kYes;
  }
  const DtoPlan* base = program.Dto(e);
  if (a == "any" && e != "any") {
    return Refinement::kNo;
  }
  if (base != nullptr && shape == nullptr &&
      (a == "object" || a == "json" || a == "list" || a == "string" ||
       a == "integer" || a == "number" || a == "boolean" || a == "null" ||
       a == "bytes" || a == "time" || a == "duration")) {
    return Refinement::kNo;
  }
  if (shape != nullptr && base == nullptr &&
      (e == "list" || e == "string" || e == "integer" || e == "number" ||
       e == "boolean" || e == "null" || e == "bytes" || e == "time" ||
       e == "duration")) {
    return Refinement::kNo;
  }
  if (shape != nullptr && base != nullptr) {
    const std::string key = absl::StrCat(a, ":", e);
    if (!visiting.insert(key).second) {
      return Refinement::kYes;
    }
    Refinement result = Refinement::kYes;
    for (const FieldPlan& field : base->fields) {
      const FieldPlan* found = shape->Field(field.name);
      if (found == nullptr) {
        if (field.required) {
          result = Refinement::kNo;
          break;
        }
        continue;
      }
      if (field.required && !found->required) {
        result = Refinement::kNo;
        break;
      }
      const Refinement nested = Refines(found->declared, field.declared,
                                        program, visiting, depth + 1);
      if (nested == Refinement::kNo) {
        result = nested;
        break;
      }
      if (nested == Refinement::kUnknown) {
        result = nested;
      }
      const auto bound = [&](bool required, const syntax::Constant& expected,
                             bool available, const syntax::Constant& actual,
                             bool lower) {
        if (!required) {
          return Refinement::kYes;
        }
        if (!available) {
          return Refinement::kNo;
        }
        if (const auto order = CompareBounds(actual, expected);
            order.has_value()) {
          return (lower ? *order >= 0 : *order <= 0) ? Refinement::kYes
                                                     : Refinement::kNo;
        }
        return ConstantToJsonValue(expected) == ConstantToJsonValue(actual)
                   ? Refinement::kYes
                   : Refinement::kUnknown;
      };
      const auto low =
          bound(field.range.has_minimum, field.range.minimum,
                found->range.has_minimum, found->range.minimum, true);
      const auto high =
          bound(field.range.has_maximum, field.range.maximum,
                found->range.has_maximum, found->range.maximum, false);
      if (low == Refinement::kNo || high == Refinement::kNo ||
          (field.unique && !found->unique)) {
        result = Refinement::kNo;
        break;
      }
      if (low == Refinement::kUnknown || high == Refinement::kUnknown) {
        result = Refinement::kUnknown;
      }
      if (field.has_enumeration) {
        if (!found->has_enumeration) {
          result = Refinement::kNo;
          break;
        }
        for (const auto& item : found->enumeration) {
          bool present = false;
          for (const auto& accepted : field.enumeration) {
            if (ConstantToJsonValue(item) == ConstantToJsonValue(accepted)) {
              present = true;
            }
          }
          if (!present) {
            result = Refinement::kNo;
            break;
          }
        }
        if (result == Refinement::kNo) {
          break;
        }
      }
      if (field.has_pattern &&
          (!found->has_pattern || field.pattern != found->pattern)) {
        result = Refinement::kUnknown;
      }
    }
    visiting.erase(key);
    return result;
  }
  const absl::flat_hash_set<std::string> builtins{
      "any",    "json",    "object", "list",  "string", "integer",
      "number", "boolean", "null",   "bytes", "time",   "duration"};
  return builtins.find(a) != builtins.end() &&
                 builtins.find(e) != builtins.end()
             ? Refinement::kNo
             : Refinement::kUnknown;
}

absl::StatusOr<DtoPlan> MeetShapes(const DtoPlan& left, const DtoPlan& right,
                                   const Program& program) {
  DtoPlan result = left;
  result.name.clear();
  result.description.clear();
  result.binary = left.binary || right.binary;
  for (const auto& field : right.fields) {
    auto found = std::find_if(
        result.fields.begin(), result.fields.end(),
        [&](const auto& existing) { return existing.name == field.name; });
    if (found == result.fields.end()) {
      result.fields.push_back(field);
      continue;
    }
    const bool required = found->required || field.required;
    const auto forward = RefinesType(field.declared, found->declared, program);
    const auto inverse = RefinesType(found->declared, field.declared, program);
    if (forward != Refinement::kYes && inverse != Refinement::kYes) {
      return (forward == Refinement::kUnknown ||
              inverse == Refinement::kUnknown ||
              (program.Dto(field.type) != nullptr &&
               program.Dto(found->type) != nullptr))
                 ? absl::UnavailableError(
                       "Nested payload intersection needs additional "
                       "evidence.")
                 : absl::FailedPreconditionError(
                       "Field payload constraints conflict.");
    }
    FieldPlan merged = inverse == Refinement::kYes ? *found : field;
    merged.required = required;
    if (!found->description.empty() && !field.description.empty()) {
      merged.description = std::min(found->description, field.description);
    } else {
      merged.description =
          found->description.empty() ? field.description : found->description;
    }
    merged.unique = found->unique || field.unique;
    for (bool lower : {true, false}) {
      const bool first =
          lower ? found->range.has_minimum : found->range.has_maximum;
      const bool second =
          lower ? field.range.has_minimum : field.range.has_maximum;
      const auto& a = lower ? found->range.minimum : found->range.maximum;
      const auto& b = lower ? field.range.minimum : field.range.maximum;
      const auto order = CompareBounds(a, b);
      if (first && second && !order.has_value()) {
        return absl::UnavailableError(
            "A field-bound intersection cannot be established.");
      }
      const auto selected = !first                                ? b
                            : !second                             ? a
                            : (lower ? *order >= 0 : *order <= 0) ? a
                                                                  : b;
      if (lower) {
        merged.range.has_minimum = first || second;
        merged.range.minimum = selected;
      } else {
        merged.range.has_maximum = first || second;
        merged.range.maximum = selected;
      }
    }
    if (merged.range.has_minimum && merged.range.has_maximum &&
        CompareBounds(merged.range.minimum, merged.range.maximum).value_or(0) >
            0) {
      return absl::FailedPreconditionError(
          "Field bounds have an empty intersection.");
    }
    if (found->has_pattern && field.has_pattern &&
        found->pattern != field.pattern) {
      return absl::UnavailableError(
          "Pattern intersection requires additional evidence.");
    }
    if (found->has_pattern) {
      merged.pattern = found->pattern;
      merged.has_pattern = true;
    }
    if (field.has_pattern) {
      merged.pattern = field.pattern;
      merged.has_pattern = true;
    }
    if (found->has_enumeration && field.has_enumeration) {
      merged.enumeration.clear();
      merged.has_enumeration = true;
      for (const auto& a : found->enumeration) {
        for (const auto& b : field.enumeration) {
          if (ConstantToJsonValue(a) == ConstantToJsonValue(b)) {
            merged.enumeration.push_back(a);
            break;
          }
        }
      }
      if (merged.enumeration.empty() && required) {
        return absl::FailedPreconditionError(
            "Required field enumerations have an empty intersection.");
      }
    } else if (found->has_enumeration) {
      merged.enumeration = found->enumeration;
      merged.has_enumeration = true;
    } else if (field.has_enumeration) {
      merged.enumeration = field.enumeration;
      merged.has_enumeration = true;
    }
    *found = std::move(merged);
  }
  std::sort(result.fields.begin(), result.fields.end(),
            [](const auto& a, const auto& b) { return a.name < b.name; });
  return result;
}

std::string Type(const PortPlan& p) {
  return p.declared.empty() ? p.type : p.declared;
}

absl::Status Mismatch(const FlowPlan& flow, std::string_view member,
                      std::string_view why) {
  return absl::FailedPreconditionError(absl::StrCat(
      "Schema of '", flow.binding_name.empty() ? flow.name : flow.binding_name,
      "', ", member, ": ", why, "."));
}
}  // namespace

std::vector<std::string_view> FieldTypeNames(const FieldPlan& field) {
  std::vector<std::string_view> names{field.type, field.element, field.dto_name,
                                      field.element_dto_name};
  if (field.syntax_type.has_value()) {
    std::vector<const syntax::TypeExpression*> pending{&*field.syntax_type};
    while (!pending.empty()) {
      const auto* type = pending.back();
      pending.pop_back();
      names.push_back(type->name);
      for (const auto& parameter : type->parameters) {
        pending.push_back(&parameter);
      }
    }
  }
  return names;
}

void MarkBinaryTypes(std::vector<DtoPlan>& types, CompilationBudget* budget) {
  CompilationBudget local_budget(CompilationLimits{});
  if (budget == nullptr) {
    budget = &local_budget;
  }
  absl::flat_hash_map<std::string_view, size_t> names;
  for (size_t index = 0; index < types.size(); ++index) {
    names[types[index].name] = index;
  }
  std::vector<std::vector<size_t>> dependents(types.size());
  std::vector<size_t> ready;
  for (size_t index = 0; index < types.size(); ++index) {
    for (const auto& field : types[index].fields) {
      if (!budget->Check()) {
        return;
      }
      for (const auto name : FieldTypeNames(field)) {
        if (name == "bytes") {
          types[index].binary = true;
        }
        const auto found = names.find(name);
        if (found != names.end()) {
          dependents[found->second].push_back(index);
        }
      }
    }
    if (types[index].binary) {
      ready.push_back(index);
    }
  }
  for (size_t next = 0; next < ready.size(); ++next) {
    if (!budget->Check()) {
      return;
    }
    for (const auto index : dependents[ready[next]]) {
      if (!types[index].binary) {
        types[index].binary = true;
        ready.push_back(index);
      }
    }
  }
}

std::string CanonicalPayloadType(std::string_view type) {
  return Canonical(type, 0);
}

Refinement RefinesType(std::string_view actual, std::string_view expected,
                       const Program& program) {
  absl::flat_hash_set<std::string> visiting;
  return Refines(actual, expected, program, visiting, 0);
}

std::string HeaderBytes(const syntax::Constant& v) {
  if (v.kind == syntax::Constant::Kind::kString) {
    return v.text;
  }
  if (v.kind == syntax::Constant::Kind::kNull) {
    return "";
  }
  if (v.kind == syntax::Constant::Kind::kDuration) {
    return absl::FormatDuration(v.duration);
  }
  return ConstantToJsonValue(v).dump();
}

absl::Status MergeContract(FlowPlan& effective, const FlowPlan& other,
                           const Program& program) {
  FlowPlan merged = effective;
  bool unknown = false;
  for (const PortPlan& p : other.ports) {
    auto found = std::find_if(
        merged.ports.begin(), merged.ports.end(), [&](const PortPlan& q) {
          return p.name == q.name && p.direction == q.direction;
        });
    if (found == merged.ports.end()) {
      merged.ports.push_back(p);
      continue;
    }
    if (found->unary != p.unary) {
      return Mismatch(other, p.name, "unary/stream contracts differ");
    }
    const bool required = found->required || p.required;
    const auto description = found->description.empty() ? p.description
                             : p.description.empty()
                                 ? found->description
                                 : std::min(found->description, p.description);
    std::vector<std::string> constraints = found->constraints;
    constraints.insert(constraints.end(), p.constraints.begin(),
                       p.constraints.end());
    const std::string previous = Type(*found);
    const DtoPlan* a_shape = found->analysis_shape != nullptr
                                 ? found->analysis_shape.get()
                                 : program.Dto(previous);
    const DtoPlan* b_shape = p.analysis_shape != nullptr
                                 ? p.analysis_shape.get()
                                 : program.Dto(Type(p));
    const Refinement r = RefinesType(Type(p), previous, program);
    const Refinement inverse = RefinesType(previous, Type(p), program);
    bool composite = false;
    if (a_shape != nullptr && b_shape != nullptr &&
        ((!constraints.empty()) ||
         (r != Refinement::kYes && inverse != Refinement::kYes))) {
      auto meet = MeetShapes(*a_shape, *b_shape, program);
      if (meet.ok()) {
        found->analysis_shape = std::make_shared<DtoPlan>(*std::move(meet));
        composite = true;
      } else if (!absl::IsUnavailable(meet.status())) {
        return meet.status();
      } else {
        unknown = true;
      }
      constraints.push_back(previous);
      constraints.push_back(Type(p));
    } else if (a_shape != nullptr && !constraints.empty() &&
               (Type(p) == "object" || Type(p) == "any" ||
                (Type(p) == "json" && !a_shape->binary))) {
      composite = true;
    } else if (r == Refinement::kYes &&
               (inverse != Refinement::kYes || Type(p) < previous)) {
      *found = p;
    } else if (inverse != Refinement::kYes) {
      if (r == Refinement::kNo && inverse == Refinement::kNo) {
        return Mismatch(other, p.name, "payload contracts conflict");
      }
      constraints.push_back(previous);
      constraints.push_back(Type(p));
      found->type = "any";
      found->declared = "any";
      unknown = true;
    }
    if (!constraints.empty()) {
      if (!composite && found->analysis_shape == nullptr) {
        unknown = true;
      }
      if (found->declared != "any") {
        constraints.push_back(Type(*found));
      }
      constraints.erase(
          std::remove(constraints.begin(), constraints.end(), "any"),
          constraints.end());
      std::sort(constraints.begin(), constraints.end());
      constraints.erase(std::unique(constraints.begin(), constraints.end()),
                        constraints.end());
      found->constraints = std::move(constraints);
      found->type = "any";
      found->declared = "any";
      for (const auto& candidate : found->constraints) {
        bool narrowest = true;
        for (const auto& requirement : found->constraints) {
          if (RefinesType(candidate, requirement, program) !=
              Refinement::kYes) {
            narrowest = false;
            break;
          }
        }
        if (narrowest) {
          found->declared = candidate;
          found->type = Container(candidate).first;
          if (candidate == Type(p)) {
            found->type = p.type;
          }
          if (const auto* shape = program.Dto(candidate); shape != nullptr) {
            found->analysis_shape = std::make_shared<DtoPlan>(*shape);
          }
          break;
        }
      }
    }
    found->required = required;
    found->description = description;
  }
  for (const HeaderPlan& h : other.headers) {
    auto found =
        std::find_if(merged.headers.begin(), merged.headers.end(),
                     [&](const HeaderPlan& q) { return h.name == q.name; });
    if (found == merged.headers.end()) {
      merged.headers.push_back(h);
    } else {
      if (h.has_default && found->has_default &&
          HeaderBytes(found->default_value) != HeaderBytes(h.default_value)) {
        return Mismatch(other, h.name, "header defaults conflict");
      }
      const auto description =
          found->description.empty() ? h.description
          : h.description.empty() ? found->description
                                  : std::min(found->description, h.description);
      const auto alias = found->alias.empty() ? h.alias
                         : h.alias.empty()    ? found->alias
                                              : std::min(found->alias, h.alias);
      if (h.has_default && !found->has_default) {
        found->default_value = h.default_value;
        found->has_default = true;
      }
      found->description = description;
      found->alias = alias;
    }
  }
  if (merged.description.empty() ||
      (!other.description.empty() && other.description < merged.description)) {
    merged.description = other.description;
  }
  merged.exported = merged.exported || other.exported;
  effective = std::move(merged);
  return unknown
             ? absl::UnavailableError(
                   "Payload intersection requires additional schema evidence.")
             : absl::OkStatus();
}

absl::Status CheckImplementation(const FlowPlan& actual,
                                 const FlowPlan& declared,
                                 const Program& program) {
  for (const PortPlan& p : declared.ports) {
    const PortPlan* q = actual.Port(p.name, p.direction);
    if (q == nullptr) {
      return Mismatch(actual, p.name, "declared port is missing");
    }
    if (q->unary != p.unary) {
      return Mismatch(actual, p.name, "unary/stream contracts differ");
    }
    if (p.required && !q->required) {
      return Mismatch(actual, p.name, "implementation weakens requiredness");
    }
    const Refinement r = RefinesType(Type(*q), Type(p), program);
    if (r == Refinement::kUnknown) {
      return absl::UnavailableError(
          absl::StrCat("Schema refinement of '", actual.name, "', port '",
                       p.name, "' cannot be established."));
    }
    if (r != Refinement::kYes) {
      return Mismatch(
          actual, p.name,
          r == Refinement::kUnknown
              ? "type refinement cannot be established"
              : "implementation widens or changes the payload type");
    }
  }
  for (const HeaderPlan& h : declared.headers) {
    const auto q =
        std::find_if(actual.headers.begin(), actual.headers.end(),
                     [&](const HeaderPlan& x) { return x.name == h.name; });
    if (q == actual.headers.end()) {
      return Mismatch(actual, h.name, "declared header is missing");
    }
    if (h.has_default &&
        (!q->has_default ||
         HeaderBytes(h.default_value) != HeaderBytes(q->default_value))) {
      return Mismatch(actual, h.name,
                      "implementation changes the header default");
    }
  }
  return absl::OkStatus();
}
}  // namespace a11::flow
