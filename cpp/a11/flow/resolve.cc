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

#include "a11/flow/resolve.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/strings/ascii.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/strings/str_replace.h>
#include <absl/time/time.h>
#include <absl/types/span.h>
#include <cmath>
#include <nlohmann/json.hpp>

#include "a11/flow/contract.h"
#include "a11/flow/diagnostic.h"
#include "a11/flow/emit_json.h"
#include "a11/flow/pattern.h"
#include "a11/flow/plan.h"
#include "a11/flow/syntax.h"
#include "a11/flow/temporal.h"
#include "a11/flow/vocabulary.h"
#include "absl/strings/match.h"

namespace a11::flow {
namespace {

using syntax::Location;
using syntax::Node;
using syntax::NodeKind;

constexpr size_t kNoSymbol = static_cast<size_t>(-1);

std::string Quoted(std::string_view text) {
  return absl::StrCat("'", text, "'");
}

/// The shapes a file declares, by name.
using DtoNames = absl::flat_hash_set<std::string>;

/// What a written type means, in the one place that decides it.
///
/// A port and a `struct` field both name a type, and they have to agree about
/// what a name means or a field could hold something its port could not carry.
/// The order is the whole of the rule: a **built-in** name first, so no
/// declaration can redefine `string`; then a **shape this file declares**,
/// which is what makes a `struct` outrank the registry; then a dotted
/// **registry tag** or a mimetype, which only the host can check.
class TypeReader {
 public:
  /// Where a diagnostic goes. A `std::function` rather than a template because
  /// the two callers report through different objects and this is called once
  /// per declaration, not once per value.
  using Reporter = std::function<void(
      std::string_view code, std::string message, const Location& location)>;

  TypeReader(const DtoNames& dtos, Reporter report)
      : dtos_(dtos), report_(std::move(report)) {}

  /// What the type *is*, as a plan records it.
  std::string Read(const syntax::TypeExpression& type) {
    if (type.name == "stream") {
      report_("flow.form.unknown-type",
              "A stream is a live binding category, not a serializable payload "
              "type. Use a port's stream modifier or a std::stream[T] binding.",
              type.location);
    }
    if (type.quoted) {
      CheckParameters(type, {0});
      return type.name;
    }
    const std::string& declared = type.name;
    if (vocabulary::TypeNames().contains(declared)) {
      const absl::Span<const int> allowed =
          vocabulary::TypeParameters(declared);
      CheckParameters(type, std::vector<int>(allowed.begin(), allowed.end()));
      return declared;
    }
    // A shape is written as it was declared, case and all: `struct` names are
    // not keywords and `Canonical` would fold a shouted one into something
    // else.
    if (dtos_.contains(type.name)) {
      CheckParameters(type, {0});
      return type.name;
    }
    if (absl::StrContains(type.name, '/')) {
      CheckParameters(type, {0});
      return type.name;
    }
    // A dotted name is the tag a serialisation registry knows a type by. An
    // undotted one is nothing the language knows, and is far more often a
    // misspelt built-in than a tag somebody meant.
    if (absl::StrContains(type.name, '.')) {
      CheckParameters(type, {0});
      return type.name;
    }
    report_("flow.form.unknown-type",
            absl::StrCat("Unknown type ", Quoted(type.name),
                         " (known: ", absl::StrJoin(Known(), ", "),
                         ", a shape this file declares, a serialisation tag "
                         "like 'a11.sdk.AudioBuffer', or a quoted mimetype)."),
            type.location);
    return type.name;
  }

  /// The shape `type` names, or empty where it names something else.
  [[nodiscard]] std::string DtoOf(const syntax::TypeExpression& type) const {
    if (type.quoted) {
      return "";
    }
    if (vocabulary::TypeNames().contains(vocabulary::Canonical(type.name))) {
      return "";
    }
    return dtos_.contains(type.name) ? type.name : "";
  }

 private:
  [[nodiscard]] std::vector<std::string> Known() const {
    std::vector<std::string> known;
    for (const std::string_view name : vocabulary::TypeNames()) {
      known.emplace_back(name);
    }
    std::sort(known.begin(), known.end());
    return known;
  }

  void CheckParameters(const syntax::TypeExpression& type,
                       const std::vector<int>& allowed) {
    for (const syntax::TypeExpression& parameter : type.parameters) {
      Read(parameter);
    }
    const int given = static_cast<int>(type.parameters.size());
    if (std::find(allowed.begin(), allowed.end(), given) != allowed.end()) {
      return;
    }
    std::vector<std::string> counts;
    counts.reserve(allowed.size());
    for (const int count : allowed) {
      counts.push_back(absl::StrCat(count));
    }
    report_(
        "flow.form.unknown-type",
        absl::StrCat(Quoted(type.name), " takes ",
                     absl::StrJoin(counts, " or "), " type parameter(s), but ",
                     type.ToString(), " gives ", given, "."),
        type.location);
  }

  const DtoNames& dtos_;
  Reporter report_;
};

/// Whether a value of this type can hold text: what `matching` needs.
bool IsTextType(std::string_view type) {
  return type == "string" || type == "text";
}

/// Whether a value of this type is counted rather than measured: what a length
/// range applies to, as against a value range.
bool IsSizedType(std::string_view type) {
  return IsTextType(type) || type == "bytes" || type == "list" ||
         type == "array" || type == "object" || type == "json";
}

/// Whether a value of this type is a magnitude a range may bound directly.
bool IsScalarType(std::string_view type) {
  return type == "number" || type == "integer" || type == "int" ||
         type == "duration" || type == "time";
}

/// Whether a constant could be a value of the named type.
///
/// Accepts broad input to catch a `default "yes"` on a `bool`, not every
/// way a value could later fail to fit. A field's real validation happens where
/// a value arrives, and a resolver that tried to do it here would be a second,
/// weaker copy of it.
bool ConstantFits(const syntax::Constant& value, std::string_view type) {
  using Kind = syntax::Constant::Kind;
  if (type == "any" || type == "json" || value.kind == Kind::kNull) {
    return true;
  }
  switch (value.kind) {
    case Kind::kBool:
      return type == "bool" || type == "boolean";
    case Kind::kInteger:
      return type == "number" || type == "integer" || type == "int" ||
             type == "duration" || type == "time";
    case Kind::kDouble:
      return type == "number" || type == "duration" || type == "time";
    case Kind::kString:
      // A byte string is written as text and a time is written as text, so a
      // string constant is a plausible default for either.
      return IsTextType(type) || type == "bytes" || type == "time" ||
             type == "duration";
    case Kind::kDuration:
      return type == "duration";
    case Kind::kList:
      return type == "list" || type == "array";
    case Kind::kObject:
      return type == "object";
    case Kind::kNull:
      return true;
  }
  return true;
}

/// The shapes a file declares, resolved: their fields, their types, and
/// everything wrong with them.
///
/// A pass of its own and ahead of the flows, because a port may name a shape
/// and a shape may name another one, in either order. Nothing here reads a
/// flow, so it does not need one.
class DtoResolver {
 public:
  DtoResolver(const LineIndex& lines,
              absl::Span<const syntax::DtoDeclarationPtr> declared,
              std::vector<Diagnostic>& diagnostics, CompilationBudget& budget)
      : budget_(budget),
        lines_(lines),
        declared_(declared),
        diagnostics_(diagnostics) {}

  std::vector<DtoPlan> Run() {
    Collect();
    std::vector<DtoPlan> plans;
    plans.reserve(kept_.size());
    for (const syntax::DtoDeclaration* declaration : kept_) {
      if (!budget_.Check()) {
        break;
      }
      plans.push_back(Resolve(*declaration));
      for (const auto& repeated : declared_) {
        if (!budget_.Check()) {
          break;
        }
        if (repeated->name.text != declaration->name.text) {
          continue;
        }
        plans.back().exported |= repeated->exported;
        const auto& description = repeated->description;
        if (!description.empty() && (plans.back().description.empty() ||
                                     description < plans.back().description)) {
          plans.back().description = description;
        }
      }
    }
    MarkBinaryTypes(plans, &budget_);
    return plans;
  }

 private:
  /// The declarations worth resolving, and the names they bind.
  ///
  /// A duplicate and a shape named after a built-in are both reported here and
  /// dropped, so nothing downstream has to wonder which `string` was meant.
  void Collect() {
    absl::flat_hash_set<std::string> seen;
    for (const syntax::DtoDeclarationPtr& declaration : declared_) {
      if (!budget_.Check()) {
        break;
      }
      const std::string& name = declaration->name.text;
      if (name.empty()) {
        continue;
      }
      if (vocabulary::TypeNames().contains(vocabulary::Canonical(name))) {
        Report("flow.form.struct-shadows-builtin",
               absl::StrCat("Shape ", Quoted(name),
                            " is named after a built-in type, which nothing "
                            "could then write."),
               declaration->name.location, name);
        continue;
      }
      if (!seen.insert(name).second) {
        const syntax::DtoDeclaration* previous = nullptr;
        for (const auto* one : kept_) {
          if (one->name.text == name) {
            previous = one;
          }
        }
        if (previous == nullptr) {
          continue;
        }
        auto a = DtoToJsonValue(Resolve(*previous));
        auto b = DtoToJsonValue(Resolve(*declaration));
        const auto normalize = [](auto& shape) {
          shape.erase("order");
          auto& fields = shape["fields"];
          if (fields.is_object()) {
            for (auto& field : fields) {
              field.erase("description");
              for (const auto key : {"type", "resolved", "element"}) {
                if (field.contains(key) && field[key].is_string()) {
                  field[key] = CanonicalPayloadType(
                      field[key].template get<std::string>());
                }
              }
              if (field.contains("one_of") && field["one_of"].is_array()) {
                auto& values = field["one_of"];
                std::sort(values.begin(), values.end());
                values.erase(std::unique(values.begin(), values.end()),
                             values.end());
              }
            }
          }
        };
        normalize(a);
        normalize(b);
        a.erase("description");
        b.erase("description");
        a.erase("exported");
        b.erase("exported");
        if (a != b) {
          Report("flow.form.duplicate-struct",
                 absl::StrCat("Shape ", Quoted(name),
                              " has inconsistent repeated definitions."),
                 declaration->name.location, name);
        } else {
          Diagnostic diagnostic;
          diagnostic.code = "flow.sequence.redundant-definition";
          diagnostic.message =
              absl::StrCat("Shape ", Quoted(name), " repeats the same schema.");
          diagnostic.severity = Severity::kInformation;
          diagnostic.family = Family::kSequence;
          diagnostic.range = lines_.Between(declaration->name.location.start,
                                            declaration->name.location.end);
          diagnostics_.push_back(std::move(diagnostic));
        }
        continue;
      }
      names_.insert(name);
      kept_.push_back(declaration.get());
    }
  }

  DtoPlan Resolve(const syntax::DtoDeclaration& declaration) {
    DtoPlan plan;
    plan.name = declaration.name.text;
    plan.exported = declaration.exported;
    plan.description = declaration.description;
    plan.location = declaration.location;

    TypeReader reader(names_, [&](std::string_view code, std::string message,
                                  const Location& location) {
      Report(code, std::move(message), location, plan.name);
    });

    absl::flat_hash_set<std::string> seen;
    for (const syntax::FieldDeclarationPtr& field : declaration.fields) {
      if (field->name.text.empty()) {
        continue;
      }
      if (!seen.insert(field->name.text).second) {
        Report("flow.form.duplicate-field",
               absl::StrCat("Field ", Quoted(field->name.text), " of ",
                            Quoted(plan.name), " is declared twice."),
               field->name.location, plan.name);
        continue;
      }
      plan.fields.push_back(ResolveField(*field, plan.name, reader));
    }
    return plan;
  }

  FieldPlan ResolveField(const syntax::FieldDeclaration& field,
                         std::string_view owner, TypeReader& reader) {
    FieldPlan entry;
    entry.name = field.name.text;
    entry.declared = field.type.ToString();
    entry.syntax_type = field.type;
    entry.type = reader.Read(field.type);
    entry.dto_name = reader.DtoOf(field.type);
    if ((entry.type == "list" || entry.type == "array") &&
        field.type.parameters.size() == 1) {
      entry.element = reader.Read(field.type.parameters.front());
      entry.element_dto_name = reader.DtoOf(field.type.parameters.front());
    }
    entry.required = field.required;
    entry.unique = field.unique;
    entry.range = field.range;
    entry.pattern = field.pattern;
    entry.has_pattern = field.has_pattern;
    entry.enumeration = field.enumeration;
    entry.has_enumeration = field.has_enumeration;
    entry.default_value = field.default_value;
    entry.has_default = field.has_default;
    entry.description = field.description;
    entry.location = field.location;
    CheckField(entry, field, owner);
    return entry;
  }

  /// The constraints, against the type they were written on.
  ///
  /// Every one of these is a mistake that would otherwise be found only when a
  /// value arrived and failed to validate for a reason the author could not act
  /// on -- `unique` on a string, a pattern on a number, a range on a bool.
  void CheckField(const FieldPlan& entry, const syntax::FieldDeclaration& field,
                  std::string_view owner) {
    const std::string_view type = entry.type;
    const bool shape = !entry.dto_name.empty();
    if (entry.unique && type != "list" && type != "array") {
      Report("flow.form.field-constraint",
             absl::StrCat("'unique' says no two items are equal, and ",
                          Quoted(entry.name), " holds one ", entry.declared,
                          " rather than a list."),
             field.location, owner);
    }
    if (entry.has_pattern && !IsTextType(type)) {
      Report("flow.form.field-constraint",
             absl::StrCat("'matching' compares text, and ", Quoted(entry.name),
                          " holds ", entry.declared, "."),
             field.location, owner);
    }
    if (!entry.range.Empty() && !IsSizedType(type) && !IsScalarType(type)) {
      Report("flow.form.field-constraint",
             absl::StrCat("A range bounds a number or a length, and ",
                          Quoted(entry.name), " holds ", entry.declared, "."),
             field.location, owner);
    }
    if (!entry.range.Empty() && entry.range.has_minimum &&
        entry.range.has_maximum &&
        entry.range.minimum.AsDouble() > entry.range.maximum.AsDouble()) {
      Report("flow.form.empty-range",
             absl::StrCat("The range on ", Quoted(entry.name),
                          " has its bounds the wrong way round, so nothing "
                          "would validate."),
             field.location, owner);
    }
    if (IsSizedType(type) && !IsScalarType(type) && entry.range.has_minimum &&
        entry.range.minimum.AsDouble() < 0) {
      Report("flow.form.field-constraint",
             absl::StrCat("A length is never negative, so the range on ",
                          Quoted(entry.name), " bounds nothing."),
             field.location, owner);
    }
    if (entry.has_default && !shape &&
        !ConstantFits(entry.default_value, type)) {
      Report("flow.form.field-type-mismatch",
             absl::StrCat("The default for ", Quoted(entry.name), " is a ",
                          syntax::ConstantKindName(entry.default_value.kind),
                          " and the field holds ", entry.declared, "."),
             field.location, owner);
    }
    if (entry.has_default && entry.required) {
      Report("flow.form.default-on-required",
             absl::StrCat(Quoted(entry.name),
                          " is required, so its default could never be used."),
             field.location, owner, Severity::kWarning);
    }
    if (entry.has_enumeration && !shape) {
      for (const syntax::Constant& allowed : entry.enumeration) {
        if (ConstantFits(allowed, type)) {
          continue;
        }
        Report("flow.form.field-type-mismatch",
               absl::StrCat("'one of' on ", Quoted(entry.name), " allows a ",
                            syntax::ConstantKindName(allowed.kind),
                            ", and the field holds ", entry.declared, "."),
               field.location, owner);
        break;
      }
    }
  }

  void Report(std::string_view code, std::string message,
              const Location& location, std::string_view owner,
              Severity severity = Severity::kError) {
    Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = severity;
    diagnostic.family = Family::kForm;
    diagnostic.message = std::move(message);
    diagnostic.range = lines_.Between(location.start, location.end);
    diagnostic.flow = std::string(owner);
    diagnostics_.push_back(std::move(diagnostic));
  }

  CompilationBudget& budget_;
  const LineIndex& lines_;
  absl::Span<const syntax::DtoDeclarationPtr> declared_;
  std::vector<Diagnostic>& diagnostics_;
  DtoNames names_;
  std::vector<const syntax::DtoDeclaration*> kept_;
};

/// One stage as it was written: `truncate 200`, `where it.ok`.
std::string StageLabel(const syntax::Stage& stage);
std::string StageBodyLabel(const syntax::Stage& stage);

/// An expression as a person would read it back, for a label or a message.
///
/// Not a formatter: this is the shortest faithful spelling of one expression,
/// the way `a11.flow.plan.unparse` writes it, so a step's label says what it
/// does. A literal as it would be written.
std::string ConstantText(const syntax::Constant& value) {
  switch (value.kind) {
    case syntax::Constant::Kind::kString:
      return absl::StrCat("\"", value.text, "\"");
    case syntax::Constant::Kind::kInteger:
      return absl::StrCat(value.integer);
    case syntax::Constant::Kind::kDouble:
      return absl::StrCat(value.number);
    case syntax::Constant::Kind::kBool:
      return value.boolean ? "true" : "false";
    case syntax::Constant::Kind::kDuration:
      return absl::FormatDuration(value.duration);
    default:
      return "null";
  }
}

std::string Unparse(const Node* node) {
  if (node == nullptr) {
    return "";
  }
  switch (node->kind) {
    case NodeKind::kLiteral: {
      const syntax::Constant& value = syntax::As<syntax::Literal>(node)->value;
      switch (value.kind) {
        case syntax::Constant::Kind::kString:
          return absl::StrCat("\"", value.text, "\"");
        case syntax::Constant::Kind::kInteger:
          return absl::StrCat(value.integer);
        case syntax::Constant::Kind::kDouble:
          return absl::StrCat(value.number);
        case syntax::Constant::Kind::kBool:
          return value.boolean ? "true" : "false";
        case syntax::Constant::Kind::kDuration:
          return absl::FormatDuration(value.duration);
        default:
          return "null";
      }
    }
    case NodeKind::kName:
      return syntax::As<syntax::Name>(node)->name;
    case NodeKind::kIt:
      return "it";
    case NodeKind::kDiscard:
      return "_";
    case NodeKind::kAttr: {
      const auto* attr = syntax::As<syntax::Attr>(node);
      return absl::StrCat(Unparse(attr->base.get()), ".", attr->name);
    }
    case NodeKind::kIndex: {
      const auto* index = syntax::As<syntax::Index>(node);
      return absl::StrCat(Unparse(index->base.get()), "[",
                          Unparse(index->index.get()), "]");
    }
    case NodeKind::kCallExpression: {
      const auto& call = *syntax::As<syntax::CallExpression>(node);
      return absl::StrCat(call.tolerant ? "try " : "", call.mode, " ",
                          call.action, "(...)");
    }
    case NodeKind::kBuiltin: {
      const auto* builtin = syntax::As<syntax::Builtin>(node);
      std::vector<std::string> args;
      args.reserve(builtin->args.size());
      for (const syntax::NodePtr& argument : builtin->args) {
        args.push_back(Unparse(argument.get()));
      }
      return absl::StrCat(builtin->name, "(", absl::StrJoin(args, ", "), ")");
    }
    case NodeKind::kUnary: {
      const auto* unary = syntax::As<syntax::Unary>(node);
      return absl::StrCat(unary->op, " ", Unparse(unary->operand.get()));
    }
    case NodeKind::kBinary: {
      const auto* binary = syntax::As<syntax::Binary>(node);
      return absl::StrCat(Unparse(binary->left.get()), " ", binary->op, " ",
                          Unparse(binary->right.get()));
    }
    case NodeKind::kTypedValue: {
      const auto* typed = syntax::As<syntax::TypedValue>(node);
      return absl::StrCat(typed->type.ToString(), "{...}");
    }
    case NodeKind::kListLiteral:
      return "[...]";
    case NodeKind::kObjectLiteral:
      return "{...}";
    case NodeKind::kOutcome:
      return absl::StrCat(
          "status ", Unparse(syntax::As<syntax::Outcome>(node)->subject.get()));
    case NodeKind::kPipelineValue:
      return absl::StrCat(
          "(", Unparse(syntax::As<syntax::PipelineValue>(node)->pipeline.get()),
          ")");
    case NodeKind::kPipeline: {
      const auto* pipeline = syntax::As<syntax::Pipeline>(node);
      std::string label = Unparse(pipeline->source.get());
      for (const syntax::StagePtr& stage : pipeline->stages) {
        absl::StrAppend(&label, " | ", StageLabel(*stage));
      }
      return label;
    }
    default:
      return syntax::NodeKindName(node->kind).data();
  }
}

/// One stage as it was written: `truncate 200`, `where it.ok`.
///
/// The tail a stage may carry -- `try` in front, `parallel n`, `unordered`,
/// `into failures` -- is written the way the author wrote it, because this
/// label is what `describe` prints and what an error message quotes back.
std::string StageLabel(const syntax::Stage& stage) {
  if (syntax::As<syntax::CallExpression>(stage.argument.get()) != nullptr) {
    return Unparse(stage.argument.get());
  }
  std::string tail;
  if (stage.parallel_expression != nullptr) {
    absl::StrAppend(&tail, " parallel ",
                    Unparse(stage.parallel_expression.get()));
  } else if (stage.parallel > 1) {
    absl::StrAppend(&tail, " parallel ", stage.parallel);
  }
  if (!stage.ordered) {
    absl::StrAppend(&tail, " unordered");
  }
  if (stage.failures != nullptr) {
    absl::StrAppend(&tail, " into ", Unparse(stage.failures.get()));
  }
  const std::string head = stage.tolerant ? "try " : "";
  const auto written = [&](std::string_view body) {
    return absl::StrCat(head, body, tail);
  };
  switch (stage.takes) {
    case vocabulary::StageArgument::kOptionalExpression:
      if (stage.argument == nullptr) {
        return written(stage.name);
      }
      return written(
          absl::StrCat(stage.name, " ", Unparse(stage.argument.get())));
    case vocabulary::StageArgument::kSortKey: {
      std::string body = stage.name;
      if (stage.argument != nullptr) {
        absl::StrAppend(&body, " by ", Unparse(stage.argument.get()));
      }
      if (stage.descending) {
        absl::StrAppend(&body, " desc");
      }
      return written(body);
    }
    case vocabulary::StageArgument::kFold:
      return written(absl::StrCat(stage.name, " ", ConstantText(stage.start),
                                  " as ", stage.carried.text, ", ",
                                  Unparse(stage.argument.get())));
    case vocabulary::StageArgument::kDuration:
      return written(
          absl::StrCat(stage.name, " ", absl::FormatDuration(stage.duration)));
    default:
      break;
  }
  if (!head.empty() || !tail.empty()) {
    return written(StageBodyLabel(stage));
  }
  return StageBodyLabel(stage);
}

/// The stage without the tail: what the older shapes have always printed.
std::string StageBodyLabel(const syntax::Stage& stage) {
  switch (stage.takes) {
    case vocabulary::StageArgument::kNumber:
      return absl::StrCat(stage.name, " ",
                          stage.is_integer ? absl::StrCat(stage.integer)
                                           : absl::StrCat(stage.number));
    case vocabulary::StageArgument::kString:
    case vocabulary::StageArgument::kOptionalString:
      if (stage.text.empty()) {
        return stage.name;
      }
      return absl::StrCat(stage.name, " \"", stage.text, "\"");
    case vocabulary::StageArgument::kExpression:
    case vocabulary::StageArgument::kStream:
      return absl::StrCat(stage.name, " ", Unparse(stage.argument.get()));
    case vocabulary::StageArgument::kLog:
    case vocabulary::StageArgument::kLogFormat: {
      if (stage.log.has_format) {
        return absl::StrCat(stage.name, " \"", stage.log.format, "\"");
      }
      if (stage.log.arguments.empty()) {
        return stage.name;
      }
      return absl::StrCat(stage.name, " ",
                          Unparse(stage.log.arguments.front().get()));
    }
    case vocabulary::StageArgument::kNone:
    default:
      return stage.name;
  }
}

/// What a reference resolved to.
///
/// Enough for the questions the resolver has to answer -- may this be read, may
/// it be written, does it have a status, is it the front of a stream a counted
/// `skip` could take values off -- and no more. The runtime's ref, with its
/// buffers and its readers, is a different object for a different job.
struct Ref {
  enum class Kind {
    kUnknown,
    /// A port of this flow.
    kPort,
    /// A port of a call: `x.out`.
    kCallPort,
    /// A node of this flow's own.
    kNode,
    /// `x.id`.
    kNodeId,
    /// An outcome: `status x`, or a bound barrier read as its status.
    kStatus,
    /// A header, a loop variable, a literal -- one value, ready-made.
    kValue,
    /// A stage applied to one of the above.
    kDerived,
  };

  Kind kind = Kind::kUnknown;
  std::string label;
  bool readable = true;
  bool writable = false;
  /// The symbol this reads through, for the usage counts.
  size_t symbol = kNoSymbol;
  /// Whether a counted `skip` may take values off the front of it: only a
  /// stream that is genuinely produced somewhere has a front.
  bool has_front = false;
  /// The graph ref this answer corresponds to, when a graph is being built.
  ///
  /// One extra field rather than a second resolver: ~25 sites return one of
  /// these, and what the runtime needs from them is the *identity* of the
  /// stream they named. [graph::kNone] on the editor path, and wherever the
  /// reference did not resolve.
  graph::RefId node = graph::kNone;
  /// For an outcome: whether a bad one is the flow's business or the subject's.
  /// True where the subject is a `try` call, or a barrier on one.
  bool tolerant = false;
  /// The shape this stream's values are, where the flow said so: a port typed
  /// with a `struct`, or what a `map Shape{..}` or an `as Shape` just made.
  ///
  /// Not a type system -- the language does not have one and is not getting one
  /// here. It is one fact carried along a pipeline, and it is carried because
  /// it answers a question that is otherwise unanswerable until a value
  /// arrives: whether `| json` has anything to render.
  ///
  /// Written `{}` rather than left bare so that the ~25 sites building one of
  /// these with a braced list -- every one of which stops before this field --
  /// are not each a -Wmissing-field-initializers warning.
  std::string shape{};
};

/// A subject's own label, out of the outcome that names it: `status x` -> `x`.
std::string SubjectLabel(std::string_view label) {
  constexpr std::string_view kPrefix = "status ";
  if (label.substr(0, std::min(kPrefix.size(), label.size())) == kPrefix) {
    return std::string(label.substr(kPrefix.size()));
  }
  return std::string(label);
}

/// One lexical scope, chained to its parent.
struct Scope {
  absl::flat_hash_map<std::string, size_t> names;
  const Scope* parent = nullptr;
  absl::flat_hash_map<std::string, size_t> outer_values;
};

/// The two passes over one flow declaration.
class FlowResolver {
 public:
  FlowResolver(const LineIndex& lines,
               const syntax::FlowDeclaration& declaration, const Program& known,
               ResolvedFlow& resolved, std::vector<Diagnostic>& diagnostics,
               CompilationBudget& budget,
               graph::GraphBuilder* absl_nullable builder = nullptr)
      : budget_(budget),
        lines_(lines),
        declaration_(declaration),
        known_(known),
        resolved_(resolved),
        diagnostics_(diagnostics),
        builder_(builder) {
    // The shapes are resolved before any flow is, so this is complete by the
    // time a port names one.
    for (const DtoPlan& dto : known.dtos) {
      dtos_.insert(dto.name);
    }
  }

  /// The flow's ports and headers, without resolving its body.
  ///
  /// The first of two passes, for the reason `a11/flow/plan.py` gives: a flow's
  /// ports are what a *sibling* calling it needs, and which flow is written
  /// first is the author's convenience rather than a dependency order.
  void Declare() {
    FlowPlan& plan = resolved_.plan;
    plan.name = declaration_.name.text;
    plan.entry = declaration_.entry;
    plan.interface_only = declaration_.interface_only;
    plan.registerable = declaration_.registerable;
    plan.imported = declaration_.imported;
    plan.module = declaration_.module;
    plan.exported = declaration_.exported;
    plan.binding_name = declaration_.binding_name.empty()
                            ? declaration_.name.text
                            : declaration_.binding_name;
    plan.description = declaration_.description;
    plan.location = declaration_.location;
    resolved_.declaration = &declaration_;

    absl::flat_hash_set<std::string> seen;
    if (declaration_.entry) {
      // The arguments, declared by nobody.
      for (const vocabulary::EntryPort& argument : vocabulary::EntryPorts()) {
        const std::string_view name = argument.name;
        const std::string_view type = argument.type;
        const bool unary = argument.unary;
        const std::string_view description = argument.description;
        PortPlan entry;
        entry.name = std::string(name);
        entry.direction = syntax::PortDirection::kInput;
        entry.declared = std::string(type);
        entry.type =
            PortType(syntax::TypeExpression{.name = std::string(type)});
        entry.unary = unary;
        entry.required = false;
        entry.description = std::string(description);
        entry.location = declaration_.location;
        seen.insert(absl::StrCat(
            syntax::PortDirectionName(syntax::PortDirection::kInput), ":",
            name));
        plan.ports.push_back(std::move(entry));
      }
    }
    for (const syntax::PortDeclarationPtr& port : declaration_.ports) {
      const std::string key = absl::StrCat(
          syntax::PortDirectionName(port->direction), ":", port->name.text);
      if (!seen.insert(key).second) {
        Report("flow.form.duplicate-port",
               absl::StrCat("Port ", Quoted(port->name.text),
                            " is declared twice."),
               port->location, Severity::kError, Family::kForm);
        continue;
      }
      PortPlan entry;
      entry.name = port->name.text;
      entry.direction = port->direction;
      entry.declared = port->type.ToString();
      entry.syntax_type = port->type;
      entry.type = PortType(port->type);
      entry.unary = port->unary;
      entry.required = port->required;
      entry.description = port->description;
      entry.location = port->location;
      plan.ports.push_back(std::move(entry));
    }
    for (const syntax::HeaderDeclarationPtr& header : declaration_.headers) {
      HeaderPlan entry;
      entry.name = absl::AsciiStrToLower(header->name);
      if (!seen.insert(absl::StrCat("header:", entry.name)).second) {
        Report("flow.form.duplicate-header",
               "A header name is declared twice after case normalization.",
               header->location, Severity::kError, Family::kForm);
        continue;
      }
      entry.alias = header->alias.text;
      entry.default_value = header->default_value;
      entry.has_default = header->has_default;
      entry.description = header->description;
      entry.location = header->location;
      plan.headers.push_back(std::move(entry));
    }
  }

  /// The flow's body, with every sibling's ports known.
  void ResolveBody() {
    if (declaration_.interface_only) {
      return;
    }
    if (builder_ != nullptr) {
      builder_->flow().name = resolved_.plan.name;
      body_ = builder_->AddBody("root");
      builder_->flow().root = body_;
    }
    Scope root;
    for (const PortPlan& port : resolved_.plan.ports) {
      const bool input = port.direction == syntax::PortDirection::kInput;
      Symbol symbol;
      symbol.kind = input ? SymbolKind::kInputPort : SymbolKind::kOutputPort;
      symbol.name = port.name;
      symbol.location = port.location;
      // An `in` port is read and an `out` port is written. Reading an `out`
      // port back is what a node of the flow's own is for, and the message for
      // it says so rather than leaving the author guessing.
      symbol.readable = input;
      symbol.writable = !input;
      if (builder_ != nullptr) {
        graph::Ref ref;
        ref.kind = graph::RefKind::kFlowPort;
        ref.label = port.name;
        ref.owner = body_;
        ref.name = port.name;
        ref.direction = port.direction;
        ref.writable = !input;
        // What the declaration said: a port carries one value unless it says
        // `stream`, and that is the only place the language states it outright.
        ref.unary = port.unary;
        symbol.ref = builder_->AddRef(std::move(ref));
      }
      Define(root, symbol);
    }
    for (const HeaderPlan& header : resolved_.plan.headers) {
      Symbol symbol;
      symbol.kind = SymbolKind::kHeader;
      symbol.name = header.alias;
      symbol.location = header.location;
      if (resolved_.implementation.has_value()) {
        for (const auto& local : resolved_.implementation->headers) {
          if (local.name == header.name) {
            symbol.name = local.alias;
            symbol.location = local.location;
            break;
          }
        }
      }
      symbol.readable = true;
      if (builder_ != nullptr) {
        graph::Ref ref;
        ref.kind = graph::RefKind::kHeader;
        ref.label = absl::StrCat("header \"", header.name, "\"");
        ref.owner = body_;
        ref.header = header.name;
        symbol.ref = builder_->AddRef(std::move(ref));
      }
      Define(root, symbol);
    }
    resolved_.plan.steps = ResolveStatements(declaration_.body, root);
  }

 private:
  // -- the graph -------------------------------------------------------------

  /// A ref of a kind that carries nothing but its label: `kExpr`, `kDerived`.
  graph::RefId NewRef(graph::RefKind kind, std::string label,
                      graph::BodyId owner) {
    graph::Ref ref;
    ref.kind = kind;
    ref.label = std::move(label);
    ref.owner = owner;
    return builder_->AddRef(std::move(ref));
  }

  /// One stage applied to `source`, as a fresh ref: a mention, not an identity.
  graph::RefId Derive(graph::RefId source, graph::Stage stage,
                      std::string label) {
    if (builder_ == nullptr || source == graph::kNone) {
      return graph::kNone;
    }
    graph::Ref ref;
    ref.kind = graph::RefKind::kDerived;
    ref.label = std::move(label);
    ref.owner = body_;
    ref.source = source;
    ref.stage = std::move(stage);
    return builder_->AddRef(std::move(ref));
  }

  graph::StepId NewStep(graph::StepKind kind, std::string label,
                        const Location& location) {
    graph::Step step;
    step.kind = kind;
    step.phase_gate = phase_gate_;
    step.label = std::move(label);
    step.body = body_;
    step.location = location;
    return builder_->AddStep(std::move(step));
  }

  /// The status of `subject`, as the graph's own ref.
  ///
  /// A call's is memoised on its step; a node's or a port's is fresh per
  /// mention, exactly as `a11.flow.plan` makes them, because the counts follow
  /// from it.
  graph::RefId StatusOfStep(graph::StepId step) {
    if (builder_ == nullptr || step == graph::kNone) {
      return graph::kNone;
    }
    graph::Step& one = builder_->step(step);
    if (one.status != graph::kNone) {
      return one.status;
    }
    graph::Ref ref;
    ref.kind = graph::RefKind::kStatus;
    ref.label = absl::StrCat("status ", one.label);
    ref.owner = one.body;
    ref.subject_step = step;
    const graph::RefId made = builder_->AddRef(std::move(ref));
    builder_->step(step).status = made;
    return made;
  }

  // Which subject of a race won, as the value that barrier is. Memoised on the
  // step the way a call's status is: `n = wait first of a, b` and `wait first
  // of a, b -> n` are one race however many read it.
  /// Which subject of a race won, as the value that barrier is.
  ///
  /// Memoised on the step the way a call's status is: `n = wait first of a, b`
  /// and `wait first of a, b -> n` are one race however many read it.
  graph::RefId WinnerOfStep(graph::StepId step) {
    if (builder_ == nullptr || step == graph::kNone) {
      return graph::kNone;
    }
    if (builder_->step(step).winner != graph::kNone) {
      return builder_->step(step).winner;
    }
    graph::Ref ref;
    ref.kind = graph::RefKind::kWinner;
    ref.label = absl::StrCat("winner of ", builder_->step(step).label);
    ref.owner = builder_->step(step).body;
    ref.subject_step = step;
    const graph::RefId made = builder_->AddRef(std::move(ref));
    builder_->step(step).winner = made;
    return made;
  }

  /// The status of a call, fresh per mention.
  ///
  /// `wait x` and `status x` each make their own, exactly as `a11.flow.plan`
  /// does -- only `x.status` is memoised -- and the reader counts follow from
  /// it. A status is one value however many refs name it, so this costs nothing
  /// but keeps the two implementations countable against each other.
  graph::RefId FreshStatusOfStep(graph::StepId step) {
    if (builder_ == nullptr || step == graph::kNone) {
      return graph::kNone;
    }
    graph::Ref ref;
    ref.kind = graph::RefKind::kStatus;
    ref.label = absl::StrCat("status ", builder_->step(step).label);
    ref.owner = builder_->step(step).body;
    ref.subject_step = step;
    return builder_->AddRef(std::move(ref));
  }

  graph::RefId StatusOfRef(graph::RefId subject, std::string_view label) {
    if (builder_ == nullptr || subject == graph::kNone) {
      return graph::kNone;
    }
    graph::Ref ref;
    ref.kind = graph::RefKind::kStatus;
    ref.label = absl::StrCat("status ", label);
    ref.owner = builder_->ref(subject).owner;
    ref.subject = subject;
    return builder_->AddRef(std::move(ref));
  }

  /// The graph stage one written stage becomes, with its argument resolved.
  graph::Stage GraphStage(const syntax::Stage& stage, graph::RefId stream,
                          graph::ExprId expr, graph::LogTail log = {},
                          graph::RefId failures = graph::kNone) {
    graph::Stage made;
    made.name = stage.name;
    made.takes = stage.takes;
    made.tolerant = stage.tolerant;
    made.failures = failures;
    made.parallel = stage.parallel;
    made.ordered = stage.ordered;
    switch (stage.takes) {
      case vocabulary::StageArgument::kNumber:
        made.count = stage.is_integer ? stage.integer : 0;
        break;
      case vocabulary::StageArgument::kString:
      case vocabulary::StageArgument::kOptionalString:
        made.text = stage.text;
        break;
      case vocabulary::StageArgument::kExpression:
      case vocabulary::StageArgument::kOptionalExpression:
        made.expr = expr;
        break;
      case vocabulary::StageArgument::kSortKey:
        made.expr = expr;
        made.descending = stage.descending;
        break;
      case vocabulary::StageArgument::kFold:
        made.expr = expr;
        made.start = stage.start;
        made.carried = stage.carried.text;
        break;
      case vocabulary::StageArgument::kDuration:
        made.duration = stage.duration;
        made.expr = expr;
        break;
      case vocabulary::StageArgument::kStream:
        made.stream = stream;
        break;
      case vocabulary::StageArgument::kLog:
      case vocabulary::StageArgument::kLogFormat:
        made.log = std::move(log);
        break;
      case vocabulary::StageArgument::kNone:
        break;
    }
    return made;
  }

  /// The `at` stage `x.field` and `x[i]` over a stream compile to.
  static graph::Stage AtStage(const syntax::Constant& key) {
    graph::Stage made;
    made.name = "at";
    made.takes = vocabulary::StageArgument::kString;
    if (key.kind == syntax::Constant::Kind::kInteger) {
      made.indexed = true;
      made.index = key.integer;
      made.text = absl::StrCat(key.integer);
    } else {
      made.text = key.text;
    }
    return made;
  }

  static graph::Stage AtStage(std::string field) {
    graph::Stage made;
    made.name = "at";
    made.takes = vocabulary::StageArgument::kString;
    made.text = std::move(field);
    return made;
  }

  // -- names -----------------------------------------------------------------

  void Define(Scope& scope, Symbol symbol) {
    if (symbol.name == "std") {
      Report("flow.name.taken", "The builtin 'std' binding is reserved.",
             symbol.location);
    }
    // The one place every name in a flow arrives -- a port, a header, a call, a
    // node, a node map, a barrier, a `value`, a loop variable, a carry -- and so
    // the one place `_` has to be turned away.
    if (symbol.name == "std") {
      Report("flow.name.taken", "The implicit std module cannot be rebound.",
             symbol.location);
    } else if (symbol.name == "_") {
      Report("flow.name.discard-bound",
             "'_' is the discard, not a name: it keeps nothing, so nothing can "
             "be bound to it or read back out of it. Give this a name, or "
             "write '-> _' where the values are to be thrown away.",
             symbol.location);
    }
    resolved_.symbols.push_back(std::move(symbol));
    scope.names[resolved_.symbols.back().name] = resolved_.symbols.size() - 1;
  }

  void CaptureVariables(Scope& outer, Scope& inner, graph::BodyId body) {
    absl::flat_hash_set<std::string> seen;
    for (const Scope* at = &outer; at != nullptr; at = at->parent) {
      for (const auto& [name, index] : at->names) {
        if (!seen.insert(name).second || inner.names.contains(name) ||
            resolved_.symbols[index].kind != SymbolKind::kValue) {
          continue;
        }
        Symbol symbol = resolved_.symbols[index];
        symbol.reads = 0;
        symbol.writes = 0;
        symbol.implicit = true;
        if (builder_ != nullptr) {
          graph::Ref input;
          input.kind = graph::RefKind::kBound;
          input.owner = body;
          input.role = "scope-input";
          input.label = name;
          input.source = symbol.ref;
          symbol.ref = builder_->AddRef(std::move(input));
        }
        Define(inner, std::move(symbol));
        inner.outer_values[name] = resolved_.symbols.size() - 1;
      }
    }
  }

  void JoinVariables(
      Scope& outer,
      const std::vector<std::pair<Scope*, graph::BodyId>>& children,
      graph::StepId owner, bool parallel = false) {
    for (const auto& [child, body] : children) {
      for (const auto source : body_extractions_[body]) {
        if (parallel && builder_ != nullptr &&
            builder_->flow().refs[source].owner != body) {
          Report(
              "flow.sequence.conflicting-access",
              "Parallel passes cannot consume the same outer stream with take.",
              builder_->step(owner).location);
        }
        last_extraction_[source] = owner;
        body_extractions_[body_].insert(source);
      }
    }
    for (const auto& [child, body] : children) {
      for (const auto& [name, input] : child->outer_values) {
        const auto prior = Lookup(outer, name);
        if (prior != kNoSymbol) {
          resolved_.symbols[prior].reads += resolved_.symbols[input].reads;
        }
      }
    }
    absl::flat_hash_set<std::string> changed;
    for (const auto& [child, body] : children) {
      for (const auto& [name, input] : child->outer_values) {
        if (child->names.at(name) != input) {
          changed.insert(name);
        }
      }
    }
    for (const auto& name : changed) {
      const auto prior = Lookup(outer, name);
      if (prior == kNoSymbol) {
        continue;
      }
      ++resolved_.symbols[prior].reads;
      Symbol merged = resolved_.symbols[prior];
      merged.implicit = true;
      if (parallel) {
        Report("flow.sequence.conflicting-access",
               absl::StrCat("Parallel passes cannot update outer variable '",
                            name, "'. Use a reduction or a sequential loop."),
               merged.location, Severity::kError, Family::kBarrier);
        continue;
      }
      if (builder_ != nullptr) {
        graph::Ref value;
        value.kind = graph::RefKind::kBound;
        value.owner = body_;
        value.role = "joined-value";
        value.label = name;
        value.bound_by = owner;
        value.source = merged.ref;
        const auto result = builder_->AddRef(std::move(value));
        graph::Step::VariableUpdate update;
        update.result = result;
        for (const auto& [child, body] : children) {
          const auto seeded = child->outer_values.find(name);
          if (seeded == child->outer_values.end()) {
            continue;
          }
          const auto final = child->names.at(name);
          graph::Step capture;
          capture.kind = graph::StepKind::kCapture;
          capture.body = body;
          capture.label = absl::StrCat("publish ", name);
          capture.source = resolved_.symbols[final].ref;
          ++resolved_.symbols[final].reads;
          capture.slot = absl::StrCat("merge:", result);
          builder_->AddStep(std::move(capture));
          update.inputs.emplace_back(body,
                                     resolved_.symbols[seeded->second].ref);
        }
        builder_->step(owner).variable_updates.push_back(std::move(update));
        merged.ref = result;
        merged.step = owner;
      }
      Define(outer, std::move(merged));
    }
  }

  [[nodiscard]] size_t Lookup(const Scope& scope, std::string_view name) const {
    for (const Scope* at = &scope; at != nullptr; at = at->parent) {
      const auto found = at->names.find(name);
      if (found != at->names.end()) {
        return found->second;
      }
    }
    return kNoSymbol;
  }

  Symbol* Find(const Scope& scope, std::string_view name) {
    const size_t index = Lookup(scope, name);
    return index == kNoSymbol ? nullptr : &resolved_.symbols[index];
  }

  /// Every name in scope, sorted -- what a message lists as what was available.
  [[nodiscard]] std::string Known(const Scope& scope) const {
    std::vector<std::string> names;
    for (const Scope* at = &scope; at != nullptr; at = at->parent) {
      for (const auto& [name, index] : at->names) {
        if (!name.empty() && name.front() == '\x1f') {
          continue;
        }
        names.push_back(name);
      }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return absl::StrJoin(names, ", ");
  }

  std::string Label(std::string_view base) {
    const int count = ++labels_[std::string(base)];
    return count == 1 ? std::string(base) : absl::StrCat(base, "#", count);
  }

  void Report(std::string_view code, std::string message,
              const Location& location, Severity severity = Severity::kError,
              Family family = Family::kName) {
    Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = severity;
    diagnostic.family = family;
    diagnostic.message = std::move(message);
    diagnostic.range = lines_.Between(location.start, location.end);
    diagnostic.flow = declaration_.name.text;
    diagnostics_.push_back(std::move(diagnostic));
  }

  // -- types -----------------------------------------------------------------

  /// What a declared type gives a port, and a diagnostic where it gives
  /// nothing.
  ///
  /// Delegated to [TypeReader] rather than decided here: a port and a `struct`
  /// field have to agree about what a name means, and they do so by asking the
  /// same object.
  std::string PortType(const syntax::TypeExpression& type) {
    return TypeReader(dtos_,
                      [this](std::string_view code, std::string message,
                             const Location& location) {
                        Report(code, std::move(message), location,
                               Severity::kError, Family::kForm);
                      })
        .Read(type);
  }

  // -- statements ------------------------------------------------------------

  /// Read a block of statements, into `body` when a graph is being built.
  /// `guarded` says this body is an `if`, a `for` or a `repeat` body: a scope
  /// that makes what is inside it conditional on something. Counted rather than
  /// derived from `body`, because the editor path builds no graph and every
  /// nested body is [graph::kNone] there, and a diagnostic that only fires when
  /// a graph is being built is a diagnostic an editor never shows.
  std::vector<StepPlan> ResolveStatements(
      const std::vector<syntax::NodePtr>& statements, Scope& scope,
      graph::BodyId body = graph::kNone, bool guarded = false) {
    const auto previous_extractions = last_extraction_;
    const auto previous_dynamic = dynamic_extractions_;
    const graph::BodyId outer = body_;
    const auto outer_gate = phase_gate_;
    if (body != graph::kNone && body != body_) {
      phase_gate_ = graph::kNone;
    }
    if (body != graph::kNone) {
      body_ = body;
    }
    if (guarded) {
      ++guarded_;
    }
    std::vector<StepPlan> steps;
    for (const syntax::NodePtr& statement : statements) {
      if (!budget_.Check()) {
        break;
      }
      // Resolving a statement source may create a barrier step for a racing
      // value read.
      std::vector<StepPlan>* const outer_pending = pending_steps_;
      pending_steps_ = &steps;
      const auto first_ref = builder_->flow().refs.size();
      const auto first_plan = steps.size();
      const auto active_gate = phase_gate_;
      ResolveStatement(statement.get(), scope, steps);
      for (size_t id = first_ref; id < builder_->flow().refs.size(); ++id) {
        if (builder_->ref(id).owner == body_) {
          builder_->ref(id).phase_gate = active_gate;
        }
      }
      if (active_gate != graph::kNone) {
        for (size_t id = first_plan; id < steps.size(); ++id) {
          steps[id].after.push_back(builder_->step(active_gate).label);
        }
      }
      pending_steps_ = outer_pending;
    }
    if (guarded) {
      --guarded_;
    }
    if (body_ == builder_->flow().root) {
      auto& flow = builder_->flow();
      absl::flat_hash_set<graph::RefId> attached;
      for (const auto& step : flow.steps) {
        for (const auto target : graph::TransferTargets(step)) {
          if (target != graph::kNone &&
              flow.refs[target].kind == graph::RefKind::kSink) {
            attached.insert(target);
          }
        }
      }
      for (const auto& step : flow.steps) {
        if (step.kind == graph::StepKind::kCancel &&
            step.target != graph::kNone) {
          const auto sink = flow.steps[step.target].sink;
          if (sink != graph::kNone) {
            attached.insert(sink);
          }
        }
      }
      for (auto& step : flow.steps) {
        if (step.sink != graph::kNone &&
            attached.find(step.sink) == attached.end()) {
          step.sink = graph::kNone;
        }
      }
    }
    CheckScopePhases();
    body_ = outer;
    phase_gate_ = outer_gate;
    if (body != graph::kNone) {
      last_extraction_ = previous_extractions;
      dynamic_extractions_ = previous_dynamic;
    }
    return steps;
  }

  void CheckContinuationCycles() {
    const auto& flow = builder_->flow();
    if (std::none_of(flow.refs.begin(), flow.refs.end(), [](const auto& ref) {
          return ref.kind == graph::RefKind::kSink;
        })) {
      return;
    }
    std::vector<std::vector<size_t>> dependencies(flow.steps.size() * 2);
    const auto edge = [&](size_t from, size_t to) {
      dependencies[from].push_back(to);
    };
    for (size_t id = 0; id < flow.steps.size() && budget_.Check(); ++id) {
      const auto& step = flow.steps[id];
      edge(2 * id + 1, 2 * id);
      if (step.transfer != nullptr) {
        for (const auto called : step.transfer->destination_calls) {
          edge(2 * id + 1, 2 * called + 1);
        }
        for (const auto& join : step.transfer->joins) {
          if (!join.expression) {
            edge(2 * id, 2 * join.target + 1);
          }
        }
      }
      if (step.phase_gate != graph::kNone) {
        edge(2 * id, 2 * step.phase_gate + 1);
      }
      for (const auto after : step.after) {
        edge(2 * id, 2 * after + 1);
      }
      for (const auto before : step.preceding_steps) {
        edge(2 * id, 2 * before + 1);
      }
      for (const auto body : step.bodies) {
        for (const auto child : flow.bodies[body].steps) {
          edge(2 * id, 2 * child + 1);
        }
      }
      if (step.kind == graph::StepKind::kPipe &&
          step.destination != graph::kNone &&
          flow.refs[step.destination].kind == graph::RefKind::kSink) {
        edge(2 * id, 2 * flow.refs[step.destination].subject_step);
      }
      for (const auto ref : flow.Destinations(id)) {
        if (flow.refs[ref].kind != graph::RefKind::kSink) {
          continue;
        }
        // A child scope holds its enclosing owner's reservation until cleanup.
        auto owner = id;
        auto body = step.body;
        while (body != flow.refs[ref].owner && body != graph::kNone &&
               budget_.Check()) {
          owner = flow.bodies[body].owner_step;
          body = flow.bodies[body].parent;
        }
        if (body == flow.refs[ref].owner && owner != graph::kNone) {
          edge(2 * flow.refs[ref].subject_step + 1, 2 * owner);
        }
      }
      auto pending = flow.Sources(id);
      for (const auto ref : flow.ValueSources(id)) {
        pending.push_back(ref);
      }
      absl::flat_hash_set<graph::RefId> seen;
      while (!pending.empty() && budget_.Check()) {
        const auto ref = pending.back();
        pending.pop_back();
        if (ref == graph::kNone || !seen.insert(ref).second) {
          continue;
        }
        if (flow.refs[ref].kind == graph::RefKind::kStatus &&
            flow.refs[ref].subject_step != graph::kNone) {
          edge(2 * id, 2 * flow.refs[ref].subject_step + 1);
        }
        for (const auto up : flow.Upstreams(ref)) {
          pending.push_back(up);
        }
        for (const auto up : flow.ValueRefs(ref)) {
          pending.push_back(up);
        }
      }
    }
    std::vector<int> state(dependencies.size());
    std::vector<std::pair<size_t, size_t>> stack;
    for (size_t root = 0; root < state.size() && budget_.Check(); ++root) {
      if (state[root] != 0) {
        continue;
      }
      state[root] = 1;
      stack.emplace_back(root, 0);
      while (!stack.empty() && budget_.Check()) {
        auto& [node, next] = stack.back();
        if (next == dependencies[node].size()) {
          state[node] = 2;
          stack.pop_back();
          continue;
        }
        const auto dependency = dependencies[node][next++];
        if (state[dependency] == 1) {
          const auto cycle_begin = std::find_if(
              stack.begin(), stack.end(),
              [&](const auto& entry) { return entry.first == dependency; });
          if (std::any_of(cycle_begin, stack.end(), [&](const auto& entry) {
                return entry.first % 2 != 0 &&
                       flow.steps[entry.first / 2].sink != graph::kNone;
              })) {
            const auto location = flow.steps[node / 2].location;
            Report("flow.barrier.continuation-cycle",
                   "Continuation completion depends on a writer whose "
                   "activation it prevents. Keep attachments before a "
                   "barrier that waits for their handle.",
                   location, Severity::kError, Family::kBarrier);
            for (auto at = cycle_begin; at != stack.end(); ++at) {
              const auto related = flow.steps[at->first / 2].location;
              diagnostics_.back().related.push_back(
                  {"", lines_.Between(related.start, related.end),
                   at->first % 2 != 0 ? "Full completion waits here."
                                      : "Writer or gate in this cycle."});
            }
            return;
          }
          continue;
        }
        if (state[dependency] == 0) {
          state[dependency] = 1;
          stack.emplace_back(dependency, 0);
        }
      }
    }
  }

  void CheckScopePhases() {
    auto& flow = builder_->flow();
    if (body_ == flow.root) {
      CheckContinuationCycles();
    }
    if (std::none_of(
            flow.bodies[body_].steps.begin(), flow.bodies[body_].steps.end(),
            [&](const auto step) { return flow.steps[step].scope_barrier; })) {
      return;
    }

    struct Read {
      graph::RefId ref;
      graph::StepId step;
      graph::StepId gate;
    };

    std::vector<Read> reads;
    std::vector<graph::StepId> owners(flow.steps.size(), graph::kNone);
    absl::flat_hash_map<graph::RefId, graph::StepId> first_read;
    absl::flat_hash_map<graph::StepId, graph::StepId> first_activation;
    absl::flat_hash_map<graph::RefId, std::vector<graph::StepId>> writers;
    const auto remember = [](auto& phases, const auto key, const auto gate) {
      const auto found = phases.find(key);
      if (found == phases.end() || gate == graph::kNone ||
          (found->second != graph::kNone && gate < found->second)) {
        phases[key] = gate;
      }
    };
    for (size_t id = 0; id < flow.steps.size() && budget_.Check(); ++id) {
      auto owner = id;
      auto body = flow.steps[id].body;
      while (body != body_ && body != graph::kNone && budget_.Check()) {
        const auto& nested = flow.bodies[body];
        if (nested.owner_step != graph::kNone) {
          owner = nested.owner_step;
        }
        body = nested.parent;
      }
      if (body != body_) {
        continue;
      }
      owners[id] = owner;
      const auto gate = flow.steps[owner].phase_gate;
      for (const auto destination : flow.Destinations(id)) {
        writers[destination].push_back(id);
      }
      std::vector<graph::RefId> pending = flow.Sources(id);
      for (const auto ref : flow.ValueSources(id)) {
        pending.push_back(ref);
      }
      absl::flat_hash_set<graph::RefId> visited;
      while (!pending.empty() && budget_.Check()) {
        const auto ref = pending.back();
        pending.pop_back();
        if (ref == graph::kNone || !visited.insert(ref).second) {
          continue;
        }
        reads.push_back({ref, id, gate});
        const auto& read = flow.refs[ref];
        if (read.owner == body_) {
          remember(first_read, ref, gate);
        }
        if (read.kind == graph::RefKind::kCallPort) {
          remember(first_activation, read.call, gate);
        } else if (read.kind == graph::RefKind::kStatus &&
                   read.subject_step != graph::kNone) {
          remember(first_activation, read.subject_step, gate);
        }
        for (const auto up : flow.Upstreams(ref)) {
          pending.push_back(up);
        }
        for (const auto up : flow.ValueRefs(ref)) {
          pending.push_back(up);
        }
      }
    }
    const auto precedes = [&](const auto step, const auto gate) {
      const auto owner = owners[step];
      const auto& preceding = flow.steps[gate].preceding_steps;
      return std::find(preceding.begin(), preceding.end(), owner) !=
             preceding.end();
    };
    const auto active_before = [&](const auto call, const auto gate) {
      if (!precedes(call, gate)) {
        return false;
      }
      const auto found = first_activation.find(call);
      return !flow.steps[call].deferred ||
             (found != first_activation.end() &&
              (found->second == graph::kNone || found->second < gate));
    };
    const auto cycle = [&](const auto step, const auto gate,
                           const auto producer, std::string message) {
      Report("flow.barrier.scope-cycle", std::move(message),
             flow.steps[step].location, Severity::kError, Family::kBarrier);
      for (const auto& [related, label] :
           {std::pair{gate, "This barrier waits for preceding work."},
            std::pair{producer, "Conflicting stream dependency."}}) {
        const auto location = flow.steps[related].location;
        diagnostics_.back().related.push_back(
            {"", lines_.Between(location.start, location.end), label});
      }
    };
    for (const auto& read : reads) {
      if (!budget_.Check()) {
        break;
      }
      const auto gate = read.gate;
      if (gate == graph::kNone || !flow.steps[gate].scope_barrier) {
        continue;
      }
      const auto& ref = flow.refs[read.ref];
      graph::StepId producer = graph::kNone;
      if (ref.kind == graph::RefKind::kCallPort &&
          active_before(ref.call, gate)) {
        producer = ref.call;
      }
      const auto found = writers.find(read.ref);
      if (found != writers.end()) {
        for (const auto writer : found->second) {
          if (precedes(writer, gate) && !flow.steps[writer].deferred) {
            producer = writer;
            break;
          }
        }
      }
      if (producer != graph::kNone) {
        cycle(read.step, gate, producer,
              "A producer before 'wait all' needs a stream consumer after "
              "the barrier. Keep the producer and consumer in the same phase.");
      }
    }
    for (const auto& [ref, destinations] : writers) {
      for (const auto writer : destinations) {
        if (!budget_.Check()) {
          break;
        }
        const auto gate = flow.steps[owners[writer]].phase_gate;
        if (gate == graph::kNone || !flow.steps[gate].scope_barrier) {
          continue;
        }
        const auto& target = flow.refs[ref];
        const auto read = first_read.find(ref);
        if ((target.kind == graph::RefKind::kCallPort &&
             active_before(target.call, gate)) ||
            (target.kind == graph::RefKind::kNode && read != first_read.end() &&
             (read->second == graph::kNone || read->second < gate))) {
          cycle(writer, gate, writer,
                "Work before 'wait all' needs an input feed after the "
                "barrier. Keep the feed and its consumer in the same phase.");
        }
      }
    }
    for (const auto& [ref, gate] : first_read) {
      if (gate != graph::kNone &&
          flow.refs[ref].kind != graph::RefKind::kNode &&
          flow.refs[ref].phase_gate == graph::kNone) {
        flow.refs[ref].phase_gate = gate;
      }
    }
  }

  bool HasDataDependency(const Node* expression, const Scope& scope) {
    if (expression == nullptr) {
      return false;
    }
    if (const auto* name = syntax::As<syntax::Name>(expression);
        name != nullptr) {
      const size_t index = Lookup(scope, name->name);
      if (index == kNoSymbol) {
        return false;
      }
      const auto& symbol = resolved_.symbols[index];
      switch (symbol.kind) {
        case SymbolKind::kInputPort:
        case SymbolKind::kOutputPort:
        case SymbolKind::kNode:
        case SymbolKind::kSink:
        case SymbolKind::kBarrier:
        case SymbolKind::kLoopVariable:
        case SymbolKind::kCarry:
          return true;
        case SymbolKind::kValue:
          return symbol.data_dependency;
        default:
          return false;
      }
    }
    if (const auto* attr = syntax::As<syntax::Attr>(expression);
        attr != nullptr) {
      const auto base = DescribeExpression(attr->base.get(), scope);
      if (base.category == ExpressionCategory::kCall) {
        return true;
      }
      if (base.category == ExpressionCategory::kNode && attr->name == "id") {
        if (const auto* node =
                syntax::As<syntax::NodeExpression>(attr->base.get());
            node != nullptr) {
          return HasDataDependency(node->id.get(), scope);
        }
        if (const auto* name = syntax::As<syntax::Name>(attr->base.get());
            name != nullptr) {
          const size_t index = Lookup(scope, name->name);
          return index != kNoSymbol && resolved_.symbols[index].data_dependency;
        }
        return false;
      }
      return HasDataDependency(attr->base.get(), scope);
    }
    if (expression->kind == NodeKind::kOutcome ||
        expression->kind == NodeKind::kWait ||
        expression->kind == NodeKind::kClose ||
        expression->kind == NodeKind::kNodeExpression) {
      return true;
    }
    if (const auto* binary = syntax::As<syntax::Binary>(expression);
        binary != nullptr) {
      const auto left = syntax::ConstantValue(binary->left.get());
      if (left.has_value() && left->kind == syntax::Constant::Kind::kBool &&
          ((binary->op == "and" && !left->boolean) ||
           (binary->op == "or" && left->boolean))) {
        return HasDataDependency(binary->left.get(), scope);
      }
    }
    bool dependent = false;
    syntax::VisitChildren(*expression, [&](const Node& child) {
      if (!dependent) {
        dependent = HasDataDependency(&child, scope);
      }
    });
    return dependent;
  }

  /// Unconditional effects need a control, explicit, or data dependency.
  /// Action and node handles alone do not wait for their producers.
  void CheckReachedByChoice(std::string_view code, std::string_view what,
                            const std::vector<syntax::Word>& after,
                            const Location& location,
                            bool data_dependency = false) {
    if (guarded_ > 0 || phase_gate_ != graph::kNone || !after.empty() ||
        data_dependency) {
      return;
    }
    if (what == "cancel") {
      Report(code,
             "This 'cancel' has no condition or 'after', so it can cancel the "
             "action as soon as it starts. Put it in an 'if' or a loop body, "
             "or give it an 'after'.",
             location, Severity::kError, Family::kForm);
      return;
    }
    Report(
        code,
        absl::StrCat("This '", what,
                     "' has no condition, 'after', or data-dependent argument. "
                     "It can run immediately alongside the other statements. "
                     "Read the result it depends on, put it in an 'if', or "
                     "give it an 'after'."),
        location, Severity::kError, Family::kForm);
  }

  /// A stream the runtime binds per pass: a loop's value, its index, a carry.
  graph::RefId BoundRef(graph::BodyId owner, graph::StepId step,
                        std::string role) {
    if (builder_ == nullptr) {
      return graph::kNone;
    }
    graph::Ref ref;
    ref.kind = graph::RefKind::kBound;
    ref.label = role;
    ref.owner = owner;
    ref.role = std::move(role);
    ref.bound_by = step;
    return builder_->AddRef(std::move(ref));
  }

  void ResolveStatement(const Node* statement, Scope& scope,
                        std::vector<StepPlan>& steps) {
    switch (statement->kind) {
      case NodeKind::kBind:
        return ResolveBind(syntax::As<syntax::Bind>(statement), scope, steps);
      case NodeKind::kBlock:
        return ResolveBlock(syntax::As<syntax::Block>(statement), scope, steps);
      case NodeKind::kCallStatement: {
        const auto* held = syntax::As<syntax::CallStatement>(statement);
        ResolveCall(*held->call, scope, Label(held->call->action), steps);
        return;
      }
      case NodeKind::kPipe:
        ResolveTransfer(*syntax::As<syntax::Pipe>(statement), scope, steps);
        return;
      case NodeKind::kSkip: {
        const auto* skip = syntax::As<syntax::Skip>(statement);
        if (!skip->count.has_value()) {
          return;
        }
        const std::vector<std::string> after = ResolveAfter(skip->after, scope);
        const std::vector<graph::StepId> held = after_waits_;
        ResolveSkip(*skip, scope, after, held, steps);
        return;
      }
      case NodeKind::kWait: {
        const auto* wait = syntax::As<syntax::Wait>(statement);
        if (wait->scope_barrier) {
          auto preceding = builder_->flow().bodies[body_].steps;
          std::vector<graph::ExprId> cleanups;
          for (size_t id = 0; id < builder_->flow().exprs.size(); ++id) {
            const auto& expression = builder_->expr(id);
            if (expression.activation != graph::kNone &&
                builder_->flow().bodies[expression.activation].parent ==
                    body_) {
              cleanups.push_back(id);
            }
          }
          const auto barrier = NewStep(graph::StepKind::kWait,
                                       Label("wait all"), wait->location);
          builder_->step(barrier).scope_barrier = true;
          builder_->step(barrier).preceding_steps = std::move(preceding);
          builder_->step(barrier).expression_cleanups = std::move(cleanups);
          StepPlan plan;
          plan.kind = "wait";
          plan.label = builder_->step(barrier).label;
          plan.scope_barrier = true;
          plan.location = wait->location;
          steps.push_back(std::move(plan));
          phase_gate_ = barrier;
          return;
        }
        // `wait first of a, b` and `wait all of a, b` read several outcomes.
        if (!wait->subjects.empty()) {
          std::vector<Ref> outcomes;
          std::vector<std::string> labels;
          outcomes.reserve(wait->subjects.size());
          for (const syntax::NodePtr& subject : wait->subjects) {
            bool waitable = IsOperation(subject.get()) ||
                            subject->kind == NodeKind::kWait ||
                            subject->kind == NodeKind::kClose;
            const auto named_subject = [&](const Node* node) {
              const auto* name = syntax::As<syntax::Name>(node);
              if (name == nullptr) {
                return false;
              }
              const auto index = Lookup(scope, name->name);
              return index == kNoSymbol ||
                     (resolved_.symbols[index].kind != SymbolKind::kValue &&
                      resolved_.symbols[index].kind != SymbolKind::kHeader &&
                      resolved_.symbols[index].kind !=
                          SymbolKind::kLoopVariable &&
                      resolved_.symbols[index].kind != SymbolKind::kCarry &&
                      resolved_.symbols[index].kind != SymbolKind::kNodeMap);
            };
            waitable |= named_subject(subject.get());
            if (const auto* attr = syntax::As<syntax::Attr>(subject.get());
                attr != nullptr) {
              if (attr->base->kind == NodeKind::kCallExpression) {
                waitable = true;
              }
              if (const auto* base = syntax::As<syntax::Name>(attr->base.get());
                  base != nullptr) {
                const auto index = Lookup(scope, base->name);
                waitable = index == kNoSymbol ||
                           resolved_.symbols[index].kind == SymbolKind::kCall;
              }
            }
            if (!waitable) {
              Report("flow.form.not-waitable",
                     "A completion race needs actions, node-backed streams, or "
                     "completion barriers; this expression is data or a "
                     "pipeline without a completion event.",
                     subject->extent, Severity::kError, Family::kForm);
            }
            outcomes.push_back(ResolveOutcome(subject.get(), scope));
            labels.push_back(SubjectLabel(outcomes.back().label));
          }
          const std::string written =
              absl::StrCat("wait ", wait->race ? "first" : "all", " of ",
                           absl::StrJoin(labels, ", "));
          StepPlan step;
          step.kind = "wait";
          step.label = written.substr(std::strlen("wait "));
          step.source = step.label;
          step.timeout = wait->timeout;
          step.location = wait->location;
          graph::StepId made = graph::kNone;
          if (builder_ != nullptr) {
            made = NewStep(graph::StepKind::kWait, written, wait->location);
            graph::Step& one = builder_->step(made);
            one.race = wait->race;
            one.timeout = wait->timeout;
            bool tolerant = true;
            for (const Ref& outcome : outcomes) {
              one.subjects.push_back(outcome.node);
              one.subject_tolerant.push_back(outcome.tolerant);
              if (!outcome.tolerant) {
                tolerant = false;
              }
            }
            // The first outcome is also the step's own, so everything that
            // reads a wait's outcome keeps working: with `first`, that is
            // whichever won, and the runtime fills it in.
            one.outcome = outcomes.front().node;
            one.tolerant = tolerant;
            if (wait->race) {
              builder_->step(made).winner = WinnerOfStep(made);
            }
          }
          step.after = ResolveAfter(wait->after, scope, made);
          ResolveDeadline(wait->deadline.get(), scope, made, step);
          ResolveDeadline(wait->timeout_expression.get(), scope, made, step,
                          true);
          const graph::RefId winner =
              made == graph::kNone ? graph::kNone : builder_->step(made).winner;
          steps.push_back(std::move(step));
          // `-> n` writes the number of whichever won, which only a race has.
          for (const syntax::NodePtr& target : wait->targets) {
            if (!wait->race) {
              Report("flow.form.wait-all-has-no-value",
                     "'wait all of' waits for every one of them, so there is "
                     "no single winner to write. 'wait first of a, b -> n' "
                     "writes which one it was.",
                     target->location);
              break;
            }
            const Ref destination = ResolveDestination(target.get(), scope);
            StepPlan pipe;
            pipe.kind = "pipe";
            pipe.label = absl::StrCat(written, " -> ", destination.label);
            pipe.source = written;
            pipe.destination = destination.label;
            pipe.location = wait->location;
            if (builder_ != nullptr) {
              const graph::StepId piped =
                  NewStep(graph::StepKind::kPipe, pipe.label, wait->location);
              graph::Step& one = builder_->step(piped);
              one.source = winner;
              one.destination = destination.node;
            }
            steps.push_back(std::move(pipe));
          }
          return;
        }
        const Ref outcome = ResolveOutcome(wait->subject.get(), scope);
        StepPlan step;
        step.kind = "wait";
        step.label = outcome.label;
        step.source = outcome.label;
        step.timeout = wait->timeout;
        step.location = wait->location;
        graph::StepId made = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kWait,
                         absl::StrCat("wait ", SubjectLabel(outcome.label)),
                         wait->location);
          graph::Step& one = builder_->step(made);
          one.outcome = outcome.node;
          one.timeout = wait->timeout;
          one.tolerant = outcome.tolerant;
        }
        step.after = ResolveAfter(wait->after, scope, made);
        ResolveDeadline(wait->deadline.get(), scope, made, step);
        ResolveDeadline(wait->timeout_expression.get(), scope, made, step,
                        true);
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kClose: {
        const auto* drain = syntax::As<syntax::Close>(statement);
        const Ref target = ResolveDestination(drain->target.get(), scope);
        StepPlan step;
        step.kind = "close";
        step.label = target.label;
        step.destination = target.label;
        step.location = drain->location;
        graph::StepId made = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kClose,
                         absl::StrCat("close ", target.label), drain->location);
          builder_->step(made).outcome = StatusOfRef(target.node, target.label);
        }
        step.after = ResolveAfter(drain->after, scope, made);
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kCancel: {
        const auto* cancel = syntax::As<syntax::Cancel>(statement);
        size_t called = kNoSymbol;
        if (cancel->name.expression != nullptr &&
            IsOperation(cancel->name.expression.get())) {
          called = ResolveOperation(cancel->name.expression.get(), scope);
          if (resolved_.symbols[called].kind != SymbolKind::kCall &&
              resolved_.symbols[called].kind != SymbolKind::kSink) {
            Report("flow.name.not-a-call",
                   "Cancellation requires an action handle.",
                   cancel->name.location);
            called = kNoSymbol;
          }
        } else {
          const auto index = Lookup(scope, cancel->name.text);
          called = index != kNoSymbol &&
                           resolved_.symbols[index].kind == SymbolKind::kSink
                       ? index
                       : ExpectCall(cancel->name.text, cancel->location, scope);
        }
        StepPlan step;
        step.kind = "cancel";
        step.label = cancel->name.text;
        step.location = cancel->location;
        graph::StepId made = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kCancel,
                         absl::StrCat("cancel ", cancel->name.text),
                         cancel->location);
          builder_->step(made).target = called == kNoSymbol
                                            ? graph::kNone
                                            : resolved_.symbols[called].step;
        }
        step.after = ResolveAfter(cancel->after, scope, made);
        CheckReachedByChoice("flow.form.unconditional-cancel", "cancel",
                             cancel->after, cancel->location);
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kAbort: {
        const auto* abort = syntax::As<syntax::Abort>(statement);
        // A destination, not an outcome: only a node this flow writes can be
        // aborted by it. Aborting one it merely reads would be telling somebody
        // else's producer how its stream ended.
        const Ref target = ResolveDestination(abort->target.get(), scope);
        std::string code_name;
        const graph::ExprId code =
            ResolveFailCode(abort->code.get(), scope, &code_name);
        const graph::ExprId message =
            abort->message == nullptr
                ? graph::kNone
                : ResolveExpression(abort->message.get(), scope, false);
        StepPlan step;
        step.kind = "abort";
        step.label = absl::StrCat("abort ", target.label);
        step.destination = target.label;
        step.location = abort->location;
        graph::StepId made = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kAbort, step.label, abort->location);
          graph::Step& one = builder_->step(made);
          one.destination = target.node;
          one.code = code;
          one.code_name = code_name;
          one.message = message;
        }
        step.after = ResolveAfter(abort->after, scope, made);
        CheckReachedByChoice(
            "flow.form.unconditional-abort", "abort", abort->after,
            abort->location,
            HasDataDependency(abort->code.get(), scope) ||
                HasDataDependency(abort->message.get(), scope));
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kFail: {
        const auto* fail = syntax::As<syntax::Fail>(statement);
        std::string code_name;
        const graph::ExprId code =
            ResolveFailCode(fail->code.get(), scope, &code_name);
        const graph::ExprId message =
            fail->message == nullptr
                ? graph::kNone
                : ResolveExpression(fail->message.get(), scope, false);
        StepPlan step;
        step.kind = "fail";
        step.label = Unparse(fail->code.get());
        step.location = fail->location;
        graph::StepId made = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kFail, "fail", fail->location);
          builder_->step(made).code = code;
          builder_->step(made).code_name = code_name;
          builder_->step(made).message = message;
        }
        step.after = ResolveAfter(fail->after, scope, made);
        CheckReachedByChoice("flow.form.unconditional-fail", "fail",
                             fail->after, fail->location,
                             HasDataDependency(fail->code.get(), scope) ||
                                 HasDataDependency(fail->message.get(), scope));
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kLog: {
        const auto* log = syntax::As<syntax::Log>(statement);
        const bool formatted = log->tail.has_format;
        const graph::LogTail tail =
            ResolveLogTail(log->tail, scope, /*allow_it=*/false, log->location);
        StepPlan step;
        step.kind = formatted ? "logf" : "log";
        const syntax::Node* subject = log->tail.arguments.empty()
                                          ? nullptr
                                          : log->tail.arguments.front().get();
        step.label = formatted ? tail.format : Unparse(subject);
        step.location = log->location;
        graph::StepId made = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kLog, step.kind, log->location);
          builder_->step(made).log = tail;
        }
        step.after = ResolveAfter(log->after, scope, made);
        CheckReachedByChoice(
            formatted ? "flow.form.unconditional-logf"
                      : "flow.form.unconditional-log",
            step.kind, log->after, log->location,
            std::any_of(log->tail.arguments.begin(), log->tail.arguments.end(),
                        [&](const auto& argument) {
                          return HasDataDependency(argument.get(), scope);
                        }));
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kNodes:
        return ResolveNodes(syntax::As<syntax::Nodes>(statement), scope, steps);
      case NodeKind::kForEach: {
        const auto* loop = syntax::As<syntax::ForEach>(statement);
        const bool concurrent =
            loop->parallel > 1 || loop->parallel_expression != nullptr;
        const auto parallel_expression =
            ResolveExpression(loop->parallel_expression.get(), scope, false);
        Ref source = ResolvePipeline(*loop->pipeline, scope);
        const bool literal_items = loop->pipeline->stages.empty() &&
                                   syntax::As<syntax::ListLiteral>(
                                       loop->pipeline->source.get()) != nullptr;
        if (literal_items) {
          // Bare list headers iterate one level through the flatten stage.
          graph::Stage flatten;
          flatten.name = "flatten";
          absl::StrAppend(&source.label, " | flatten");
          source.node = Derive(source.node, std::move(flatten), source.label);
          source.kind = Ref::Kind::kDerived;
        }
        StepPlan step;
        step.kind = "for";
        step.label = Label("for");
        step.source = source.label;
        step.location = loop->location;
        graph::StepId made = graph::kNone;
        graph::BodyId inner_body = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kForEach, step.label, loop->location);
          inner_body =
              builder_->AddBody(absl::StrCat(step.label, ".body"), body_, made);
          graph::Step& one = builder_->step(made);
          one.source = source.node;
          one.parallel = loop->parallel;
          one.parallel_expression = parallel_expression;
          one.bodies.push_back(inner_body);
        }
        const graph::RefId item = BoundRef(inner_body, made, "item");
        const graph::RefId index = BoundRef(inner_body, made, "index");
        if (builder_ != nullptr) {
          graph::Step& one = builder_->step(made);
          one.item = item;
          one.index = index;
        }
        Scope inner;
        inner.parent = &scope;
        const graph::BodyId outer_body = body_;
        body_ = inner_body;
        for (size_t at = 0; at < loop->variables.size(); ++at) {
          Symbol variable;
          variable.kind = SymbolKind::kLoopVariable;
          variable.name = loop->variables[at].text;
          variable.location = loop->variables[at].location;
          if (loop->variables.size() == 1) {
            if (!literal_items) {
              variable.value_type = InferredType(loop->pipeline.get(), scope);
            }
            if (variable.value_type.has_value()) {
              variable.shape = variable.value_type->ToString();
            }
          }
          variable.ref = loop->variables.size() == 1
                             ? item
                             : Derive(item,
                                      AtStage(syntax::Constant::Integer(
                                          static_cast<long long>(at))),
                                      absl::StrCat(loop->variables[at].text));
          Define(inner, variable);
        }
        body_ = outer_body;
        DefineIndex(inner, loop->location, index);
        CaptureVariables(scope, inner, inner_body);
        // A `for` is a loop, so `until`/`while` may end it. It carries nothing,
        // which is what `carries_a_value` tells `<-`.
        LoopState state;
        state.label = step.label;
        state.step = made;
        state.carries_a_value = false;
        state.parallel = concurrent ? std::max(loop->parallel, 2) : 1;
        loops_.push_back(state);
        step.bodies.push_back(
            ResolveStatements(loop->body, inner, inner_body, true));
        if (loop->termination != nullptr) {
          const auto outer = body_;
          body_ = inner_body;
          ResolveStatement(loop->termination.get(), inner, step.bodies.back());
          body_ = outer;
        }
        loops_.pop_back();
        step.after = ResolveAfter(loop->after, scope, made);
        JoinVariables(scope, {{&inner, inner_body}}, made, concurrent);
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kRepeat: {
        const auto* repeat = syntax::As<syntax::Repeat>(statement);
        if (repeat->start != nullptr) {
          Constant(repeat->start.get());
        }
        StepPlan step;
        step.kind = "repeat";
        step.label = Label("repeat");
        step.location = repeat->location;
        graph::StepId made = graph::kNone;
        graph::BodyId inner_body = graph::kNone;
        if (builder_ != nullptr) {
          made =
              NewStep(graph::StepKind::kRepeat, step.label, repeat->location);
          inner_body =
              builder_->AddBody(absl::StrCat(step.label, ".body"), body_, made);
          graph::Step& one = builder_->step(made);
          one.bodies.push_back(inner_body);
          one.max_iterations = repeat->max_iterations;
          if (repeat->start != nullptr) {
            if (std::optional<syntax::Constant> start =
                    syntax::ConstantValue(repeat->start.get());
                start.has_value()) {
              one.start = *std::move(start);
            }
          }
        }
        const graph::RefId carry_ref =
            repeat->variable.Empty() ? graph::kNone
                                     : BoundRef(inner_body, made, "carry");
        const graph::RefId index = BoundRef(inner_body, made, "index");
        if (builder_ != nullptr) {
          graph::Step& one = builder_->step(made);
          one.carry = carry_ref;
          one.index = index;
        }
        Scope inner;
        inner.parent = &scope;
        LoopState state;
        state.label = step.label;
        state.step = made;
        if (!repeat->variable.Empty()) {
          Symbol carry;
          carry.kind = SymbolKind::kCarry;
          carry.name = repeat->variable.text;
          carry.location = repeat->variable.location;
          carry.ref = carry_ref;
          Define(inner, carry);
          state.carries = repeat->variable.text;
          state.has_carry_name = true;
        }
        DefineIndex(inner, repeat->location, index);
        CaptureVariables(scope, inner, inner_body);
        loops_.push_back(state);
        step.bodies.push_back(
            ResolveStatements(repeat->body, inner, inner_body, true));
        if (repeat->termination != nullptr) {
          const auto outer = body_;
          body_ = inner_body;
          ResolveStatement(repeat->termination.get(), inner,
                           step.bodies.back());
          body_ = outer;
        }
        // Read before the pop: `until` sets the flag on the entry the body was
        // resolved against, not on the copy pushed in.
        const bool stopped = loops_.back().stopped;
        loops_.pop_back();
        JoinVariables(scope, {{&inner, inner_body}}, made);
        // Nothing ends it.
        if (!stopped && !repeat->max_iterations.has_value()) {
          Report("flow.form.unbounded-repeat",
                 absl::StrCat(
                     "Nothing ends this 'repeat': it has no 'until', no "
                     "'while' and no 'max'. Say when it stops, or bound it "
                     "with 'max n'."),
                 repeat->location, Severity::kError, Family::kForm);
        }
        // After the repeat's own NewStep, for the reason the `for` case gives.
        step.after = ResolveAfter(repeat->after, scope, made);
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kCarry: {
        const auto* carry = syntax::As<syntax::Carry>(statement);
        if (loops_.empty()) {
          Report("flow.barrier.carry-outside-repeat",
                 "'<-' carries a value into the next pass of a 'repeat', and "
                 "there is no repeat here.",
                 carry->location, Severity::kError, Family::kBarrier);
          ResolvePipeline(*carry->pipeline, scope);
          return;
        }
        LoopState& state = loops_.back();
        if (!state.carries_a_value) {
          // A `for` is the innermost loop here. It takes its value from its
          // stream, so there is nothing for a pass to hand the next one.
          Report("flow.barrier.carry-outside-repeat",
                 absl::StrCat(
                     "'<-' carries a value into the next pass of a 'repeat', "
                     "and the loop here is ",
                     state.label,
                     ", a 'for': every pass takes its value from the stream, "
                     "so there is nothing for one pass to hand the next."),
                 carry->location, Severity::kError, Family::kBarrier);
          ResolvePipeline(*carry->pipeline, scope);
          return;
        }
        if (!state.has_carry_name || state.carries != carry->name.text) {
          Report("flow.barrier.wrong-carry",
                 absl::StrCat(
                     "This repeat carries ",
                     state.has_carry_name ? Quoted(state.carries) : "'nothing'",
                     ", not ", Quoted(carry->name.text), "."),
                 carry->location, Severity::kError, Family::kBarrier);
        } else if (state.carried) {
          Report("flow.barrier.duplicate-carry",
                 absl::StrCat(Quoted(carry->name.text), " is already carried."),
                 carry->location, Severity::kError, Family::kBarrier);
        }
        const graph::StepId owner = state.step;
        state.carried = true;
        const Ref source = ResolvePipeline(*carry->pipeline, scope);
        StepPlan step;
        step.kind = "capture";
        step.label = absl::StrCat(carry->name.text, " <- ", source.label);
        step.source = source.label;
        step.location = carry->location;
        if (builder_ != nullptr) {
          const graph::StepId made = NewStep(graph::StepKind::kCapture,
                                             "capture carry", carry->location);
          builder_->step(made).source = source.node;
          builder_->step(made).slot = "carry";
          if (owner != graph::kNone) {
            builder_->step(owner).carry_source = source.node;
          }
        }
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kUntil: {
        const auto* until = syntax::As<syntax::Until>(statement);
        if (loops_.empty()) {
          Report("flow.barrier.until-outside-repeat",
                 "'until'/'while' ends a loop, and there is no 'repeat' or "
                 "'for' here.",
                 until->location, Severity::kError, Family::kBarrier);
        } else if (loops_.back().stopped) {
          Report("flow.barrier.duplicate-until",
                 absl::StrCat(loops_.back().label,
                              " already has a stop condition."),
                 until->location, Severity::kError, Family::kBarrier);
        } else if (loops_.back().parallel > 1) {
          Report("flow.barrier.until-parallel",
                 absl::StrCat(
                     loops_.back().label,
                     " runs its passes in parallel, so there is no 'the pass "
                     "that just finished' for 'until' to ask about. Drop the "
                     "'parallel', or filter the stream with 'first n' or "
                     "'where' instead."),
                 until->location, Severity::kError, Family::kBarrier);
        } else {
          loops_.back().stopped = true;
        }
        const graph::StepId owner =
            loops_.empty() ? graph::kNone : loops_.back().step;
        const graph::ExprId condition =
            ResolveExpression(until->condition.get(), scope, false);
        if (builder_ != nullptr && owner != graph::kNone) {
          graph::Step& one = builder_->step(owner);
          one.condition = condition;
          one.stop_when = until->stop_when;
          // Each stream the condition reads is captured inside the pass,
          // because the question is asked once the pass is over and the streams
          // are gone by then.
          const std::vector<graph::RefId> read = builder_->expr(condition).refs;
          for (const graph::RefId ref : read) {
            const graph::StepId made =
                NewStep(graph::StepKind::kCapture, "capture condition",
                        until->location);
            builder_->step(made).source = ref;
            builder_->step(made).slot = absl::StrCat("condition:", ref);
          }
        }
        return;
      }
      case NodeKind::kIf: {
        const auto* branch = syntax::As<syntax::If>(statement);
        const graph::ExprId condition =
            ResolveExpression(branch->condition.get(), scope, false);
        std::optional<syntax::TypeExpression> option_type;
        if (!branch->binding.Empty()) {
          option_type = InferredType(branch->condition.get(), scope);
          if (!option_type.has_value() || option_type->name != "option") {
            Report("flow.form.optional-binding",
                   "An optional binding requires "
                   "an option. Use 'try take' or a validated std::option "
                   "conversion.",
                   branch->condition->location, Severity::kError,
                   Family::kForm);
          }
          if (Lookup(scope, branch->binding.text) != kNoSymbol) {
            Report("flow.name.taken",
                   "An optional binding must use a fresh "
                   "branch-local name; it cannot shadow an enclosing name.",
                   branch->binding.location, Severity::kError, Family::kForm);
          }
        }
        StepPlan step;
        step.kind = "if";
        step.optional_binding = branch->binding.text;
        step.label = Label("if");
        step.location = branch->location;
        graph::StepId made = graph::kNone;
        graph::BodyId then_body = graph::kNone;
        graph::BodyId else_body = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kIf, step.label, branch->location);
          then_body =
              builder_->AddBody(absl::StrCat(step.label, ".then"), body_, made);
          else_body =
              builder_->AddBody(absl::StrCat(step.label, ".else"), body_, made);
          graph::Step& one = builder_->step(made);
          one.condition = condition;
          one.bodies.push_back(then_body);
          one.bodies.push_back(else_body);
        }
        Scope then_scope;
        then_scope.parent = &scope;
        CaptureVariables(scope, then_scope, then_body);
        if (!branch->binding.Empty()) {
          graph::Ref payload;
          payload.kind = graph::RefKind::kBound;
          payload.owner = then_body;
          payload.role = "optional-binding";
          payload.bound_by = made;
          payload.label = branch->binding.text;
          payload.stage.type = option_type;
          const auto ref = builder_->AddRef(std::move(payload));
          builder_->step(made).item = ref;
          Symbol local;
          local.kind = SymbolKind::kValue;
          local.name = branch->binding.text;
          local.location = branch->binding.location;
          local.visibility = branch->binding_scope;
          local.ref = ref;
          if (option_type.has_value() && !option_type->parameters.empty()) {
            local.value_type = option_type->parameters.front();
            local.shape = local.value_type->ToString();
          }
          Define(then_scope, std::move(local));
        }
        step.bodies.push_back(
            ResolveStatements(branch->then_body, then_scope, then_body, true));
        Scope else_scope;
        else_scope.parent = &scope;
        CaptureVariables(scope, else_scope, else_body);
        step.bodies.push_back(
            ResolveStatements(branch->else_body, else_scope, else_body, true));
        JoinVariables(
            scope, {{&then_scope, then_body}, {&else_scope, else_body}}, made);
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kError:
        // The parser already said what it could not read; nothing to add.
        return;
      default:
        Report("flow.syntax.unexpected",
               absl::StrCat("Cannot run a ",
                            syntax::NodeKindName(statement->kind), " here."),
               statement->location, Severity::kError, Family::kSyntax);
        return;
    }
  }

  void DefineIndex(Scope& scope, const Location& location,
                   graph::RefId ref = graph::kNone) {
    Symbol index;
    index.kind = SymbolKind::kLoopVariable;
    index.name = "index";
    index.location = location;
    index.implicit = true;
    index.ref = ref;
    Define(scope, index);
  }

  static std::string PatternOfExpression(const Node* expression) {
    if (const auto* value = syntax::As<syntax::PipelineValue>(expression);
        value != nullptr) {
      return PatternOf(*value->pipeline);
    }
    if (const auto* typed = syntax::As<syntax::TypedValue>(expression);
        typed != nullptr) {
      return PatternOfExpression(typed->value.get());
    }
    const auto* function = syntax::As<syntax::Builtin>(expression);
    if (function == nullptr || function->name != "match" ||
        function->args.empty()) {
      return "";
    }
    const auto value = syntax::ConstantValue(function->args.front().get());
    return value.has_value() && value->kind == syntax::Constant::Kind::kString
               ? value->text
               : "";
  }

  static std::string PatternOf(const syntax::Pipeline& pipeline) {
    if (!pipeline.stages.empty()) {
      const syntax::Stage& last = *pipeline.stages.back();
      return vocabulary::Canonical(last.name) == "match" ? last.text : "";
    }
    return PatternOfExpression(pipeline.source.get());
  }

  void OrderExtraction(const Node& value, graph::StepId made) {
    syntax::VisitSubtree(value, [&](const Node& node) {
      const auto* take = syntax::As<syntax::PipelineValue>(&node);
      if (take == nullptr || !take->take) {
        return;
      }
      const auto found = extraction_sources_.find(take);
      if (found == extraction_sources_.end()) {
        return;
      }
      if (dynamic_extractions_.contains(found->second)) {
        Report(
            "flow.sequence.conflicting-access",
            "A per-item take competes with another extraction of this stream. "
            "Name values in a sequential for loop to order the reads.",
            node.location);
      }
      const auto prior = last_extraction_.find(found->second);
      if (prior != last_extraction_.end() && prior->second != made) {
        builder_->step(made).after.push_back(prior->second);
      }
      last_extraction_[found->second] = made;
      body_extractions_[body_].insert(found->second);
    });
  }

  std::pair<graph::RefId, graph::StepId> InitializeValue(
      graph::RefId source, const Location& location, std::string label,
      const Node* extraction = nullptr, bool deferred = false) {
    const auto step = NewStep(graph::StepKind::kCapture, label, location);
    auto& capture = builder_->step(step);
    capture.source = source;
    capture.deferred = deferred;
    capture.slot = absl::StrCat("initialize:", step);
    if (extraction != nullptr) {
      OrderExtraction(*extraction, step);
    }
    graph::Ref value;
    value.kind = graph::RefKind::kBound;
    value.role = "initialized-value";
    value.owner = body_;
    value.bound_by = step;
    value.source = source;
    value.unary = true;
    value.label = std::move(label);
    return {builder_->AddRef(std::move(value)), step};
  }

  std::optional<syntax::TypeExpression> InferredType(const Node* expression,
                                                     const Scope& scope) {
    const auto info = DescribeExpression(expression, scope);
    if (!info.type.empty()) {
      syntax::TypeExpression inferred;
      inferred.name = info.type;
      if (info.type.find('[') != std::string::npos) {
        auto parsed_type = ParseTypeExpression(
            absl::StrReplaceAll(info.type, {{".", "::"}}), {}, &budget_);
        if (parsed_type.ok()) {
          inferred = *std::move(parsed_type);
          std::vector<syntax::TypeExpression*> pending{&inferred};
          while (!pending.empty()) {
            auto* type = pending.back();
            pending.pop_back();
            if (type->name.starts_with("std::")) {
              type->name.erase(0, 5);
            } else if (type->name.find("::") != std::string::npos &&
                       known_.Dto(type->name) == nullptr) {
              type->name = absl::StrReplaceAll(type->name, {{"::", "."}});
            }
            for (auto& parameter : type->parameters) {
              pending.push_back(&parameter);
            }
          }
        }
      }
      return inferred;
    }
    return std::nullopt;
  }

  std::optional<syntax::TypeExpression> AssignmentType(
      const syntax::Word& name,
      const std::optional<syntax::TypeExpression>& annotated, Scope& scope) {
    const auto previous = Lookup(scope, name.text);
    if (previous == kNoSymbol) {
      return annotated;
    }
    const auto& symbol = resolved_.symbols[previous];
    if (symbol.kind != SymbolKind::kValue) {
      Report("flow.name.taken",
             absl::StrCat(
                 Quoted(name.text),
                 " is already taken in this scope and cannot be reassigned."),
             name.location);
      return annotated;
    }
    if (annotated.has_value() && symbol.value_type.has_value() &&
        CanonicalPayloadType(annotated->ToString()) !=
            CanonicalPayloadType(symbol.value_type->ToString())) {
      Report("flow.form.field-type-mismatch",
             "Reassignment cannot change a variable's declared type.",
             name.location);
    }
    return symbol.value_type.has_value() ? symbol.value_type : annotated;
  }

  graph::RefId TransferSink(graph::StepId step) {
    if (builder_->step(step).sink != graph::kNone) {
      return builder_->step(step).sink;
    }
    const auto& operation = builder_->step(step);
    graph::Ref ref;
    ref.kind = graph::RefKind::kSink;
    ref.label = operation.label;
    ref.owner = operation.body;
    ref.writable = true;
    ref.subject_step = step;
    if (operation.destination != graph::kNone &&
        builder_->ref(operation.destination).kind == graph::RefKind::kSink) {
      ref.subject = operation.destination;
      ref.sources = builder_->ref(operation.destination).sources;
    } else {
      for (const auto target : graph::TransferTargets(operation)) {
        ref.sources.push_back(target);
      }
    }
    const auto id = builder_->AddRef(std::move(ref));
    builder_->step(step).sink = id;
    return id;
  }

  graph::StepId ResolveTransfer(const syntax::Pipe& pipe, Scope& scope,
                                std::vector<StepPlan>& steps) {
    std::vector<const syntax::Pipeline*> serial;
    const syntax::Pipe* leaf = &pipe;
    while (true) {
      serial.push_back(leaf->pipeline.get());
      const auto* next = leaf->targets.size() == 1
                             ? syntax::As<syntax::Pipe>(leaf->targets[0].get())
                             : nullptr;
      if (next == nullptr || next->tolerant || !next->after.empty()) {
        break;
      }
      leaf = next;
    }
    std::vector<Ref> sources;
    std::vector<graph::TransferJoin> joins;
    for (const auto* pipeline : std::views::reverse(serial)) {
      const auto first_expr = builder_->flow().exprs.size();
      const auto first_step = builder_->flow().steps.size();
      sources.push_back(ResolvePipeline(*pipeline, scope));
      for (size_t id = first_step; id < builder_->flow().steps.size(); ++id) {
        const auto& step = builder_->step(id);
        if (step.kind == graph::StepKind::kCall && step.body == body_) {
          joins.push_back({sources.back().node, id, false});
        }
      }
      for (size_t id = first_expr; id < builder_->flow().exprs.size(); ++id) {
        auto& expression = builder_->expr(id);
        if (expression.activation != graph::kNone &&
            builder_->flow().bodies[expression.activation].parent == body_) {
          expression.pipeline_owned = true;
          joins.push_back({sources.back().node, id, true});
        }
      }
    }
    const auto first_destination_step = builder_->flow().steps.size();
    std::vector<Ref> targets;
    bool discard = false;
    for (const auto& target : leaf->targets) {
      discard |= IsDiscard(target.get());
      targets.push_back(IsDiscard(target.get())
                            ? Ref{Ref::Kind::kUnknown, "_"}
                            : ResolveDestination(target.get(), scope));
    }
    if (targets.size() > 1 &&
        std::any_of(targets.begin(), targets.end(), [&](const Ref& ref) {
          return ref.node != graph::kNone &&
                 builder_->ref(ref.node).kind == graph::RefKind::kSink;
        })) {
      Report("flow.form.continuation-grouping",
             "Broadcast terminal destinations directly; attach to a single "
             "continuation in a separate transfer.",
             pipe.location, Severity::kError, Family::kForm);
    }
    StepPlan plan;
    plan.kind = "pipe";
    plan.location = pipe.location;
    plan.tolerant = pipe.tolerant;
    for (const auto& ref : sources) {
      plan.serial_sources.push_back(ref.label);
    }
    for (const auto& ref : targets) {
      plan.destinations.push_back(ref.label);
    }
    plan.source = absl::StrJoin(plan.serial_sources, " then ");
    plan.destination = absl::StrJoin(plan.destinations, ", ");
    if (targets.size() == 1 && targets[0].node != graph::kNone &&
        builder_->ref(targets[0].node).kind == graph::RefKind::kSink) {
      plan.continuation_parent = targets[0].label;
      plan.destinations.clear();
      for (const auto physical : builder_->ref(targets[0].node).sources) {
        plan.destinations.push_back(
            physical == graph::kNone ? "_" : builder_->ref(physical).label);
      }
    }
    plan.label = absl::StrCat(plan.source, " -> ", plan.destination);
    plan.after = ResolveAfter(pipe.after, scope);
    const auto held = after_waits_;
    const auto made =
        NewStep(graph::StepKind::kPipe, plan.label, pipe.location);
    auto& operation = builder_->step(made);
    operation.after = held;
    operation.discard =
        discard &&
        std::all_of(targets.begin(), targets.end(),
                    [](const Ref& ref) { return ref.node == graph::kNone; });
    operation.tolerant = pipe.tolerant;
    operation.source = sources.empty() ? graph::kNone : sources.front().node;
    operation.destination = targets.empty() ? graph::kNone : targets[0].node;
    std::vector<graph::StepId> destination_calls;
    for (size_t id = first_destination_step; id < made; ++id) {
      const auto& step = builder_->step(id);
      if (step.kind == graph::StepKind::kCall && step.body == body_) {
        destination_calls.push_back(id);
      }
    }
    if (sources.size() > 1 || targets.size() > 1 || !joins.empty() ||
        !destination_calls.empty()) {
      operation.transfer = std::make_shared<graph::TransferPlan>();
      if (sources.size() > 1) {
        for (const auto& ref : sources) {
          operation.transfer->sources.push_back(ref.node);
        }
      }
      if (targets.size() > 1) {
        for (const auto& ref : targets) {
          operation.transfer->destinations.push_back(ref.node);
        }
      }
      operation.transfer->joins = std::move(joins);
      operation.transfer->destination_calls = std::move(destination_calls);
    }
    steps.push_back(std::move(plan));
    return made;
  }

  void ResolveDestructuring(const syntax::Bind& bind, Scope& scope,
                            std::vector<StepPlan>& steps) {
    const auto source = ResolveSource(bind.value.get(), scope);
    std::vector<syntax::Word> names{bind.name};
    names.insert(names.end(), bind.names.begin(), bind.names.end());
    absl::flat_hash_set<std::string> seen;
    std::vector<std::optional<syntax::TypeExpression>> types;
    for (const auto& name : names) {
      if (!seen.insert(name.text).second) {
        Report("flow.name.taken", "A destructuring pattern repeats a name.",
               name.location);
      }
      types.push_back(AssignmentType(name, bind.type, scope));
      if (!types.back().has_value()) {
        const Node* member = nullptr;
        if (const auto* list =
                syntax::As<syntax::ListLiteral>(bind.value.get());
            list != nullptr && types.size() <= list->items.size()) {
          member = list->items[types.size() - 1].get();
        }
        if (const auto* object =
                syntax::As<syntax::ObjectLiteral>(bind.value.get());
            object != nullptr) {
          for (const auto& [key, value] : object->pairs) {
            if (key == name.text) {
              member = value.get();
            }
          }
        }
        if (member != nullptr) {
          types.back() = InferredType(member, scope);
        } else {
          const auto info = DescribeExpression(bind.value.get(), scope);
          if (const auto* shape = known_.Dto(info.type); shape != nullptr) {
            if (const auto* field = shape->Field(name.text); field != nullptr) {
              types.back() = field->syntax_type;
            }
          }
        }
      }
      if (types.back().has_value()) {
        PortType(*types.back());
      }
    }
    graph::RefId validated = source.node;
    graph::StepId initialized = graph::kNone;
    if (builder_ != nullptr) {
      graph::Stage validation;
      validation.name = "destructure";
      validation.destructured_types = types;
      for (const auto& name : names) {
        validation.destructured.push_back(name.text);
      }
      validated = Derive(source.node, std::move(validation), "destructuring");
      const auto captured =
          InitializeValue(validated, bind.location, "destructuring");
      validated = captured.first;
      initialized = captured.second;
    }
    for (size_t index = 0; index < names.size(); ++index) {
      Symbol symbol;
      symbol.name = names[index].text;
      symbol.location = names[index].location;
      symbol.kind = SymbolKind::kValue;
      symbol.readable = true;
      symbol.step = initialized;
      symbol.value_type = types[index];
      symbol.shape = types[index].has_value() ? types[index]->ToString() : "";
      if (builder_ != nullptr) {
        auto stage = AtStage(names[index].text);
        symbol.ref = Derive(validated, std::move(stage), names[index].text);
      }
      Define(scope, std::move(symbol));
    }
    StepPlan plan;
    plan.kind = "initialize";
    plan.label = "destructuring";
    plan.source = source.label;
    plan.location = bind.location;
    steps.push_back(std::move(plan));
  }

  void ResolveBind(const syntax::Bind* bind, Scope& scope,
                   std::vector<StepPlan>& steps) {
    if (!bind->names.empty()) {
      ResolveDestructuring(*bind, scope, steps);
      return;
    }
    const auto previous = Lookup(scope, bind->name.text);
    const auto assigned_type = AssignmentType(bind->name, bind->type, scope);
    const Node* value = bind->value.get();
    if (const auto* pipe = syntax::As<syntax::Pipe>(value); pipe != nullptr) {
      const auto step = ResolveTransfer(*pipe, scope, steps);
      builder_->step(step).label = bind->name.text;
      Symbol handle;
      handle.kind = SymbolKind::kSink;
      handle.name = bind->name.text;
      handle.location = bind->name.location;
      handle.step = step;
      handle.ref = TransferSink(step);
      handle.writable = true;
      handle.readable = false;
      handle.tolerant = pipe->tolerant;
      Define(scope, std::move(handle));
      steps.back().label = bind->name.text;
      return;
    }
    if (const auto* name = syntax::As<syntax::Name>(value); name != nullptr) {
      if (auto* source = Find(scope, name->name);
          source != nullptr && source->kind == SymbolKind::kSink) {
        DescribeExpression(value, scope);
        ++source->reads;
        Symbol alias = *source;
        alias.name = bind->name.text;
        alias.location = bind->name.location;
        alias.reads = alias.writes = alias.status_reads = 0;
        Define(scope, std::move(alias));
        StepPlan plan;
        plan.kind = "alias";
        plan.label = bind->name.text;
        plan.source = name->name;
        plan.location = bind->location;
        steps.push_back(std::move(plan));
        return;
      }
    }

    if (bind->type.has_value() && bind->type->name == "stream") {
      const auto* typed = syntax::As<syntax::TypedValue>(value);
      if (typed == nullptr || bind->type->parameters.size() != 1) {
        Report("flow.form.field-type-mismatch",
               "A stream binding requires one payload type.", bind->location,
               Severity::kError, Family::kForm);
        return;
      }
      const auto& payload = bind->type->parameters.front();
      PortType(payload);
      auto source = ResolveSource(typed->value.get(), scope);
      if (source.kind == Ref::Kind::kValue ||
          source.kind == Ref::Kind::kStatus) {
        Report("flow.form.field-type-mismatch",
               "A stream binding requires a stream source.", bind->location,
               Severity::kError, Family::kForm);
      }
      Symbol symbol;
      symbol.name = bind->name.text;
      symbol.location = bind->name.location;
      symbol.kind = SymbolKind::kInputPort;
      symbol.readable = true;
      symbol.shape = payload.ToString();
      symbol.value_type = payload;
      if (builder_ != nullptr) {
        graph::Stage conversion;
        conversion.name = "coerce";
        conversion.type = payload;
        symbol.ref = Derive(source.node, std::move(conversion), symbol.name);
      }
      Define(scope, std::move(symbol));
      StepPlan plan;
      plan.kind = "alias";
      plan.label = bind->name.text;
      plan.source = source.label;
      plan.location = bind->location;
      steps.push_back(std::move(plan));
      return;
    }
    if (value->kind == NodeKind::kCallExpression) {
      ResolveCall(*syntax::As<syntax::CallExpression>(value), scope,
                  bind->name.text, steps);
      return;
    }
    if (value->kind == NodeKind::kNodeExpression) {
      ResolveNode(syntax::As<syntax::NodeExpression>(value), scope,
                  bind->name.text, bind->name.location);
      return;
    }
    if (value->kind != NodeKind::kWait && value->kind != NodeKind::kClose &&
        value->kind != NodeKind::kBlock && value->kind != NodeKind::kForEach &&
        value->kind != NodeKind::kRepeat && value->kind != NodeKind::kPipe) {
      if (const auto* typed = syntax::As<syntax::TypedValue>(value);
          typed != nullptr) {
        const auto info = DescribeExpression(typed->value.get(), scope);
        if (info.category == ExpressionCategory::kStream) {
          Report("flow.form.field-type-mismatch",
                 "A value initializer requires explicit 'take', '| consume', "
                 "or '| collect' for a stream.",
                 bind->location, Severity::kError, Family::kForm);
        }
      }
      Ref source = ResolveSource(value, scope);
      const bool scalar =
          source.kind == Ref::Kind::kValue || source.kind == Ref::Kind::kStatus;
      Symbol symbol;
      symbol.name = bind->name.text;
      symbol.location = bind->name.location;
      symbol.kind = scalar ? SymbolKind::kValue : SymbolKind::kInputPort;
      symbol.ref = source.node;
      symbol.writable = false;
      symbol.data_dependency = HasDataDependency(value, scope);
      symbol.shape = bind->type.has_value() ? bind->type->name : source.shape;
      symbol.value_type = assigned_type;
      symbol.pattern = PatternOfExpression(value);
      if (!symbol.value_type.has_value() && previous != kNoSymbol) {
        symbol.value_type = resolved_.symbols[previous].value_type;
      }
      if (!symbol.value_type.has_value() && scalar) {
        symbol.value_type = InferredType(value, scope);
        if (symbol.value_type.has_value()) {
          symbol.shape = symbol.value_type->ToString();
        }
      }
      if (symbol.value_type.has_value() && !bind->type.has_value() &&
          builder_ != nullptr) {
        graph::Stage conversion;
        conversion.name = "coerce";
        conversion.type = symbol.value_type;
        symbol.ref = Derive(source.node, std::move(conversion), symbol.name);
        source.node = symbol.ref;
      }
      if (builder_ != nullptr && scalar) {
        const auto captured =
            InitializeValue(source.node, bind->location, symbol.name);
        symbol.ref = captured.first;
        symbol.step = captured.second;
      }
      Define(scope, std::move(symbol));
      StepPlan step;
      step.kind = scalar ? "initialize" : "alias";
      step.label = bind->name.text;
      step.source = source.label;
      step.location = bind->location;
      steps.push_back(std::move(step));
      return;
    }
    // A bound `wait`/`close`: the barrier is the step, and the name reads as
    // the outcome it waited for.
    const size_t before = steps.size();
    const size_t graph_before =
        builder_ == nullptr ? 0 : builder_->flow().steps.size();
    ++binding_;
    ResolveStatement(value, scope, steps);
    --binding_;
    Symbol barrier;
    barrier.kind = SymbolKind::kBarrier;
    barrier.name = bind->name.text;
    barrier.location = bind->name.location;
    barrier.readable = true;
    if (builder_ != nullptr && builder_->flow().steps.size() > graph_before) {
      graph::StepId own = graph_before;
      for (size_t id = graph_before; id < builder_->flow().steps.size(); ++id) {
        const auto& candidate = builder_->step(id);
        if (candidate.body == body_ &&
            candidate.location.start == value->location.start) {
          own = id;
          break;
        }
      }
      barrier.step = own;
      // A race name reads its winner. Other barrier names read the outcome they
      // waited for; a race subject's outcome remains available through status.
      const graph::RefId winner = builder_->step(own).winner;
      barrier.ref =
          winner != graph::kNone ? winner : builder_->step(own).outcome;
      if (barrier.ref == graph::kNone &&
          graph::RecordsOutcome(builder_->step(own).kind)) {
        // A loop makes its outcome ref only when something names it, unlike a
        // block, which makes one up front.
        barrier.ref = StatusOfStep(own);
      }
      barrier.tolerant = builder_->step(own).tolerant;
      builder_->step(own).label = bind->name.text;
    }
    Define(scope, barrier);
    if (steps.size() > before) {
      steps[before].label = bind->name.text;
    }
  }

  // `[try] { ... A block is *not* a guarding scope for the `fail`/`cancel`
  // rule: its statements run at once like any body's, so a `fail` at the top of
  // one races them exactly as it would in the flow.
  /// `[try] { ... }` -- a body run as one step, with an outcome of its own.
  ///
  /// The step carries a status the way a call does, which is what lets the
  /// ordinary bound-statement path hand it to a name: `s = try { .. }` needs
  /// nothing here beyond the outcome existing. A block is *not* a guarding
  /// scope for the `fail`/`cancel` rule: its statements run at once like any
  /// body's, so a `fail` at the top of one races them exactly as it would in
  /// the flow.
  void ResolveBlock(const syntax::Block* block, Scope& scope,
                    std::vector<StepPlan>& steps) {
    StepPlan step;
    step.kind = "block";
    step.label = Label(block->after.empty() ? "block" : "after");
    step.location = block->location;
    graph::StepId made = graph::kNone;
    graph::BodyId inner_body = graph::kNone;
    if (builder_ != nullptr) {
      made = NewStep(graph::StepKind::kBlock, step.label, block->location);
      inner_body =
          builder_->AddBody(absl::StrCat(step.label, ".body"), body_, made);
      graph::Step& one = builder_->step(made);
      one.bodies.push_back(inner_body);
      one.tolerant = block->tolerant;
      // Its own outcome, so a name bound to it reads how the block went. Made
      // here rather than on demand because the bound-statement path looks for
      // it on the step it finds.
      graph::Ref outcome;
      outcome.kind = graph::RefKind::kStatus;
      outcome.label = absl::StrCat("status ", step.label);
      outcome.owner = body_;
      outcome.subject_step = made;
      one.outcome = builder_->AddRef(std::move(outcome));
    }
    step.after = ResolveAfter(block->after, scope, made);
    Scope inner;
    inner.parent = &scope;
    CaptureVariables(scope, inner, inner_body);
    step.bodies.push_back(ResolveStatements(block->body, inner, inner_body,
                                            !block->after.empty()));
    JoinVariables(scope, {{&inner, inner_body}}, made);
    steps.push_back(std::move(step));
  }

  void ResolveNodes(const syntax::Nodes* nodes, Scope& scope,
                    std::vector<StepPlan>& steps) {
    FlowPlan& plan = resolved_.plan;
    if (std::find(plan.node_maps.begin(), plan.node_maps.end(),
                  nodes->name.text) == plan.node_maps.end()) {
      plan.node_maps.push_back(nodes->name.text);
    }
    // A `nodes` block is a scope for traffic, not for names: its steps join the
    // body around it, so what it calls stays nameable after it.
    Symbol map;
    map.kind = SymbolKind::kNodeMap;
    map.name = nodes->name.text;
    map.location = nodes->name.location;
    map.readable = false;
    Define(scope, map);
    if (nodes->body.empty()) {
      return;
    }
    const std::string outer = node_map_;
    node_map_ = nodes->name.text;
    for (const syntax::NodePtr& statement : nodes->body) {
      ResolveStatement(statement.get(), scope, steps);
    }
    node_map_ = outer;
  }

  void ResolveNode(const syntax::NodeExpression* node, Scope& scope,
                   std::string_view name, const Location& location) {
    const std::string map =
        node->node_map.Empty() ? node_map_ : node->node_map.text;
    if (!map.empty()) {
      Symbol* found = Find(scope, map);
      if (found == nullptr || found->kind != SymbolKind::kNodeMap) {
        Report("flow.name.unknown-node-map",
               absl::StrCat("Unknown node map ", Quoted(map),
                            "; declare it with 'nodes ", map, "'."),
               node->location);
      } else {
        ++found->reads;
        FlowPlan& plan = resolved_.plan;
        if (std::find(plan.node_maps.begin(), plan.node_maps.end(), map) ==
            plan.node_maps.end()) {
          plan.node_maps.push_back(map);
        }
      }
    }
    const graph::ExprId id_expr =
        node->id == nullptr ? graph::kNone
                            : ResolveExpression(node->id.get(), scope, false);
    Symbol symbol;
    symbol.kind = SymbolKind::kNode;
    symbol.data_dependency = HasDataDependency(node->id.get(), scope);
    symbol.name = std::string(name);
    symbol.location = location;
    symbol.readable = true;
    symbol.writable = true;
    if (builder_ != nullptr) {
      graph::Ref ref;
      ref.kind = graph::RefKind::kNode;
      ref.label = std::string(name);
      ref.owner = body_;
      ref.name = std::string(name);
      ref.writable = true;
      ref.id_expr = id_expr;
      ref.node_map = map;
      symbol.ref = builder_->AddRef(std::move(ref));
    }
    Define(scope, symbol);
  }

  void ResolveDeadline(const Node* expression, Scope& scope,
                       graph::StepId target, StepPlan& plan,
                       bool timeout = false) {
    if (expression == nullptr) {
      return;
    }
    plan.has_deadline |= !timeout;
    const auto expr = ResolveExpression(expression, scope, false);
    if (builder_ == nullptr) {
      return;
    }
    graph::Ref value;
    value.kind = graph::RefKind::kExpr;
    value.owner = body_;
    value.expr = expr;
    value.label = "deadline";
    const auto source = builder_->AddRef(std::move(value));
    const auto capture =
        NewStep(graph::StepKind::kCapture, "deadline", expression->location);
    auto& initialized = builder_->step(capture);
    initialized.source = source;
    initialized.slot =
        absl::StrCat(timeout ? "timeout:" : "deadline:", capture);
    initialized.after = builder_->step(target).after;
    builder_->step(target).after.push_back(capture);
    if (timeout) {
      builder_->step(target).timeout_value = capture;
    } else {
      builder_->step(target).deadline = capture;
    }
  }

  /// The steps a statement waits for, from its `after` names.
  ///
  /// A name may be a step -- a call, a bound `wait`/`close` -- or a port or
  /// node, in which case the statement waits for that stream to be finished.
  /// The second reading is the one an author reaches for without thinking.
  ///
  /// With `step` it also hangs the graph's own answer on that step: a name that
  /// is a call or a barrier waits for its step, and a port or a node grows a
  /// `wait` step of its own here -- the same step a written-out `x = wait port`
  /// would have made, so the two spellings cannot drift apart.
  std::vector<std::string> ResolveAfter(const std::vector<syntax::Word>& after,
                                        Scope& scope,
                                        graph::StepId step = graph::kNone) {
    std::vector<std::string> held;
    std::vector<graph::StepId> waits;
    for (const syntax::Word& word : after) {
      if (word.expression != nullptr) {
        const Ref outcome = ResolveOutcome(word.expression.get(), scope);
        held.push_back(outcome.label);
        if (builder_ != nullptr) {
          const auto barrier =
              NewStep(graph::StepKind::kWait, outcome.label, word.location);
          builder_->step(barrier).outcome = outcome.node;
          builder_->step(barrier).tolerant = outcome.tolerant;
          waits.push_back(barrier);
        }
        continue;
      }
      Symbol* found = Find(scope, word.text);
      if (found == nullptr) {
        Report("flow.name.unknown",
               absl::StrCat("Unknown name ", Quoted(word.text),
                            " (known: ", Known(scope), ")."),
               word.location);
        continue;
      }
      switch (found->kind) {
        case SymbolKind::kCall:
        case SymbolKind::kSink:
        case SymbolKind::kBarrier:
        case SymbolKind::kValue:
        case SymbolKind::kNode:
        case SymbolKind::kInputPort:
        case SymbolKind::kOutputPort:
          break;
        default:
          Report("flow.name.no-status",
                 absl::StrCat(Quoted(word.text),
                              " has no status: that belongs to a call, a node, "
                              "a port, or a barrier."),
                 word.location);
          continue;
      }
      ++found->reads;
      ++found->status_reads;
      held.push_back(word.text);
      if (builder_ == nullptr) {
        continue;
      }
      if (found->kind == SymbolKind::kCall ||
          found->kind == SymbolKind::kBarrier ||
          found->kind == SymbolKind::kSink ||
          found->kind == SymbolKind::kValue) {
        if (found->step != graph::kNone) {
          waits.push_back(found->step);
        }
        continue;
      }
      const graph::RefId subject = found->ref;
      const std::string label = found->name;
      const graph::RefId outcome = StatusOfRef(subject, label);
      const graph::StepId barrier = NewStep(
          graph::StepKind::kWait, absl::StrCat("wait ", label), word.location);
      builder_->step(barrier).outcome = outcome;
      waits.push_back(barrier);
    }
    after_waits_ = waits;
    if (builder_ != nullptr && step != graph::kNone) {
      builder_->step(step).after = std::move(waits);
    }
    return held;
  }

  size_t ExpectCall(std::string_view name, const Location& location,
                    Scope& scope) {
    const size_t index = Lookup(scope, name);
    if (index == kNoSymbol) {
      Report("flow.name.unknown",
             absl::StrCat("Unknown name ", Quoted(name),
                          " (known: ", Known(scope), ")."),
             location);
      return kNoSymbol;
    }
    if (resolved_.symbols[index].kind != SymbolKind::kCall) {
      Report("flow.name.not-a-call",
             absl::StrCat(Quoted(name), " is not a call."), location);
      return kNoSymbol;
    }
    ++resolved_.symbols[index].reads;
    return index;
  }

  /// Resolve `skip n reference` as a shared offset on the reference.
  void ResolveSkip(const syntax::Skip& skip, Scope& scope,
                   const std::vector<std::string>& after,
                   const std::vector<graph::StepId>& held,
                   std::vector<StepPlan>& steps) {
    const syntax::Pipeline& pipeline = *skip.targets.front().pipeline;
    const Ref source = ResolvePipeline(pipeline, scope);
    const long long count = *skip.count;
    if (!source.has_front) {
      Report("flow.sequence.skip-count-target",
             absl::StrCat("'skip ", count, "' takes a port or a node, and ",
                          source.label, " is not one. Use '| drop ", count,
                          "' to drop values from a pipeline instead."),
             skip.location, Severity::kError, Family::kSequence);
    } else if (builder_ != nullptr && source.node != graph::kNone) {
      auto& skipped = builder_->ref(source.node).skip;
      if (count > std::numeric_limits<long long>::max() - skipped) {
        Report("flow.form.bad-number",
               "The combined skip count exceeds a signed 64-bit value.",
               skip.location, Severity::kError, Family::kForm);
      } else {
        skipped += count;
      }
    }
    StepPlan step;
    step.kind = "skip";
    step.label = source.label;
    step.source = source.label;
    step.location = skip.location;
    if (builder_ != nullptr) {
      const graph::StepId made = NewStep(
          graph::StepKind::kSkip,
          absl::StrCat("skip ", count, " of ", source.label), skip.location);
      builder_->step(made).source = source.node;
      builder_->step(made).count = count;
      builder_->step(made).after = held;
    }
    step.after = after;
    steps.push_back(std::move(step));
  }

  /// The tail a `log` or `logf` was written with.
  ///
  /// One routine for the statement and the stage, as the parser has one: the
  /// difference between them is only whether `it` is bound, which the caller
  /// knows and this does not have to.
  ///
  /// @param allow_it Whether the arguments may say `it` -- true in a stage.
  graph::LogTail ResolveLogTail(const syntax::LogTail& tail, Scope& scope,
                                bool allow_it, const Location& location) {
    graph::LogTail made;
    made.line = location.line;
    made.format = tail.format;
    made.has_format = tail.has_format;
    if (!tail.level.Empty()) {
      if (!vocabulary::IsLogLevel(tail.level.text)) {
        Report("flow.form.unknown-log-level",
               absl::StrCat(
                   "Unknown log level ", Quoted(tail.level.text),
                   " (known: ", absl::StrJoin(vocabulary::LogLevels(), ", "),
                   ", either case)."),
               tail.level.location, Severity::kError, Family::kForm);
      } else {
        made.level = vocabulary::Canonical(tail.level.text);
      }
    }
    for (const syntax::NodePtr& argument : tail.arguments) {
      made.arguments.push_back(
          ResolveExpression(argument.get(), scope, allow_it));
    }
    return made;
  }

  /// A `fail` code: a canonical name, a number, or an expression.
  ///
  /// A named code is not an expression -- nothing in scope is called
  /// `not_found` -- so it comes back through `named` rather than as an [Expr].
  graph::ExprId ResolveFailCode(const Node* code, Scope& scope,
                                std::string* absl_nullable named = nullptr) {
    if (code == nullptr) {
      return graph::kNone;
    }
    if (const auto* name = syntax::As<syntax::Name>(code); name != nullptr) {
      if (vocabulary::IsStatusCode(name->name)) {
        if (named != nullptr) {
          *named = name->name;
        }
        return graph::kNone;
      }
      if (Lookup(scope, name->name) == kNoSymbol) {
        // The names `fail` accepts, in the case they are canonically spelled in
        // and the order `a11.flow.plan.FAIL_CODES` lists them.
        std::vector<std::string> known;
        for (const std::string_view entry : vocabulary::StatusCodes()) {
          std::string shouted(entry);
          for (char& letter : shouted) {
            if (letter >= 'a' && letter <= 'z') {
              letter = static_cast<char>(letter - 32);
            }
          }
          known.push_back(std::move(shouted));
        }
        std::sort(known.begin(), known.end());
        Report("flow.form.unknown-status-code",
               absl::StrCat("Unknown status code ", Quoted(name->name),
                            " (known: ", absl::StrJoin(known, ", "),
                            ", either case, or a number)."),
               code->location, Severity::kError, Family::kForm);
        return graph::kNone;
      }
    }
    return ResolveExpression(code, scope, false);
  }

  // -- calls -----------------------------------------------------------------

  /// An argument that reads an output of the very call the statement waits for.
  ///
  /// The barrier holds the whole statement, arguments included, so this is a
  /// flow that cannot start: the feed will not read `x.out` until `x` has
  /// finished, and `x` cannot finish while nothing is reading `x.out`. Refused
  /// rather than warned about, because there is no arrangement of it that runs
  /// -- an author who means "everything it wrote, then hand it over" writes the
  /// reduction (`x.out | collect`) and needs no `after` at all.
  void CheckArgumentReadsAwaited(const syntax::Pipeline& pipeline,
                                 const std::vector<syntax::Word>& after,
                                 Scope& scope) {
    if (after.empty()) {
      return;
    }
    for (const syntax::Word& awaited : after) {
      const Symbol* found = Find(scope, awaited.text);
      if (found == nullptr || found->kind != SymbolKind::kCall) {
        continue;
      }
      const syntax::Node* read = FindPortRead(pipeline, awaited.text);
      if (read == nullptr) {
        continue;
      }
      Report("flow.barrier.after-reads-subject",
             absl::StrCat("This argument reads ", Quoted(Unparse(read)),
                          " and the statement waits for ", Quoted(awaited.text),
                          ", so neither can go first: the argument is not read "
                          "until ",
                          awaited.text, " has finished, and ", awaited.text,
                          " cannot finish while nothing reads its output. Drop "
                          "the 'after', or reduce the argument with "
                          "'| collect'."),
             read->location, Severity::kError, Family::kBarrier);
      return;
    }
  }

  /// The first `call.port` anywhere in `pipeline`, or null: a stage reads too.
  ///
  /// First *in the text*, because [syntax::VisitSubtree] promises only that a
  /// parent is seen before its children and a diagnostic points somewhere
  /// stable.
  const syntax::Node* absl_nullable FindPortRead(
      const syntax::Pipeline& pipeline, std::string_view call) {
    const syntax::Node* found = nullptr;
    syntax::VisitSubtree(pipeline, [&](const syntax::Node& node) {
      const auto* attr = syntax::As<syntax::Attr>(&node);
      if (attr == nullptr || attr->name == "status") {
        return;
      }
      const auto* base = syntax::As<syntax::Name>(attr->base.get());
      if (base == nullptr || base->name != call) {
        return;
      }
      if (found == nullptr || node.location.start < found->location.start) {
        found = &node;
      }
    });
    return found;
  }

  size_t ResolveCall(const syntax::CallExpression& call, Scope& scope,
                     std::string_view label, std::vector<StepPlan>& steps,
                     bool anonymous = false) {
    const std::string map = call.modifiers->node_map.Empty()
                                ? node_map_
                                : call.modifiers->node_map.text;
    if (!map.empty()) {
      Symbol* found = Find(scope, map);
      if (found == nullptr || found->kind != SymbolKind::kNodeMap) {
        Report("flow.name.unknown-node-map",
               absl::StrCat("Unknown node map ", Quoted(map),
                            "; declare it with 'nodes ", map, " { ... }'."),
               call.location);
      } else {
        ++found->reads;
      }
    }

    Symbol symbol;
    symbol.kind = SymbolKind::kCall;
    symbol.implicit = anonymous;
    symbol.name = std::string(label);
    symbol.location = call.location;
    symbol.action = call.action;
    symbol.tolerant = call.tolerant;
    symbol.target = call.invalid_target ? nullptr : known_.Flow(call.action);
    if (!call.invalid_target && symbol.target == nullptr &&
        call.action.find("::") != std::string::npos) {
      Report("flow.name.unknown-call",
             absl::StrCat(
                 "Unknown qualified callable '", call.action,
                 "'. Declare or import its module and callable contract."),
             call.action_location, Severity::kError, Family::kName);
    }
    if (symbol.target != nullptr && symbol.target->ambiguous) {
      Report("flow.name.ambiguous-implementation",
             "This callable has several executable bodies; qualify a unique "
             "implementation.",
             call.location);
    }
    symbol.readable = false;
    const auto startup_cycle = [&](const Node* expression) {
      if (expression == nullptr) {
        return;
      }
      bool found = false;
      syntax::VisitSubtree(*expression, [&](const Node& node) {
        if (const auto* name = syntax::As<syntax::Name>(&node);
            name != nullptr && name->name == label && !found) {
          found = true;
          Report("flow.barrier.expression-cycle",
                 "Action initialization depends on its own result, so it "
                 "cannot start.",
                 node.location, Severity::kError, Family::kBarrier);
          diagnostics_.back().related.push_back(
              {"", lines_.Between(call.location.start, call.location.end),
               "This invocation waits for the value above before starting."});
        }
      });
    };
    for (const auto& [name, value] : call.modifiers->headers) {
      startup_cycle(value.get());
    }
    startup_cycle(call.modifiers->action_id.get());
    startup_cycle(call.modifiers->deadline.get());
    startup_cycle(call.modifiers->timeout_expression.get());
    for (const auto& dependency : call.modifiers->after) {
      if (dependency.text == label) {
        Report(
            "flow.barrier.expression-cycle",
            "An invocation cannot wait for its own completion before starting.",
            dependency.location, Severity::kError, Family::kBarrier);
      }
    }
    Define(scope, symbol);
    const size_t index = resolved_.symbols.size() - 1;

    StepPlan step;
    // The verb it was written with, not "a call": a described step says which
    // of `run` and `call` it is, the way a `wait`/`close` step says which of
    // those it was, because that is the distinction a reader is checking for.
    step.kind = call.mode;
    step.label = std::string(label);
    step.action = call.action;
    step.mode = call.mode;
    step.node_map = map;
    step.timeout = call.modifiers->timeout;
    step.tolerant = call.tolerant;
    step.tee = call.modifiers->tee;
    step.location = call.location;
    // The headers and the id first: they are read where the call is written,
    // and the step carries what they resolved to.
    std::vector<std::pair<std::string, graph::ExprId>> headers;
    for (const auto& [name, value] : call.modifiers->headers) {
      headers.emplace_back(name, ResolveExpression(value.get(), scope, false));
    }
    graph::ExprId action_id = graph::kNone;
    if (call.modifiers->action_id != nullptr) {
      action_id =
          ResolveExpression(call.modifiers->action_id.get(), scope, false);
    }
    graph::StepId made = graph::kNone;
    if (builder_ != nullptr) {
      made = NewStep(graph::StepKind::kCall, std::string(label), call.location);
      graph::Step& one = builder_->step(made);
      one.action = call.action;
      one.deferred = anonymous;
      one.mode = call.mode;
      one.node_map = map;
      one.timeout = call.modifiers->timeout;
      one.tee = call.modifiers->tee;
      one.tolerant = call.tolerant;
      one.headers = std::move(headers);
      one.forward = call.modifiers->forward;
      one.action_id = action_id;
      resolved_.symbols[index].step = made;
    }
    step.after = ResolveAfter(call.modifiers->after, scope, made);
    ResolveDeadline(call.modifiers->deadline.get(), scope, made, step);
    ResolveDeadline(call.modifiers->timeout_expression.get(), scope, made, step,
                    true);
    // The statement's barrier, kept where the arguments can reach it: `after`
    // holds the *statement*, and an argument is part of the statement, so the
    // feeds wait for the same steps the call does.
    const std::vector<std::string> after = step.after;
    const std::vector<graph::StepId> held = after_waits_;
    // The call first, then one pipe per argument feeding it -- the order
    // `a11.flow.plan` puts them in, and the order they read in.
    steps.push_back(std::move(step));
    for (const syntax::CallExpression::Argument& argument : call.args) {
      const Ref destination =
          CallPort(index, argument.port.text, syntax::PortDirection::kInput,
                   argument.port.location);
      CheckArgumentReadsAwaited(*argument.pipeline, call.modifiers->after,
                                scope);
      const Ref source = ResolvePipeline(*argument.pipeline, scope);
      StepPlan feed;
      feed.kind = "pipe";
      feed.label =
          absl::StrCat(source.label, " -> ", label, ".", argument.port.text);
      feed.source = source.label;
      feed.destination = absl::StrCat(label, ".", argument.port.text);
      feed.after = after;
      feed.location = argument.port.location;
      if (builder_ != nullptr) {
        const graph::StepId pipe =
            NewStep(graph::StepKind::kPipe, feed.label, argument.port.location);
        builder_->step(pipe).source = source.node;
        builder_->step(pipe).destination = destination.node;
        builder_->step(pipe).after = held;
        builder_->step(pipe).deferred = anonymous;
        builder_->step(pipe).activation = made;
      }
      steps.push_back(std::move(feed));
    }
    return index;
  }

  /// A port of a call, checked against the flow it names where that is known.
  Ref CallPort(size_t call, std::string_view name,
               syntax::PortDirection direction, const Location& location) {
    Ref ref;
    ref.kind = Ref::Kind::kCallPort;
    ref.symbol = call;
    ref.has_front = true;
    if (call == kNoSymbol) {
      ref.kind = Ref::Kind::kUnknown;
      return ref;
    }
    const Symbol& symbol = resolved_.symbols[call];
    ref.label = absl::StrCat(symbol.name, ".", name);
    ref.readable = direction == syntax::PortDirection::kOutput;
    ref.writable = direction == syntax::PortDirection::kInput;
    if (symbol.target != nullptr) {
      if (const auto* port = symbol.target->Port(name, direction);
          port != nullptr) {
        ref.shape = port->type;
      }
    }
    if (symbol.target != nullptr && !symbol.target->HasPort(name, direction)) {
      const std::vector<std::string> declared =
          symbol.target->PortNames(direction);
      Report(
          "flow.name.unknown-port",
          absl::StrCat(
              symbol.action, " has no ",
              direction == syntax::PortDirection::kInput ? "input" : "output",
              " port ", Quoted(name), " (declared: ",
              declared.empty() ? "none" : absl::StrJoin(declared, ", "), ")."),
          location);
      ref.kind = Ref::Kind::kUnknown;
      return ref;
    }
    if (builder_ == nullptr) {
      return ref;
    }
    // Memoised per `direction:name`: the flow may name one port of one call
    // from several places, and it is one stream however often it is written.
    const graph::StepId step = symbol.step;
    if (step == graph::kNone) {
      return ref;
    }
    const std::string key =
        absl::StrCat(syntax::PortDirectionName(direction), ":", name);
    if (const auto found = builder_->step(step).ports.find(key);
        found != builder_->step(step).ports.end()) {
      ref.node = found->second;
      return ref;
    }
    graph::Ref port;
    port.kind = graph::RefKind::kCallPort;
    port.label = ref.label;
    port.owner = builder_->step(step).body;
    port.name = std::string(name);
    port.direction = direction;
    port.call = step;
    port.writable = ref.writable;
    // A sibling flow declared its ports here, so this one is knowable.
    if (symbol.target != nullptr) {
      if (const PortPlan* declared = symbol.target->Port(name, direction);
          declared != nullptr) {
        port.unary = declared->unary;
      }
    }
    ref.node = builder_->AddRef(std::move(port));
    builder_->step(step).ports.emplace(key, ref.node);
    return ref;
  }

  // -- pipelines and references ----------------------------------------------

  Ref ResolvePipelineCall(const syntax::CallExpression& call, Ref source,
                          ExpressionInfo& upstream, Scope& scope,
                          const syntax::Pipeline& pipeline) {
    const auto* target = known_.Flow(call.action);
    if (target == nullptr || call.invalid_target) {
      Report("flow.form.pipeline-call",
             "A pipeline invocation needs a known callable contract. Declare "
             "or import its interface.",
             call.action_location);
      return source;
    }
    std::vector<std::string> supplied;
    for (const auto& argument : call.args) {
      supplied.push_back(argument.port.text);
    }
    const auto selected = SelectPipelinePorts(*target, supplied);
    if (!selected.ok()) {
      Report("flow.form.pipeline-call",
             std::string(selected.status().message()), call.action_location);
      for (const auto& port : target->ports) {
        if (port.location.end > port.location.start) {
          diagnostics_.back().related.push_back(
              {"", lines_.Between(port.location.start, port.location.end),
               absl::StrCat(syntax::PortDirectionName(port.direction),
                            " port '", port.name, "'.")});
        }
      }
      return source;
    }
    const auto& input = *selected->input;
    const auto& output = *selected->output;
    const auto expected = input.declared.empty() ? input.type : input.declared;
    if (!upstream.type.empty() &&
        RefinesType(upstream.type, expected, known_) == Refinement::kNo) {
      Report(
          "flow.form.pipeline-call",
          absl::StrCat("Pipeline input '", input.name, "' expects ", expected,
                       ", but the upstream carries ", upstream.type, "."),
          call.action_location);
    }
    CheckArgumentReadsAwaited(pipeline, call.modifiers->after, scope);
    std::vector<StepPlan> discarded;
    auto& steps = pending_steps_ == nullptr ? discarded : *pending_steps_;
    const auto first = steps.size();
    const size_t index =
        ResolveCall(call, scope, Label(call.action), steps, true);
    ++resolved_.symbols[index].reads;
    size_t call_plan = first;
    while (call_plan < steps.size() &&
           (steps[call_plan].action.empty() ||
            steps[call_plan].location.start != call.location.start)) {
      ++call_plan;
    }
    if (call_plan == steps.size()) {
      Report("flow.form.pipeline-call", "The invocation could not be lowered.",
             call.location);
      return source;
    }
    steps[call_plan].pipeline_input = input.name;
    steps[call_plan].pipeline_output = output.name;
    const auto destination = CallPort(
        index, input.name, syntax::PortDirection::kInput, call.location);
    const auto result = CallPort(index, output.name,
                                 syntax::PortDirection::kOutput, call.location);
    const auto action = resolved_.symbols[index].step;
    StepPlan feed;
    feed.kind = "pipe";
    feed.source = source.label;
    feed.destination = destination.label;
    feed.label = absl::StrCat(source.label, " -> ", destination.label);
    feed.after = steps[call_plan].after;
    feed.location = call.location;
    if (builder_ != nullptr) {
      if (input.unary) {
        graph::Stage check;
        check.name = "consume";
        check.optional = !input.required;
        source.node = Derive(source.node, std::move(check), source.label);
      }
      auto type = ParseTypeExpression(expected);
      if (type.ok()) {
        graph::Stage check;
        check.name = "coerce";
        check.type = input.syntax_type.has_value() ? *input.syntax_type
                                                   : *std::move(type);
        source.node = Derive(source.node, std::move(check), source.label);
      }
      const auto held = builder_->step(action).after;
      const auto pipe =
          NewStep(graph::StepKind::kPipe, feed.label, call.location);
      builder_->step(pipe).source = source.node;
      builder_->step(pipe).destination = destination.node;
      builder_->step(pipe).deferred = true;
      builder_->step(pipe).activation = action;
      builder_->step(pipe).after = held;
    }
    steps.push_back(std::move(feed));
    upstream = {};
    upstream.category = ExpressionCategory::kStream;
    upstream.type = output.declared.empty() ? output.type : output.declared;
    upstream.unary = output.unary;
    upstream.cardinality_known = true;
    upstream.action = call.action;
    upstream.port = output.name;
    upstream.shape = known_.Dto(upstream.type);
    return result;
  }

  Ref ResolvePipeline(const syntax::Pipeline& pipeline, Scope& scope) {
    Ref ref = ResolveSource(pipeline.source.get(), scope);
    ExpressionInfo carried = DescribeExpression(pipeline.source.get(), scope);
    // The pattern the values carry as the pipeline is walked, so `it` in a
    // stage knows what the stage before made of them.
    std::string previous_pattern = PatternOf(pipeline);
    if (!pipeline.stages.empty()) {
      previous_pattern.clear();
    }
    for (const syntax::StagePtr& stage : pipeline.stages) {
      if (const auto* call =
              syntax::As<syntax::CallExpression>(stage->argument.get());
          call != nullptr && (stage->name == "run" || stage->name == "call")) {
        ref = ResolvePipelineCall(*call, ref, carried, scope, pipeline);
        resolved_.expressions.insert_or_assign(stage.get(), carried);
        previous_pattern.clear();
        continue;
      }
      graph::RefId stream = graph::kNone;
      graph::ExprId expr = graph::kNone;
      graph::RefId carry = graph::kNone;
      graph::LogTail log;
      if (stage->takes == vocabulary::StageArgument::kLog ||
          stage->takes == vocabulary::StageArgument::kLogFormat) {
        const std::string outer = it_pattern_;
        it_pattern_ = previous_pattern;
        log = ResolveLogTail(stage->log, scope, /*allow_it=*/true,
                             stage->location);
        it_pattern_ = outer;
      } else if (stage->takes == vocabulary::StageArgument::kStream) {
        stream = ResolveSource(stage->argument.get(), scope).node;
      } else if (stage->takes == vocabulary::StageArgument::kFold) {
        // The fold's expression sees two things: `it`, the value in hand, and
        // the name the author gave what the last value produced.
        carry = BoundRef(body_, graph::kNone, "fold");
        Scope folding;
        folding.parent = &scope;
        if (!stage->carried.text.empty()) {
          Symbol accumulator;
          accumulator.kind = SymbolKind::kLoopVariable;
          accumulator.name = stage->carried.text;
          accumulator.location = stage->carried.location;
          accumulator.ref = carry;
          Define(folding, accumulator);
        }
        const std::string outer = it_pattern_;
        it_pattern_ = previous_pattern;
        expr = ResolveExpression(stage->argument.get(), folding, true);
        it_pattern_ = outer;
      } else if (stage->argument != nullptr) {
        // `it` is the value this stage is looking at, which is whatever the
        // stage before it made -- so a `map` after a `match` knows the
        // pattern's fields.
        const std::string outer = it_pattern_;
        it_pattern_ = previous_pattern;
        expr = ResolveExpression(stage->argument.get(), scope, true);
        it_pattern_ = outer;
      }
      if ((stage->parallel > 1 || stage->parallel_expression != nullptr) &&
          stage->argument != nullptr) {
        syntax::VisitSubtree(*stage->argument, [&](const Node& node) {
          const auto* take = syntax::As<syntax::PipelineValue>(&node);
          if (take != nullptr && take->take) {
            Report(
                "flow.sequence.conflicting-access",
                "Parallel stage workers cannot take from a shared stream. "
                "Extract values before the stage or use a sequential for loop.",
                take->location);
          }
        });
      }
      previous_pattern =
          vocabulary::Canonical(stage->name) == "match" ? stage->text : "";
      CheckShapeStage(*stage, ref);
      CheckPatternStage(*stage);
      // `into failures` names a destination, so it resolves the way a pipe's
      // target does: as something this flow may write.
      graph::RefId failures = graph::kNone;
      if (stage->failures != nullptr) {
        const Ref target = ResolveDestination(stage->failures.get(), scope);
        failures = target.node;
      }
      const graph::RefId source = ref.node;
      absl::StrAppend(&ref.label, " | ", StageLabel(*stage));
      ref.kind = Ref::Kind::kDerived;
      ref.has_front = false;
      ref.writable = false;
      ref.tolerant = false;
      ref.shape = ShapeAfter(*stage, ref.shape);
      graph::Stage made =
          GraphStage(*stage, stream, expr, std::move(log), failures);
      made.carry = carry;
      made.parallel_expression =
          ResolveExpression(stage->parallel_expression.get(), scope, false);
      ref.node = Derive(source, std::move(made), ref.label);
      if (stage->name == "map") {
        const auto mapped = DescribeExpression(stage->argument.get(), scope);
        carried.type = mapped.type;
        carried.shape = mapped.shape;
        carried.fields = mapped.fields;
      } else if (stage->name == "count") {
        carried.type = "int";
        carried.unary = true;
        carried.cardinality_known = true;
      } else if (stage->name == "collect") {
        carried.type = carried.type.empty()
                           ? "list"
                           : absl::StrCat("list[", carried.type, "]");
        carried.unary = true;
        carried.cardinality_known = true;
      } else if (stage->name == "flatten") {
        carried.unary = false;
        auto type = ParseTypeExpression(carried.type);
        carried.type = type.ok() && type->parameters.size() == 1
                           ? type->parameters.front().ToString()
                           : "";
      } else {
        carried.type = ShapeAfter(*stage, carried.type);
      }
      resolved_.expressions.insert_or_assign(stage.get(), carried);
    }
    resolved_.expressions.insert_or_assign(&pipeline, carried);
    return ref;
  }

  /// What a stage does to the shape a stream was carrying.
  ///
  /// A stage that *chooses* values keeps it -- `first 3` of a stream of shapes
  /// is still shapes. A stage that reshapes replaces it, with whatever the new
  /// shape is where the flow said so and with nothing where it did not.
  std::string ShapeAfter(const syntax::Stage& stage,
                         const std::string& carried) {
    const std::string name = vocabulary::Canonical(stage.name);
    if (name == "map") {
      // `map Shape{..}` and `map it as Shape` name the output shape. Other map
      // expressions have no statically known shape.
      const auto* typed = syntax::As<syntax::TypedValue>(stage.argument.get());
      if (typed == nullptr) {
        return "";
      }
      return known_.Dto(typed->type.name) != nullptr ? typed->type.name : "";
    }
    if (name == "take while" || name == "take until" || name == "where" ||
        name == "pace" || name == "timeout" || name == "deadline" ||
        name == "consume") {
      return carried;
    }
    if (name == "log" || name == "logf") {
      // A log changes nothing about the value, so it changes nothing about what
      // the stream is carrying either.
      return carried;
    }
    if (vocabulary::PositionalStages().contains(name) || name == "then" ||
        name == "batch" || name == "group") {
      // `batch`, `group` and `window` make lists *of* the values, so what each
      // value is has not changed; the rest choose among them.
      return name == "batch" || name == "group" || name == "window" ? ""
                                                                    : carried;
    }
    return "";
  }

  /// The pattern a `match` stage was written with, read now rather than at run
  /// time.
  ///
  /// A pattern is a literal almost every time, so a typo in one is a fact about
  /// the text and belongs in the editor with something to point at -- not in a
  /// failure the first value triggers. The pattern language says what is wrong
  /// with it; this only has to place it.
  void CheckPatternStage(const syntax::Stage& stage) {
    if (vocabulary::Canonical(stage.name) != "match") {
      return;
    }
    const pattern::Compiled compiled = pattern::Compile(stage.text);
    if (compiled.ok()) {
      return;
    }
    Report("flow.form.bad-pattern", std::string(compiled.error), stage.location,
           Severity::kError, Family::kForm);
  }

  /// `| json` on a stream of a shape that holds bytes.
  ///
  /// JSON has nothing to carry a byte string in, so this is not a value that
  /// would render oddly -- it is one that cannot be rendered at all. `packb`
  /// can, which is what the message says.
  void CheckShapeStage(const syntax::Stage& stage, const Ref& ref) {
    if (ref.shape.empty()) {
      return;
    }
    if (vocabulary::Canonical(stage.name) != "json") {
      return;
    }
    const DtoPlan* shape = known_.Dto(ref.shape);
    if (shape == nullptr || !shape->binary) {
      return;
    }
    Report("flow.form.not-json-representable",
           absl::StrCat(Quoted(shape->name),
                        " holds bytes, which JSON has nothing to carry; write "
                        "'| packb' instead."),
           stage.location, Severity::kError, Family::kForm);
  }

  /// `zip(a, b, c)`: several streams read in step, as one stream of tuples.
  ///
  /// Every argument has to be a stream, and each is resolved as one -- so each
  /// counts as a reader of whatever produces it, and the existing analysis
  /// materialises and replays them the way it would for any other reader. An
  /// argument that is a plain value is a stream of one, which is the same rule
  /// a pipeline's source follows.
  Ref ResolveZip(const syntax::Zip& zip, Scope& scope) {
    return ResolveStreams(zip.name, zip.sources, scope);
  }

  Ref ResolveStreams(std::string_view name,
                     const std::vector<syntax::NodePtr>& arguments,
                     Scope& scope) {
    Ref ref;
    ref.kind = Ref::Kind::kDerived;
    // A zip has a front -- the tuples are produced here -- but a counted `skip`
    // on it would have to take tuples off a stream nothing else holds, and
    // there is nowhere upstream to apply the count.
    ref.has_front = false;
    std::vector<std::string> labels;
    std::vector<graph::RefId> sources;
    for (const syntax::NodePtr& source : arguments) {
      const Ref one = ResolveSource(source.get(), scope);
      labels.push_back(one.label);
      sources.push_back(one.node);
    }
    ref.label = absl::StrCat(name, "(", absl::StrJoin(labels, ", "), ")");
    if (builder_ == nullptr) {
      return ref;
    }
    graph::Ref made;
    made.kind =
        name == "interleave" ? graph::RefKind::kMerge : graph::RefKind::kZip;
    made.label = ref.label;
    made.owner = body_;
    made.sources = std::move(sources);
    ref.node = builder_->AddRef(std::move(made));
    return ref;
  }

  // `wait first of a, b` read where a value is expected. Written this way, `let
  // n = wait first of a, b` and `n = wait first of a, b` are the same race as
  // the statement form.
  /// `wait first of a, b` read where a value is expected.
  ///
  /// The barrier is a step like any other -- it has to be, because it waits and
  /// because `after` may name it -- so this makes the step and hands back the
  /// value it produces. Written this way, `let n = wait first of a, b` and
  /// `n = wait first of a, b` are the same race as the statement form.
  Ref ResolveRaceValue(const syntax::Wait& wait, Scope& scope) {
    Ref ref;
    ref.label = "wait";
    if (!wait.race) {
      Report("flow.form.wait-all-has-no-value",
             "'wait all of' waits for every one of them, so there is no single "
             "winner to name. 'wait first of a, b' is the one that is a value.",
             wait.location);
      return Ref{Ref::Kind::kUnknown, "wait all of"};
    }
    // The statement path is the one implementation: it makes the step, its
    // outcomes and its winner, and reports whatever is wrong with the subjects.
    std::vector<StepPlan> made;
    const size_t graph_before =
        builder_ == nullptr ? 0 : builder_->flow().steps.size();
    ResolveStatement(&wait, scope, made);
    if (pending_steps_ != nullptr) {
      for (StepPlan& one : made) {
        pending_steps_->push_back(std::move(one));
      }
    }
    if (builder_ == nullptr || builder_->flow().steps.size() <= graph_before) {
      return Ref{Ref::Kind::kUnknown, "wait first of"};
    }
    graph::StepId own = graph::kNone;
    for (size_t id = graph_before; id < builder_->flow().steps.size(); ++id) {
      const auto& candidate = builder_->step(id);
      if (candidate.kind == graph::StepKind::kWait &&
          candidate.location.start == wait.location.start) {
        own = id;
      }
    }
    if (own == graph::kNone) {
      return ref;
    }
    builder_->step(own).deferred = true;
    const graph::Step& step = builder_->step(own);
    ref.kind = Ref::Kind::kValue;
    ref.label = step.label;
    ref.node = step.winner;
    ref.has_front = true;
    return ref;
  }

  /// A pipeline source: a stream, a path over one, or a single value.
  Ref ResolveSource(const Node* expression, Scope& scope) {
    if (expression != nullptr && expression->kind == NodeKind::kPipe) {
      Report("flow.form.sink-as-source",
             "A transfer returns a continuation sink, not a source stream.",
             expression->location, Severity::kError, Family::kForm);
      return Ref{};
    }
    std::optional<Ref> stream = AsStream(expression, scope);
    if (stream.has_value()) {
      return *std::move(stream);
    }
    const graph::ExprId expr = ResolveExpression(expression, scope, false);
    Ref ref;
    ref.kind = Ref::Kind::kValue;
    ref.label = Unparse(expression);
    if (builder_ != nullptr) {
      graph::Ref made;
      made.kind = graph::RefKind::kExpr;
      made.label = ref.label;
      made.owner = body_;
      made.expr = expr;
      ref.node = builder_->AddRef(std::move(made));
    }
    return ref;
  }

  ExpressionInfo DescribeExpression(const Node* expression,
                                    const Scope& scope) {
    if (expression == nullptr) {
      return {};
    }
    if (const auto found = resolved_.expressions.find(expression);
        found != resolved_.expressions.end()) {
      return found->second;
    }
    ExpressionInfo info;
    if (const auto* literal = syntax::As<syntax::Literal>(expression);
        literal != nullptr) {
      info.category = ExpressionCategory::kValue;
      info.unary = true;
      info.cardinality_known = true;
      using K = syntax::Constant::Kind;
      switch (literal->value.kind) {
        case K::kInteger:
          info.type = "integer";
          break;
        case K::kDouble:
          info.type = "number";
          break;
        case K::kBool:
          info.type = "bool";
          break;
        case K::kString:
          info.type = "string";
          break;
        case K::kDuration:
          info.type = "duration";
          break;
        case K::kNull:
          info.type = "null";
          break;
        default:
          break;
      }
    }
    if (const auto* function = syntax::As<syntax::Builtin>(expression);
        function != nullptr) {
      info.category = ExpressionCategory::kValue;
      info.unary = true;
      info.cardinality_known = true;
      const auto& name = function->name;
      if (name == "now" || name == "time" || name == "parse_time") {
        info.type = "time";
      } else if (name == "duration" || name == "parse_duration") {
        info.type = "duration";
      } else if (name == "integer" || name == "int" || name == "len") {
        info.type = "integer";
      } else if (name == "number" || name == "seconds") {
        info.type = "number";
      } else if (name == "bool" || name == "contains" || name == "startswith" ||
                 name == "endswith") {
        info.type = "bool";
      } else if (name == "upper" || name == "lower" || name == "trim" ||
                 name == "text" || name == "string" || name == "join" ||
                 name == "replace" || name == "strformat" ||
                 name == "b64encode" || name == "b64urlencode" ||
                 name == "utf8decode") {
        info.type = "string";
      } else if (name == "bytes" || name == "b64decode" ||
                 name == "b64urldecode" || name == "utf8encode") {
        info.type = "bytes";
      } else if (name == "keys" || name == "values" || name == "split" ||
                 name == "list") {
        info.type = "list";
      } else if (name == "option") {
        info.type = "option";
        info.fields = {"has_value", "value"};
        if (!function->args.empty()) {
          if (const auto* object = syntax::As<syntax::ObjectLiteral>(
                  function->args.front().get());
              object != nullptr) {
            for (const auto& [key, payload] : object->pairs) {
              if (key == "value") {
                const auto type = DescribeExpression(payload.get(), scope).type;
                if (!type.empty()) {
                  info.type = absl::StrCat("option[", type, "]");
                }
              }
            }
          }
        }
      } else if (name == "status") {
        info.type = "status";
      } else if (name == "object" || name == "merge") {
        info.type = "object";
      } else if (name == "zip" || name == "interleave") {
        info.category = ExpressionCategory::kStream;
        info.unary = false;
        info.cardinality_known = false;
      }
      if (const auto fields = FieldsOfPattern(PatternOfExpression(expression));
          fields.has_value()) {
        info.fields = fields->names;
      }
    } else if (expression->kind == NodeKind::kListLiteral ||
               expression->kind == NodeKind::kObjectLiteral) {
      info.category = ExpressionCategory::kValue;
      info.type =
          expression->kind == NodeKind::kListLiteral ? "list" : "object";
      info.unary = true;
      info.cardinality_known = true;
    } else if (const auto* binary = syntax::As<syntax::Binary>(expression);
               binary != nullptr) {
      info.category = ExpressionCategory::kValue;
      const auto left = DescribeExpression(binary->left.get(), scope).type;
      const auto right = DescribeExpression(binary->right.get(), scope).type;
      if (binary->op == "+" || binary->op == "-") {
        if (left == "time" && right == "time" && binary->op == "-") {
          info.type = "duration";
        } else if (left == "time" &&
                   (right == "duration" || right == "number" ||
                    right == "integer")) {
          info.type = "time";
        } else if (left == right) {
          info.type = left;
        } else if ((left == "integer" || left == "number") &&
                   (right == "integer" || right == "number")) {
          info.type = "number";
        }
      } else if (binary->op == "and" || binary->op == "or") {
        std::string payload = left;
        const bool option =
            binary->op == "or" &&
            (left == "option" || left == "std::option" ||
             left.starts_with("option[") || left.starts_with("std::option["));
        if (option) {
          const size_t bracket = left.find('[');
          payload = bracket == std::string::npos
                        ? ""
                        : left.substr(bracket + 1, left.size() - bracket - 2);
        }
        if (option && (payload.empty() || payload == right)) {
          info.type = right;
        } else if (!option && left == right) {
          info.type = left;
        }
      } else {
        info.type = "bool";
      }
      info.unary = true;
      info.cardinality_known = true;
    } else if (const auto* unary = syntax::As<syntax::Unary>(expression);
               unary != nullptr) {
      info.category = ExpressionCategory::kValue;
      info.type = unary->op == "not"
                      ? "bool"
                      : DescribeExpression(unary->operand.get(), scope).type;
      info.unary = true;
      info.cardinality_known = true;
    }
    const auto call = [&](std::string action) {
      info.category = ExpressionCategory::kCall;
      info.action = std::move(action);
      info.callable = known_.Flow(info.action);
      if (info.callable != nullptr) {
        info.description = info.callable->description;
      }
    };
    const auto completion = [&] {
      info.category = ExpressionCategory::kCompletion;
      info.type = "status";
      info.cardinality_known = true;
      info.unary = true;
      for (const auto name : vocabulary::OrderedStatusFields()) {
        info.fields.emplace_back(name);
      }
    };
    if (expression->kind == NodeKind::kPipe) {
      info.category = ExpressionCategory::kSink;
      info.type = "continuation-sink";
      info.writable = true;
    } else if (const auto* operation =
                   syntax::As<syntax::CallExpression>(expression);
               operation != nullptr) {
      if (!operation->invalid_target) {
        call(operation->action);
      }
    } else if (expression->kind == NodeKind::kNodeExpression) {
      info.category = ExpressionCategory::kNode;
      info.fields = {"id"};
      info.writable = true;
    } else if (const auto* name = syntax::As<syntax::Name>(expression);
               name != nullptr) {
      const size_t index = Lookup(scope, name->name);
      if (index != kNoSymbol) {
        const auto& symbol = resolved_.symbols[index];
        info.symbol = index;
        if (symbol.kind == SymbolKind::kCall) {
          call(symbol.action);
        } else if (symbol.kind == SymbolKind::kNode) {
          info.category = ExpressionCategory::kNode;
          info.fields = {"id"};
          info.writable = true;
        } else if (symbol.kind == SymbolKind::kSink) {
          info.category = ExpressionCategory::kSink;
          info.type = "continuation-sink";
          info.writable = true;
        } else if (symbol.kind == SymbolKind::kBarrier) {
          completion();
        } else {
          info.category = symbol.kind == SymbolKind::kValue ||
                                  symbol.kind == SymbolKind::kLoopVariable ||
                                  symbol.kind == SymbolKind::kCarry ||
                                  symbol.kind == SymbolKind::kHeader
                              ? ExpressionCategory::kValue
                              : ExpressionCategory::kStream;
          info.type = symbol.value_type.has_value()
                          ? symbol.value_type->ToString()
                          : symbol.shape;
          if (symbol.value_type.has_value() &&
              symbol.value_type->name == "option") {
            info.fields = {"has_value", "value"};
          }
          if (symbol.value_type.has_value() &&
              symbol.value_type->name == "status") {
            for (const auto field : vocabulary::OrderedStatusFields()) {
              info.fields.emplace_back(field);
            }
          }
          if (const auto fields = FieldsOfPattern(symbol.pattern);
              fields.has_value()) {
            info.fields = fields->names;
          }
          info.writable = symbol.writable;
          for (const auto direction : {syntax::PortDirection::kInput,
                                       syntax::PortDirection::kOutput}) {
            if (const auto* port = resolved_.plan.Port(name->name, direction);
                port != nullptr) {
              info.type = port->declared.empty() ? port->type : port->declared;
              info.shape = port->analysis_shape.get();
              info.unary = port->unary;
              info.cardinality_known = true;
              info.description = port->description;
            }
          }
        }
      }
    } else if (const auto* attr = syntax::As<syntax::Attr>(expression);
               attr != nullptr) {
      const auto base = DescribeExpression(attr->base.get(), scope);
      if (base.category == ExpressionCategory::kCall) {
        if (attr->name == "status") {
          completion();
        } else {
          info.category = ExpressionCategory::kStream;
          info.action = base.action;
          info.port = attr->name;
          if (base.callable != nullptr) {
            for (const auto direction : {syntax::PortDirection::kOutput,
                                         syntax::PortDirection::kInput}) {
              if (const auto* port = base.callable->Port(attr->name, direction);
                  port != nullptr) {
                info.type =
                    port->declared.empty() ? port->type : port->declared;
                info.shape = port->analysis_shape.get();
                info.unary = port->unary;
                info.cardinality_known = true;
                info.description = port->description;
                info.writable = direction == syntax::PortDirection::kInput;
                break;
              }
            }
          }
        }
      } else if (base.category == ExpressionCategory::kNode &&
                 attr->name == "id") {
        info.category = ExpressionCategory::kValue;
        info.type = "string";
        info.unary = true;
        info.cardinality_known = true;
      } else if (base.type == "option" || base.type == "std::option" ||
                 base.type.starts_with("option[") ||
                 base.type.starts_with("std::option[")) {
        info.category = base.category;
        info.unary = base.unary;
        info.cardinality_known = base.cardinality_known;
        if (attr->name == "has_value") {
          info.type = "bool";
        } else if (attr->name == "value") {
          const auto option = InferredType(attr->base.get(), scope);
          if (option.has_value() && option->parameters.size() == 1) {
            info.type = option->parameters.front().ToString();
          }
        }
      } else if (base.shape != nullptr) {
        if (const auto* field = base.shape->Field(attr->name);
            field != nullptr) {
          info.category = base.category;
          info.type = field->declared.empty() ? field->type : field->declared;
          info.unary = base.unary;
          info.cardinality_known = base.cardinality_known;
          info.description = field->description;
        }
      } else if (base.category == ExpressionCategory::kSink &&
                 attr->name == "status") {
        completion();
      } else if (base.category == ExpressionCategory::kCompletion ||
                 base.type == "status") {
        info.category = ExpressionCategory::kValue;
        info.type = attr->name == "ok"        ? "bool"
                    : attr->name == "code"    ? "string"
                    : attr->name == "details" ? "list"
                    : attr->name == "number"  ? "integer"
                                              : "string";
        info.unary = true;
        info.cardinality_known = true;
      }
    } else if (const auto* typed = syntax::As<syntax::TypedValue>(expression);
               typed != nullptr) {
      info.category = ExpressionCategory::kValue;
      info.type = typed->type.ToString();
    } else if (expression->kind == NodeKind::kWait ||
               expression->kind == NodeKind::kClose ||
               expression->kind == NodeKind::kOutcome) {
      completion();
      if (const auto* wait = syntax::As<syntax::Wait>(expression);
          wait != nullptr && wait->race) {
        info.category = ExpressionCategory::kValue;
        info.type = "integer";
        info.fields.clear();
      }
    } else if (const auto* value =
                   syntax::As<syntax::PipelineValue>(expression);
               value != nullptr) {
      info = DescribeExpression(value->pipeline->source.get(), scope);
      info.category = ExpressionCategory::kValue;
      for (const auto& stage : value->pipeline->stages) {
        if (const auto found = resolved_.expressions.find(stage.get());
            found != resolved_.expressions.end()) {
          info = found->second;
          continue;
        }
        const auto before = info.type;
        if (stage->name != "run" && stage->name != "call") {
          info.type = ShapeAfter(*stage, info.type);
        }
        if (info.type != before) {
          info.shape = nullptr;
        }
        if (vocabulary::ReducingStages().contains(stage->name)) {
          info.cardinality_known = true;
          info.unary = true;
        }
      }
      info.category = ExpressionCategory::kValue;
      if (value->optional) {
        info.type = info.type.empty() ? "option"
                                      : absl::StrCat("option[", info.type, "]");
        info.shape = nullptr;
        info.fields = {"has_value", "value"};
        info.unary = true;
        info.cardinality_known = true;
      }
    } else if (const auto* index = syntax::As<syntax::Index>(expression);
               index != nullptr) {
      info = DescribeExpression(index->base.get(), scope);
      info.shape = nullptr;
      const size_t bracket = info.type.find('[');
      if (bracket != std::string::npos && info.type.back() == ']') {
        info.type =
            info.type.substr(bracket + 1, info.type.size() - bracket - 2);
      } else {
        info.type.clear();
      }
    }
    if (info.shape == nullptr) {
      info.shape = info.type.empty() ? nullptr : known_.Dto(info.type);
    }
    resolved_.expressions.insert_or_assign(expression, info);
    return info;
  }

  /// A name or a path rooted at one, as a stream; `nullopt` if it is a value.
  std::optional<Ref> AsStream(const Node* expression, Scope& scope) {
    if (!budget_.Check()) {
      return std::nullopt;
    }
    DescribeExpression(expression, scope);
    if (expression == nullptr) {
      return std::nullopt;
    }
    switch (expression->kind) {
      case NodeKind::kBuiltin: {
        const auto* function = syntax::As<syntax::Builtin>(expression);
        if (function->name == "zip" || function->name == "interleave") {
          return ResolveStreams(function->name, function->args, scope);
        }
        return std::nullopt;
      }
      case NodeKind::kClose:
        return CompletionValue(expression, scope);
      case NodeKind::kNodeExpression: {
        const size_t index = ResolveOperation(expression, scope);
        Ref ref;
        ref.kind = Ref::Kind::kNode;
        ref.symbol = index;
        ref.node = resolved_.symbols[index].ref;
        ref.label = "node()";
        ref.writable = true;
        ref.has_front = true;
        return ref;
      }
      case NodeKind::kPipelineValue: {
        const auto* value = syntax::As<syntax::PipelineValue>(expression);
        if (value->take) {
          return std::nullopt;
        }
        return ResolvePipeline(*value->pipeline, scope);
      }
      case NodeKind::kZip:
        return ResolveZip(*syntax::As<syntax::Zip>(expression), scope);
      case NodeKind::kWait:
        if (syntax::As<syntax::Wait>(expression)->race ||
            !syntax::As<syntax::Wait>(expression)->subjects.empty()) {
          return ResolveRaceValue(*syntax::As<syntax::Wait>(expression), scope);
        }
        return CompletionValue(expression, scope);
      case NodeKind::kOutcome:
        return ResolveOutcome(
            syntax::As<syntax::Outcome>(expression)->subject.get(), scope);
      case NodeKind::kName: {
        const auto* name = syntax::As<syntax::Name>(expression);
        Symbol* found = Find(scope, name->name);
        if (found == nullptr) {
          Report("flow.name.unknown",
                 absl::StrCat("Unknown name ", Quoted(name->name),
                              " (known: ", Known(scope), ")."),
                 expression->location);
          return Ref{Ref::Kind::kUnknown, name->name};
        }
        if (found->kind == SymbolKind::kCall) {
          Report("flow.name.call-as-stream",
                 absl::StrCat(Quoted(name->name),
                              " is a call; name one of its ports, like ",
                              name->name, ".output."),
                 expression->location);
          return Ref{Ref::Kind::kUnknown, name->name};
        }
        if (found->kind == SymbolKind::kNodeMap) {
          Report(
              "flow.name.not-a-stream",
              absl::StrCat(Quoted(name->name), " is a node map, not a stream."),
              expression->location);
          return Ref{Ref::Kind::kUnknown, name->name};
        }
        if (found->kind == SymbolKind::kSink) {
          Report("flow.form.sink-as-source",
                 "A continuation sink cannot be read or piped as a source. "
                 "Use it on the right of '->', or read 'status handle'.",
                 expression->location, Severity::kError, Family::kForm);
          return Ref{Ref::Kind::kUnknown, name->name};
        }
        if (!found->readable) {
          Report("flow.name.not-a-stream",
                 absl::StrCat(Quoted(name->name),
                              " is written by this flow, not read."),
                 expression->location);
          return Ref{Ref::Kind::kUnknown, name->name};
        }
        ++found->reads;
        if (found->kind == SymbolKind::kBarrier) {
          ++found->status_reads;
        }
        Ref ref;
        ref.label = name->name;
        ref.symbol = Lookup(scope, name->name);
        ref.node = found->ref;
        switch (found->kind) {
          case SymbolKind::kInputPort:
          case SymbolKind::kOutputPort: {
            ref.kind = Ref::Kind::kPort;
            ref.has_front = true;
            ref.writable = found->writable;
            const syntax::PortDirection side =
                found->kind == SymbolKind::kInputPort
                    ? syntax::PortDirection::kInput
                    : syntax::PortDirection::kOutput;
            if (const PortPlan* port = resolved_.plan.Port(name->name, side);
                port != nullptr && known_.Dto(port->type) != nullptr) {
              ref.shape = port->type;
            }
            break;
          }
          case SymbolKind::kNode:
            ref.kind = Ref::Kind::kNode;
            ref.has_front = true;
            ref.writable = true;
            break;
          case SymbolKind::kValue:
            // One value, ready-made. It stands where an expression does and it
            // is a stream of one where a pipeline's source does -- which is
            // what lets `image | chunk 65536` be written of a `value`.
            ref.kind = Ref::Kind::kValue;
            break;
          case SymbolKind::kSink:
          case SymbolKind::kBarrier:
            // A named barrier reads as the status it waited for, which is the
            // barrier's own outcome rather than a second one of it.
            ref.kind = Ref::Kind::kStatus;
            ref.tolerant = found->tolerant;
            break;
          default:
            ref.kind = Ref::Kind::kValue;
            break;
        }
        return ref;
      }
      case NodeKind::kAttr: {
        const auto* attr = syntax::As<syntax::Attr>(expression);
        std::optional<Ref> named = AttrStream(attr, scope);
        if (named.has_value()) {
          return named;
        }
        std::optional<Ref> inner = AsStream(attr->base.get(), scope);
        if (!inner.has_value() || inner->kind == Ref::Kind::kValue ||
            inner->kind == Ref::Kind::kStatus) {
          return std::nullopt;
        }
        Ref ref = *std::move(inner);
        const graph::RefId source = ref.node;
        absl::StrAppend(&ref.label, ".", attr->name);
        ref.kind = Ref::Kind::kDerived;
        ref.has_front = false;
        ref.writable = false;
        ref.tolerant = false;
        ref.node = Derive(source, AtStage(attr->name), ref.label);
        return ref;
      }
      case NodeKind::kIndex: {
        const auto* index = syntax::As<syntax::Index>(expression);
        std::optional<Ref> inner = AsStream(index->base.get(), scope);
        if (!inner.has_value() || inner->kind == Ref::Kind::kValue ||
            inner->kind == Ref::Kind::kStatus) {
          return std::nullopt;
        }
        Constant(index->index.get());
        Ref ref = *std::move(inner);
        const graph::RefId source = ref.node;
        absl::StrAppend(&ref.label, "[", Unparse(index->index.get()), "]");
        ref.kind = Ref::Kind::kDerived;
        ref.has_front = false;
        ref.writable = false;
        ref.tolerant = false;
        ref.node = Derive(source,
                          AtStage(syntax::ConstantValue(index->index.get())
                                      .value_or(syntax::Constant::Null())),
                          ref.label);
        return ref;
      }
      default:
        return std::nullopt;
    }
  }

  /// `x.y` where `x` is a call, a node or a barrier -- not a field of a value.
  std::optional<Ref> AttrStream(const syntax::Attr* attr, Scope& scope) {
    const auto* base = syntax::As<syntax::Name>(attr->base.get());
    const size_t index = base != nullptr ? Lookup(scope, base->name)
                         : IsOperation(attr->base.get())
                             ? ResolveOperation(attr->base.get(), scope)
                             : kNoSymbol;
    if (index == kNoSymbol) {
      return std::nullopt;
    }
    Symbol& symbol = resolved_.symbols[index];
    if (symbol.kind == SymbolKind::kCall) {
      ++symbol.reads;
      if (attr->name == "status") {
        ++symbol.status_reads;
        Ref ref;
        ref.kind = Ref::Kind::kStatus;
        ref.label = absl::StrCat("status ", symbol.name);
        ref.symbol = index;
        ref.tolerant = symbol.tolerant;
        ref.node = StatusOfStep(symbol.step);
        return ref;
      }
      // An output unless the flow it names declares it as an input, which is
      // what lets `x.in-port` be written and `x.out-port` be read.
      syntax::PortDirection direction = syntax::PortDirection::kOutput;
      if (symbol.target != nullptr &&
          !symbol.target->HasPort(attr->name, syntax::PortDirection::kOutput) &&
          symbol.target->HasPort(attr->name, syntax::PortDirection::kInput)) {
        direction = syntax::PortDirection::kInput;
      }
      return CallPort(index, attr->name, direction, attr->location);
    }
    if (symbol.kind == SymbolKind::kNode && attr->name == "id") {
      ++symbol.reads;
      Ref ref;
      ref.kind = Ref::Kind::kNodeId;
      ref.label = absl::StrCat(symbol.name, ".id");
      ref.symbol = index;
      // Fresh per mention: an id is one value handed over, not a stream several
      // readers share.
      if (builder_ != nullptr && symbol.ref != graph::kNone) {
        graph::Ref made;
        made.kind = graph::RefKind::kNodeId;
        made.label = ref.label;
        made.owner = builder_->ref(symbol.ref).owner;
        made.subject = symbol.ref;
        ref.node = builder_->AddRef(std::move(made));
      }
      return ref;
    }
    if (symbol.kind == SymbolKind::kSink && attr->name == "status") {
      ++symbol.reads;
      ++symbol.status_reads;
      Ref ref;
      ref.kind = Ref::Kind::kStatus;
      ref.label = absl::StrCat(symbol.name, ".status");
      ref.tolerant = symbol.tolerant;
      ref.node = FreshStatusOfStep(symbol.step);
      return ref;
    }
    if (symbol.kind == SymbolKind::kBarrier) {
      ++symbol.reads;
      ++symbol.status_reads;
      Ref ref;
      ref.kind = Ref::Kind::kDerived;
      ref.label = absl::StrCat(symbol.name, ".", attr->name);
      ref.symbol = index;
      ref.node = Derive(symbol.ref, AtStage(attr->name), ref.label);
      return ref;
    }
    return std::nullopt;
  }

  /// The status of what `expression` names.
  ///
  /// The longest prefix of the reference that names something with a status
  /// wins, and anything left over reads into the record: `status x.ok` is the
  /// call's status asked whether it is ok, and `status x.out` is that port's.
  Ref ResolveOutcome(const Node* expression, Scope& scope) {
    if (expression != nullptr && (expression->kind == NodeKind::kWait ||
                                  expression->kind == NodeKind::kClose)) {
      return CompletionValue(expression, scope);
    }
    if (IsOperation(expression)) {
      const size_t index = ResolveOperation(expression, scope);
      syntax::Name named;
      named.name = resolved_.symbols[index].name;
      named.location = expression->location;
      return ResolveOutcome(&named, scope);
    }
    if (const auto* attr = syntax::As<syntax::Attr>(expression);
        attr != nullptr && IsOperation(attr->base.get())) {
      const auto port = AttrStream(attr, scope);
      if (port.has_value()) {
        Ref result;
        result.kind = Ref::Kind::kStatus;
        result.label = absl::StrCat("status ", port->label);
        result.tolerant = port->tolerant;
        if (builder_ != nullptr) {
          result.node = StatusOfRef(port->node, port->label);
        }
        return result;
      }
    }
    // `wait _`, `close _`, `status _`: a discard is not a thing with an
    // outcome, because it is not a thing.
    if (IsDiscard(expression)) {
      ReportDiscardRead(expression, "waited on");
      return Ref{Ref::Kind::kUnknown, "_"};
    }
    std::vector<std::string> path;
    const Node* cursor = expression;
    // A trailing field of the record belongs to the record, not to a port of
    // that name: `status x.code` is what x finished with.
    while (true) {
      const auto* attr = syntax::As<syntax::Attr>(cursor);
      if (attr == nullptr) {
        break;
      }
      if (!vocabulary::StatusFields().contains(attr->name)) {
        break;
      }
      if (!HasStatus(attr->base.get(), scope)) {
        break;
      }
      path.push_back(attr->name);
      cursor = attr->base.get();
    }
    while (!HasStatus(cursor, scope)) {
      if (const auto* attr = syntax::As<syntax::Attr>(cursor);
          attr != nullptr) {
        path.push_back(attr->name);
        cursor = attr->base.get();
        continue;
      }
      if (const auto* name = syntax::As<syntax::Name>(cursor);
          name != nullptr) {
        if (Lookup(scope, name->name) == kNoSymbol) {
          Report("flow.name.unknown",
                 absl::StrCat("Unknown name ", Quoted(name->name),
                              " (known: ", Known(scope), ")."),
                 expression->location);
        } else {
          Report("flow.name.no-status",
                 absl::StrCat(Quoted(name->name),
                              " has no status: that belongs to a call, a node, "
                              "a port, or a barrier."),
                 expression->location);
        }
        return Ref{Ref::Kind::kUnknown, Unparse(expression)};
      }
      Report("flow.name.no-status",
             "A status belongs to a call, a node, a port, or a barrier.",
             expression->location);
      return Ref{Ref::Kind::kUnknown, Unparse(expression)};
    }
    // Reading a status is reading the subject: count it, so a `try` whose
    // status nothing reads can be told from one that is waited on.
    if (const auto* name = syntax::As<syntax::Name>(cursor); name != nullptr) {
      if (Symbol* found = Find(scope, name->name); found != nullptr) {
        ++found->reads;
        ++found->status_reads;
      }
    } else if (const auto* attr = syntax::As<syntax::Attr>(cursor);
               attr != nullptr) {
      if (const auto* base = syntax::As<syntax::Name>(attr->base.get());
          base != nullptr) {
        if (Symbol* found = Find(scope, base->name); found != nullptr) {
          ++found->reads;
          ++found->status_reads;
        }
      }
    }
    Ref ref;
    ref.kind = Ref::Kind::kStatus;
    ref.label = absl::StrCat("status ", Unparse(cursor));
    GraphSubject(cursor, scope, ref);
    for (const std::string& at : std::views::reverse(path)) {
      const graph::RefId source = ref.node;
      absl::StrAppend(&ref.label, ".", at);
      // A field of the record: the status is still the thing waited for, and
      // the field is read out of the value it gives.
      ref.node = Derive(source, AtStage(at), ref.label);
    }
    return ref;
  }

  /// The graph ref that is the status of what `cursor` names.
  ///
  /// A call's and a node's are fresh per mention; a bound barrier's is the one
  /// it already waited for, because `s` and `wait ...` are the same moment.
  void GraphSubject(const Node* cursor, Scope& scope, Ref& out) {
    if (builder_ == nullptr) {
      return;
    }
    if (const auto* name = syntax::As<syntax::Name>(cursor); name != nullptr) {
      const size_t index = Lookup(scope, name->name);
      if (index == kNoSymbol) {
        return;
      }
      const Symbol& symbol = resolved_.symbols[index];
      if (symbol.kind == SymbolKind::kCall) {
        out.tolerant = symbol.tolerant;
        out.node = FreshStatusOfStep(symbol.step);
        return;
      }
      if (symbol.kind == SymbolKind::kBarrier ||
          symbol.kind == SymbolKind::kSink) {
        out.tolerant = symbol.tolerant;
        out.node = FreshStatusOfStep(symbol.step);
        return;
      }
      out.node = StatusOfRef(symbol.ref, symbol.name);
      return;
    }
    const auto* attr = syntax::As<syntax::Attr>(cursor);
    if (attr == nullptr) {
      return;
    }
    const auto* base = syntax::As<syntax::Name>(attr->base.get());
    if (base == nullptr) {
      return;
    }
    const size_t index = Lookup(scope, base->name);
    if (index == kNoSymbol) {
      return;
    }
    if (resolved_.symbols[index].kind != SymbolKind::kCall) {
      return;
    }
    if (attr->name == "status") {
      out.tolerant = resolved_.symbols[index].tolerant;
      out.node = FreshStatusOfStep(resolved_.symbols[index].step);
      return;
    }
    // The port's own outcome: an output cut off mid-stream is a thing that
    // happened, and `wait x.out` is how a flow notices.
    syntax::PortDirection direction = syntax::PortDirection::kOutput;
    const FlowPlan* target = resolved_.symbols[index].target;
    if (target != nullptr &&
        !target->HasPort(attr->name, syntax::PortDirection::kOutput) &&
        target->HasPort(attr->name, syntax::PortDirection::kInput)) {
      direction = syntax::PortDirection::kInput;
    }
    const Ref port = CallPort(index, attr->name, direction, attr->location);
    out.node = StatusOfRef(port.node, port.label);
  }

  /// Whether `expression` names something a status belongs to.
  bool HasStatus(const Node* expression, const Scope& scope) const {
    if (const auto* name = syntax::As<syntax::Name>(expression);
        name != nullptr) {
      const size_t index = Lookup(scope, name->name);
      if (index == kNoSymbol) {
        return false;
      }
      switch (resolved_.symbols[index].kind) {
        case SymbolKind::kCall:
        case SymbolKind::kSink:
        case SymbolKind::kBarrier:
        case SymbolKind::kNode:
        case SymbolKind::kInputPort:
        case SymbolKind::kOutputPort:
          return true;
        default:
          return false;
      }
    }
    if (const auto* attr = syntax::As<syntax::Attr>(expression);
        attr != nullptr) {
      const auto* base = syntax::As<syntax::Name>(attr->base.get());
      if (base == nullptr) {
        return false;
      }
      const size_t index = Lookup(scope, base->name);
      if (index == kNoSymbol) {
        return false;
      }
      const Symbol& symbol = resolved_.symbols[index];
      if (symbol.kind != SymbolKind::kCall) {
        return false;
      }
      if (attr->name == "status") {
        return true;
      }
      if (symbol.target == nullptr) {
        return true;
      }
      return symbol.target->HasPort(attr->name,
                                    syntax::PortDirection::kOutput) ||
             symbol.target->HasPort(attr->name, syntax::PortDirection::kInput);
    }
    return false;
  }

  /// Whether this is a bare `_`.
  ///
  /// Only where a pipe's target goes, and every other caller of
  /// [ParseReference] asks this so it can say what is wrong rather than let
  /// [ResolveDestination] or [ResolveSource] guess at it.
  [[nodiscard]] static bool IsDiscard(const Node* absl_nullable expression) {
    return expression != nullptr && expression->kind == NodeKind::kDiscard;
  }

  /// Refuse a `_` written anywhere it keeps nothing to give.
  ///
  /// @param doing How the statement would have used it: "read", "waited on".
  void ReportDiscardRead(const Node* expression, std::string_view doing) {
    Report("flow.name.discard-read",
           absl::StrCat("'_' throws a stream away, so there is nothing here to "
                        "be ",
                        doing,
                        ". It may only stand where a pipe's destination does, "
                        "as in 'source | stage -> _'."),
           expression->location);
  }

  /// True, having reported, where @p expression is a `_` that cannot stand
  /// here. The shape every reference-reading statement uses to guard itself.
  bool RefusedAsDiscard(const Node* absl_nullable expression,
                        std::string_view doing) {
    if (!IsDiscard(expression)) {
      return false;
    }
    ReportDiscardRead(expression, doing);
    return true;
  }

  Ref ResolveDestination(const Node* expression, Scope& scope) {
    DescribeExpression(expression, scope);
    if (const auto* pipe = syntax::As<syntax::Pipe>(expression);
        pipe != nullptr) {
      const auto step = ResolveTransfer(*pipe, scope, *pending_steps_);
      Ref ref;
      ref.label = builder_->step(step).label;
      ref.writable = true;
      ref.node = TransferSink(step);
      return ref;
    }

    if (IsOperation(expression) &&
        expression->kind == NodeKind::kNodeExpression) {
      return *AsStream(expression, scope);
    }
    if (const auto* attr = syntax::As<syntax::Attr>(expression);
        attr != nullptr && IsOperation(attr->base.get())) {
      const size_t index = ResolveOperation(attr->base.get(), scope);
      if (resolved_.symbols[index].kind == SymbolKind::kCall) {
        return CallPort(index, attr->name, syntax::PortDirection::kInput,
                        attr->location);
      }
    }
    if (IsDiscard(expression)) {
      // Reached from the destinations that are not a pipe's target: a `close`,
      // an `abort`, a `try ... into`. A pipe's own target never gets here.
      ReportDiscardRead(expression, "written");
      return Ref{Ref::Kind::kUnknown, "_"};
    }
    if (const auto* name = syntax::As<syntax::Name>(expression);
        name != nullptr) {
      Symbol* found = Find(scope, name->name);
      if (found == nullptr) {
        Report("flow.name.unknown",
               absl::StrCat("Unknown destination ", Quoted(name->name),
                            " (known: ", Known(scope), ")."),
               expression->location);
        return Ref{Ref::Kind::kUnknown, name->name};
      }
      if (found->kind == SymbolKind::kCall) {
        Report("flow.name.call-as-stream",
               absl::StrCat(Quoted(name->name),
                            " is a call; name the port to write, like ",
                            name->name, ".input."),
               expression->location);
        return Ref{Ref::Kind::kUnknown, name->name};
      }
      if (found->kind == SymbolKind::kSink) {
        ++found->writes;
        Ref ref;
        ref.label = name->name;
        ref.writable = true;
        ref.node = found->ref;
        ref.symbol = Lookup(scope, name->name);
        return ref;
      }
      if (found->kind == SymbolKind::kBarrier) {
        Report("flow.name.not-writable",
               absl::StrCat(Quoted(name->name),
                            " is a barrier, not somewhere to write."),
               expression->location);
        return Ref{Ref::Kind::kUnknown, name->name};
      }
      if (!found->writable) {
        Report("flow.name.not-writable",
               absl::StrCat(Quoted(name->name),
                            " cannot be written by this flow (an 'in' port and "
                            "a call's output are read, not written)."),
               expression->location);
        return Ref{Ref::Kind::kUnknown, name->name};
      }
      ++found->writes;
      Ref ref;
      ref.kind = found->kind == SymbolKind::kNode ? Ref::Kind::kNode
                                                  : Ref::Kind::kPort;
      ref.label = name->name;
      ref.writable = true;
      ref.has_front = true;
      ref.symbol = Lookup(scope, name->name);
      ref.node = found->ref;
      return ref;
    }
    if (const auto* attr = syntax::As<syntax::Attr>(expression);
        attr != nullptr) {
      const auto* base = syntax::As<syntax::Name>(attr->base.get());
      if (base != nullptr) {
        const size_t index = Lookup(scope, base->name);
        if (index == kNoSymbol) {
          Report("flow.name.unknown",
                 absl::StrCat("Unknown destination ", Quoted(base->name), "."),
                 expression->location);
          return Ref{Ref::Kind::kUnknown, base->name};
        }
        if (resolved_.symbols[index].kind != SymbolKind::kCall) {
          Report("flow.name.not-writable",
                 absl::StrCat(Quoted(base->name), " is not a call, so ",
                              Quoted(attr->name), " is not a port to write."),
                 expression->location);
          return Ref{Ref::Kind::kUnknown, base->name};
        }
        ++resolved_.symbols[index].reads;
        return CallPort(index, attr->name, syntax::PortDirection::kInput,
                        expression->location);
      }
    }
    Report("flow.name.not-writable",
           "A destination is an 'out' port or a call's input port.",
           expression->location);
    return Ref{Ref::Kind::kUnknown, Unparse(expression)};
  }

  // -- expressions -----------------------------------------------------------

  /// Bind the names inside an expression. `allow_it` is what a `where`/`map`
  /// stage passes, being the only place `it` means anything.
  ///
  /// The streams it mentions become the graph's [graph::Expr]: the syntax node
  /// that named each one, and the ref it named. Keyed on the node rather than
  /// rewritten into the tree, which is what lets the tree stay immutable and
  /// shared -- the Python reference replaced each name with a `RefValue`, and a
  /// tree handed across a language boundary cannot do that.
  graph::ExprId ResolveExpression(const Node* expression, Scope& scope,
                                  bool allow_it) {
    if (expression == nullptr) {
      return graph::kNone;
    }
    std::vector<const Node*> checking{expression};
    while (!checking.empty()) {
      const auto* node = checking.back();
      checking.pop_back();
      if (node->kind == NodeKind::kOutcome) {
        continue;
      }
      if (const auto* attr = syntax::As<syntax::Attr>(node);
          attr != nullptr && attr->name == "status" &&
          DescribeExpression(attr->base.get(), scope).category ==
              ExpressionCategory::kSink) {
        continue;
      }

      const auto* name = syntax::As<syntax::Name>(node);
      const auto index =
          name == nullptr ? kNoSymbol : Lookup(scope, name->name);
      if (node->kind == NodeKind::kPipe ||
          (index != kNoSymbol &&
           resolved_.symbols[index].kind == SymbolKind::kSink)) {
        Report("flow.form.sink-as-source",
               "A continuation sink is a destination or completion handle, "
               "not payload data. Read its status explicitly.",
               node->location, Severity::kError, Family::kForm);
        continue;
      }
      syntax::VisitChildren(
          *node, [&](const Node& child) { checking.push_back(&child); });
    }
    allow_it = allow_it || inherited_it_;
    const bool outer_it = inherited_it_;
    inherited_it_ = allow_it;
    if (builder_ == nullptr) {
      WalkExpression(expression, scope, allow_it, nullptr);
      inherited_it_ = outer_it;
      return graph::kNone;
    }
    bool effectful = false;
    syntax::VisitSubtree(*expression, [&](const Node& n) {
      if (IsOperation(&n) || n.kind == NodeKind::kWait ||
          n.kind == NodeKind::kClose) {
        effectful = true;
      }
    });
    const graph::BodyId outer_body = body_;
    if (effectful) {
      body_ = builder_->AddBody("expression", outer_body);
    }
    graph::Expr expr;
    expr.node = expression;
    WalkExpression(expression, scope, allow_it, &expr);
    const graph::ExprId raw = builder_->AddExpr(std::move(expr));
    inherited_it_ = outer_it;
    bool extracts = false;
    syntax::VisitSubtree(*expression, [&](const Node& node) {
      const auto* take = syntax::As<syntax::PipelineValue>(&node);
      if (take == nullptr || !take->take) {
        return;
      }
      extracts = true;
      if (!allow_it) {
        return;
      }
      const auto source = extraction_sources_.find(take);
      if (source == extraction_sources_.end()) {
        return;
      }
      const auto dynamic = dynamic_extractions_.find(source->second);
      if (last_extraction_.contains(source->second) ||
          (dynamic != dynamic_extractions_.end() &&
           dynamic->second != expression)) {
        Report("flow.sequence.conflicting-access",
               "Competing per-item reads of an outer stream need a sequential "
               "for loop.",
               take->location);
      }
      dynamic_extractions_[source->second] = expression;
      body_extractions_[body_].insert(source->second);
    });
    if (!effectful) {
      if (!extracts || allow_it) {
        return raw;
      }
      graph::Ref source;
      source.kind = graph::RefKind::kExpr;
      source.owner = body_;
      source.expr = raw;
      source.label = "take expression";
      const auto ref = builder_->AddRef(std::move(source));
      const auto initialized =
          InitializeValue(ref, expression->location, "take expression",
                          expression, true)
              .first;
      graph::Expr result;
      result.node = expression;
      result.materialized = true;
      result.refs.push_back(initialized);
      result.bound.emplace_back(expression, initialized);
      return builder_->AddExpr(std::move(result));
    }
    graph::Ref value;
    value.kind = graph::RefKind::kExpr;
    value.owner = body_;
    value.expr = raw;
    value.label = "expression result";
    const graph::RefId ref = builder_->AddRef(std::move(value));
    const graph::StepId capture = NewStep(
        graph::StepKind::kCapture, "expression result", expression->location);
    builder_->step(capture).source = ref;
    builder_->step(capture).slot = "$result";
    if (extracts && !allow_it) {
      OrderExtraction(*expression, capture);
    }
    graph::Expr activation;
    activation.node = expression;
    activation.activation = body_;
    body_ = outer_body;
    return builder_->AddExpr(std::move(activation));
  }

  static bool IsOperation(const Node* node) {
    return node != nullptr && (node->kind == NodeKind::kCallExpression ||
                               node->kind == NodeKind::kNodeExpression ||
                               node->kind == NodeKind::kPipe);
  }

  Ref CompletionValue(const Node* expression, Scope& scope) {
    std::vector<StepPlan> steps;
    const size_t first =
        builder_ == nullptr ? 0 : builder_->flow().steps.size();
    ResolveStatement(expression, scope, steps);
    if (pending_steps_ != nullptr) {
      for (auto& step : steps) {
        pending_steps_->push_back(std::move(step));
      }
    }
    Ref result;
    result.kind = Ref::Kind::kStatus;
    result.label = Unparse(expression);
    if (builder_ != nullptr) {
      for (size_t i = first; i < builder_->flow().steps.size(); ++i) {
        auto& step = builder_->step(i);
        if ((step.kind == graph::StepKind::kWait ||
             step.kind == graph::StepKind::kClose) &&
            step.location.start == expression->location.start) {
          step.deferred = true;
          result.node = StatusOfStep(i);
        }
      }
    }
    return result;
  }

  size_t ResolveOperation(const Node* node, Scope& scope) {
    const auto cached = operations_.find(node);
    if (cached != operations_.end()) {
      return cached->second;
    }
    const std::string label =
        absl::StrCat("\x1f", "anonymous", anonymous_count_++);
    size_t index;
    if (const auto* call = syntax::As<syntax::CallExpression>(node);
        call != nullptr) {
      std::vector<StepPlan> discarded;
      auto& steps = pending_steps_ == nullptr ? discarded : *pending_steps_;
      index = ResolveCall(*call, scope, label, steps, true);
    } else if (const auto* pipe = syntax::As<syntax::Pipe>(node);
               pipe != nullptr) {
      const auto step = ResolveTransfer(*pipe, scope, *pending_steps_);
      Symbol handle;
      handle.kind = SymbolKind::kSink;
      handle.name = label;
      handle.location = node->location;
      handle.implicit = true;
      handle.readable = false;
      handle.writable = true;
      handle.tolerant = pipe->tolerant;
      handle.ref = TransferSink(step);
      handle.step = step;
      Define(scope, std::move(handle));
      index = resolved_.symbols.size() - 1;
    } else {
      ResolveNode(syntax::As<syntax::NodeExpression>(node), scope, label,
                  node->location);
      index = resolved_.symbols.size() - 1;
      resolved_.symbols[index].implicit = true;
    }
    operations_.emplace(node, index);
    return index;
  }

  /// Note that `node` reads `ref`, for the expression being built.
  void Remember(graph::Expr* absl_nullable out, const Node* node,
                graph::RefId ref) {
    if (out == nullptr || ref == graph::kNone) {
      return;
    }
    out->bound.emplace_back(node, ref);
    if (std::find(out->refs.begin(), out->refs.end(), ref) == out->refs.end()) {
      out->refs.push_back(ref);
    }
  }

  void ScalarTip(const Node* expression, Scope&) {
    if (expression == nullptr) {
      return;
    }
    const Node* stream = expression;
    const syntax::Pipeline* pipeline = syntax::As<syntax::Pipeline>(stream);
    if (const auto* value = syntax::As<syntax::PipelineValue>(stream);
        value != nullptr) {
      pipeline = value->pipeline.get();
    }
    if (pipeline != nullptr) {
      for (const auto& stage : pipeline->stages) {
        if (vocabulary::ReducingStages().contains(stage->name) ||
            ((stage->name == "first" || stage->name == "last") &&
             stage->number == 1)) {
          return;
        }
      }
      stream = pipeline->source.get();
    }
    bool anonymous =
        stream != nullptr && stream->kind == NodeKind::kNodeExpression;
    if (const auto* attr = syntax::As<syntax::Attr>(stream);
        attr != nullptr && IsOperation(attr->base.get())) {
      const auto* call = syntax::As<syntax::CallExpression>(attr->base.get());
      if (call == nullptr || call->invalid_target || attr->name == "status") {
        return;
      }
      anonymous = true;
      if (const auto* target = known_.Flow(call->action); target != nullptr) {
        const auto* port =
            target->Port(attr->name, syntax::PortDirection::kOutput);
        if (port == nullptr || port->unary) {
          return;
        }
      }
    }
    if (!anonymous || !scalar_tips_.insert(stream).second) {
      return;
    }
    auto span = stream->extent;
    if (pipeline != nullptr && !pipeline->stages.empty()) {
      span = expression->extent;
    }
    if (span.end <= span.start || span.end > lines_.source().size()) {
      return;
    }
    Report("flow.sequence.scalar-rvalue-read",
           "This scalar read takes one value from the anonymous stream. If "
           "exactly one value is expected, use '| consume' to check that the "
           "stream has one item.",
           span, Severity::kInformation, Family::kSequence);
    const auto text = lines_.source().substr(span.start, span.end - span.start);
    if (text.find('#') == std::string_view::npos) {
      diagnostics_.back().fixes.push_back(
          {"Require exactly one value with '| consume'",
           {{span.start, span.end, absl::StrCat("(", text, " | consume)")}},
           false});
    }
  }

  void CheckConstantConversion(std::string_view type, const Node* value) {
    const auto constant = syntax::ConstantValue(value);
    if (!constant.has_value()) {
      return;
    }
    bool loses = false;
    if (constant->kind == syntax::Constant::Kind::kString) {
      double numeric;
      if (type == "duration") {
        loses = !ParseDuration(constant->text).has_value() &&
                !absl::SimpleAtod(constant->text, &numeric);
      }
      if (type == "time") {
        loses = !ParseTime(constant->text).has_value() &&
                !absl::SimpleAtod(constant->text, &numeric);
      }
    }
    if ((type == "int" || type == "integer") &&
        constant->kind == syntax::Constant::Kind::kDouble) {
      loses = std::trunc(constant->number) != constant->number;
    }
    if (loses) {
      Report("flow.sequence.lossy-conversion",
             "This constant conversion loses precision or falls back to zero; "
             "use a checked parser when invalid text should fail.",
             value->location, Severity::kWarning, Family::kSequence);
    }
  }

  void WalkExpression(const Node* expression, Scope& scope, bool allow_it,
                      graph::Expr* absl_nullable out) {
    if (expression == nullptr) {
      return;
    }
    if (!budget_.Check()) {
      return;
    }
    DescribeExpression(expression, scope);
    switch (expression->kind) {
      case NodeKind::kCallExpression:
        if (syntax::As<syntax::CallExpression>(expression)->invalid_target) {
          return;
        }
        Report("flow.form.handle-escape",
               "A call handle is a capability, and cannot be embedded as data. "
               "Select an output port or status.",
               expression->location, Severity::kError, Family::kForm);
        return;
      case NodeKind::kNodeExpression: {
        const auto stream = AsStream(expression, scope);
        ScalarTip(expression, scope);
        if (stream.has_value()) {
          Remember(out, expression, stream->node);
        }
        return;
      }
      case NodeKind::kWait:
      case NodeKind::kClose: {
        const auto value = AsStream(expression, scope);
        if (value.has_value()) {
          Remember(out, expression, value->node);
        }
        return;
      }
      case NodeKind::kIt:
        if (!allow_it) {
          Report("flow.name.it-outside-stage",
                 "'it' names the value a 'where' or 'map' stage is looking at, "
                 "and there is none here.",
                 expression->location);
        }
        return;
      case NodeKind::kDiscard:
        // In an expression, and in whatever an expression is read into: an
        // argument, a condition, a header, a `map`, a log's format argument.
        ReportDiscardRead(expression, "read");
        return;
      case NodeKind::kLiteral:
      case NodeKind::kError:
        return;
      case NodeKind::kListLiteral:
        for (const syntax::NodePtr& item :
             syntax::As<syntax::ListLiteral>(expression)->items) {
          WalkExpression(item.get(), scope, allow_it, out);
        }
        return;
      case NodeKind::kObjectLiteral:
        for (const auto& [key, value] :
             syntax::As<syntax::ObjectLiteral>(expression)->pairs) {
          WalkExpression(value.get(), scope, allow_it, out);
        }
        return;
      case NodeKind::kSpread:
        // What is spread has to be a value with parts, but which parts it has
        // is not known until it is read; the walk is of the thing being spread.
        WalkExpression(syntax::As<syntax::Spread>(expression)->value.get(),
                       scope, allow_it, out);
        return;
      case NodeKind::kBuiltin: {
        const auto* function = syntax::As<syntax::Builtin>(expression);
        if (function->name == "zip" || function->name == "interleave") {
          Remember(out, expression,
                   ResolveStreams(function->name, function->args, scope).node);
          return;
        }
        if (function->args.size() == 1) {
          CheckConstantConversion(function->name, function->args.front().get());
        }
        for (const syntax::NodePtr& argument : function->args) {
          WalkExpression(argument.get(), scope, allow_it, out);
        }
        return;
      }
      case NodeKind::kTypedValue: {
        const auto* typed = syntax::As<syntax::TypedValue>(expression);
        // The type is checked as far as it can be here -- a built-in name is
        // either known or misspelt -- and a tag is left to the runtime, which
        // is the only place that knows what has been registered.
        if (!absl::StrContains(typed->type.name, '.') &&
            !absl::StrContains(typed->type.name, '/') && !typed->type.quoted) {
          PortType(typed->type);
        }
        // A shape declared in this file is the one type whose fields are known
        // here, so it is the one whose literal can be checked before it runs.
        if (const DtoPlan* shape = known_.Dto(typed->type.name);
            shape != nullptr) {
          CheckShapeLiteral(*shape, typed->value.get());
        }
        CheckConstantConversion(typed->type.name, typed->value.get());
        WalkExpression(typed->value.get(), scope, allow_it, out);
        return;
      }
      case NodeKind::kUnary:
        WalkExpression(syntax::As<syntax::Unary>(expression)->operand.get(),
                       scope, allow_it, out);
        return;
      case NodeKind::kBinary: {
        const auto* binary = syntax::As<syntax::Binary>(expression);
        WalkExpression(binary->left.get(), scope, allow_it, out);
        WalkExpression(binary->right.get(), scope, allow_it, out);
        return;
      }
      case NodeKind::kPipelineValue: {
        const auto* value = syntax::As<syntax::PipelineValue>(expression);
        const auto source = ResolvePipeline(*value->pipeline, scope);
        Remember(out, expression, source.node);
        if (value->take) {
          extraction_sources_[value] = source.node;
        }
        return;
      }
      case NodeKind::kOutcome:
        Remember(
            out, expression,
            ResolveOutcome(
                syntax::As<syntax::Outcome>(expression)->subject.get(), scope)
                .node);
        return;
      case NodeKind::kName: {
        const auto* name = syntax::As<syntax::Name>(expression);
        const auto index = Lookup(scope, name->name);
        if (index != kNoSymbol &&
            resolved_.symbols[index].kind == SymbolKind::kCall) {
          Report("flow.form.handle-escape",
                 "A call handle cannot be embedded as data. Select an output "
                 "port or status.",
                 expression->location, Severity::kError, Family::kForm);
          return;
        }
        const std::optional<Ref> stream = AsStream(expression, scope);
        if (stream.has_value()) {
          Remember(out, expression, stream->node);
        }
        return;
      }
      case NodeKind::kAttr: {
        const auto* attr = syntax::As<syntax::Attr>(expression);
        if (const std::optional<Ref> named = AttrStream(attr, scope);
            named.has_value()) {
          ScalarTip(expression, scope);
          Remember(out, expression, named->node);
          return;
        }
        CheckMember(attr, scope);
        WalkExpression(attr->base.get(), scope, allow_it, out);
        return;
      }
      case NodeKind::kIndex: {
        const auto* index = syntax::As<syntax::Index>(expression);
        WalkExpression(index->base.get(), scope, allow_it, out);
        WalkExpression(index->index.get(), scope, allow_it, out);
        return;
      }
      default:
        Report("flow.form.statement-as-value",
               absl::StrCat("Cannot use a ",
                            syntax::NodeKindName(expression->kind),
                            " as a value."),
               expression->location, Severity::kError, Family::kForm);
        return;
    }
  }

  /// The field names a value is known to have, and what said so.
  ///
  /// Two things in this language say what a value holds: a port declared with a
  /// `struct`, and a `match` pattern, whose holes *are* its fields. Nothing
  /// else does, and where nothing said, nothing is checked -- a value carrying
  /// `object` or `json` may hold anything, and reporting a field it has would
  /// be worse than reporting none.
  struct Fields {
    /// What to call it in the message: `'Source'`, or `the pattern`.
    std::string subject;
    std::vector<std::string> names;
  };

  [[nodiscard]] std::optional<Fields> FieldsOfPattern(
      const std::string& text) const {
    if (text.empty()) {
      return std::nullopt;
    }
    const pattern::Compiled compiled = pattern::Compile(text);
    // A pattern that does not read is reported where it is written; nothing is
    // known about what it names, so nothing is checked here. Nor is a
    // positional one, which names no fields at all.
    if (!compiled.ok() || !compiled.pattern.AllNamed()) {
      return std::nullopt;
    }
    Fields found;
    found.subject = "the pattern";
    for (const pattern::Hole& hole : compiled.pattern.holes) {
      found.names.push_back(hole.name);
    }
    return found;
  }

  [[nodiscard]] std::optional<Fields> FieldsOfSymbol(
      const Symbol& symbol) const {
    if (symbol.kind == SymbolKind::kValue) {
      return FieldsOfPattern(symbol.pattern);
    }
    if (symbol.kind != SymbolKind::kInputPort &&
        symbol.kind != SymbolKind::kOutputPort) {
      return std::nullopt;
    }
    const syntax::PortDirection direction =
        symbol.kind == SymbolKind::kInputPort ? syntax::PortDirection::kInput
                                              : syntax::PortDirection::kOutput;
    const PortPlan* port = resolved_.plan.Port(symbol.name, direction);
    if (port == nullptr) {
      return std::nullopt;
    }
    const DtoPlan* shape = known_.Dto(port->type);
    if (shape == nullptr) {
      return std::nullopt;
    }
    Fields found;
    found.subject = Quoted(shape->name);
    found.names = shape->FieldNames();
    return found;
  }

  /// `x.field` against what `x` is known to hold.
  ///
  /// Only where the base is a name or `it`, and only one level deep: a field
  /// that holds a record of its own says nothing about *its* keys, so
  /// `src.meta.title` checks `meta` and stops. Conservative on purpose -- the
  /// value of this check is that it never cries wolf.
  void CheckMember(const syntax::Attr* attr, Scope& scope) {
    if (attr->name.empty()) {
      return;
    }
    const auto metadata = DescribeExpression(attr->base.get(), scope);
    if (metadata.shape != nullptr) {
      if (metadata.shape->Field(attr->name) == nullptr) {
        Report("flow.form.unknown-field",
               absl::StrCat(Quoted(metadata.shape->name), " has no field ",
                            Quoted(attr->name), " (it has: ",
                            absl::StrJoin(metadata.shape->FieldNames(), ", "),
                            ")."),
               attr->location, Severity::kError, Family::kForm);
      }
      return;
    }
    std::optional<Fields> known;
    if (syntax::As<syntax::It>(attr->base.get()) != nullptr) {
      known = FieldsOfPattern(it_pattern_);
    } else if (const auto* base = syntax::As<syntax::Name>(attr->base.get());
               base != nullptr) {
      const size_t index = Lookup(scope, base->name);
      if (index == kNoSymbol) {
        return;
      }
      known = FieldsOfSymbol(resolved_.symbols[index]);
    }
    if (!known.has_value() || known->names.empty()) {
      return;
    }
    if (std::find(known->names.begin(), known->names.end(), attr->name) !=
        known->names.end()) {
      return;
    }
    Report("flow.form.unknown-field",
           absl::StrCat(known->subject, " has no field ", Quoted(attr->name),
                        " (it has: ", absl::StrJoin(known->names, ", "), ")."),
           attr->location, Severity::kError, Family::kForm);
  }

  /// `Shape{...}`, against the shape it names.
  ///
  /// Only what is knowable without running anything: a key the shape does not
  /// have, a constant that could not be a value of the field's type, and --
  /// when nothing is spread in -- a required field left out. A spread makes the
  /// set of keys a run-time fact, so the missing-field check stands down rather
  /// than guessing; the coercion at run time is what catches it then, with the
  /// same words.
  void CheckShapeLiteral(const DtoPlan& shape, const Node* value) {
    const auto* object = syntax::As<syntax::ObjectLiteral>(value);
    if (object == nullptr) {
      return;
    }
    absl::flat_hash_set<std::string> given;
    bool spread = false;
    for (const auto& [key, held] : object->pairs) {
      if (syntax::As<syntax::Spread>(held.get()) != nullptr) {
        spread = true;
        continue;
      }
      given.insert(key);
      const FieldPlan* field = shape.Field(key);
      if (field == nullptr) {
        Report("flow.form.unknown-field",
               absl::StrCat(
                   Quoted(shape.name), " has no field ", Quoted(key),
                   " (it has: ", absl::StrJoin(shape.FieldNames(), ", "), ")."),
               held == nullptr ? value->location : held->location,
               Severity::kError, Family::kForm);
        continue;
      }
      // A constant is the only value whose kind is known here. Anything read at
      // run time is checked when it arrives.
      const std::optional<syntax::Constant> constant =
          syntax::ConstantValue(held.get());
      if (!constant.has_value() || !field->dto_name.empty()) {
        continue;
      }
      if (ConstantFits(*constant, field->type)) {
        continue;
      }
      Report("flow.form.field-type-mismatch",
             absl::StrCat(Quoted(key), " of ", Quoted(shape.name), " holds ",
                          field->declared, ", and this is a ",
                          syntax::ConstantKindName(constant->kind), "."),
             held->location, Severity::kError, Family::kForm);
    }
    if (spread) {
      return;
    }
    std::vector<std::string> missing;
    for (const FieldPlan& field : shape.fields) {
      if (!field.required || field.has_default) {
        continue;
      }
      if (given.contains(field.name)) {
        continue;
      }
      missing.push_back(field.name);
    }
    if (missing.empty()) {
      return;
    }
    Report(
        "flow.form.missing-field",
        absl::StrCat(Quoted(shape.name), " requires ",
                     absl::StrJoin(missing, ", "), ", which this leaves out."),
        value->location, Severity::kError, Family::kForm);
  }

  void Constant(const Node* expression) {
    if (expression == nullptr) {
      return;
    }
    if (syntax::ConstantValue(expression).has_value()) {
      return;
    }
    if (expression->kind == NodeKind::kError) {
      return;
    }
    Report("flow.syntax.constant-required",
           "Expected a constant value here (a literal, list or object).",
           expression->location, Severity::kError, Family::kSyntax);
  }

  /// What one `repeat` knows about itself while its body is being read.
  /// The pattern `it` refers to in the stage being resolved, if any.
  std::string it_pattern_;

  /// How many `if`, `for` or `repeat` bodies enclose the statement in hand.
  /// See [ResolveStatements] and [CheckReachedByChoice].
  int guarded_ = 0;

  /// A loop being resolved, so the statements that belong to one can find it.
  ///
  /// `repeat` and `for` share this because they share `until`/`while`: both are
  /// a body run more than once, and "stop when this holds" means the same thing
  /// to each. They do not share `<-`, which is why `carries_a_value` is here --
  /// a `for` gets its value from its stream and has nothing to carry.
  struct LoopState {
    std::string label;
    /// The graph step it is, so `<-` and `until` can fill it in.
    graph::StepId step = graph::kNone;
    std::string carries;
    bool has_carry_name = false;
    bool carried = false;
    bool stopped = false;
    /// False for a `for`. What `<-` checks.
    bool carries_a_value = true;
    /// How many passes may run at once, so `until` can refuse to be written on
    /// a loop where "the pass that just finished" names several passes.
    int parallel = 1;
  };

  CompilationBudget& budget_;
  const LineIndex& lines_;
  const syntax::FlowDeclaration& declaration_;
  const Program& known_;
  /// The shapes the file declares, for [PortType]. A set beside `known_` rather
  /// than a scan of it: a type is read once per port and once per cast.
  DtoNames dtos_;
  ResolvedFlow& resolved_;
  std::vector<Diagnostic>& diagnostics_;
  /// Null on the editor path: no graph, and none of the work of building one.
  graph::GraphBuilder* absl_nullable builder_ = nullptr;
  /// The body statements are being read into, which is what a ref made here
  /// owns.
  graph::BodyId body_ = graph::kNone;
  graph::StepId phase_gate_ = graph::kNone;
  /// The graph steps the last [ResolveAfter] found, for the one statement that
  /// makes several steps out of one `after`.
  std::vector<graph::StepId> after_waits_;
  /// The plan steps the statement being resolved is appending to; see
  /// [ResolveStatements]. Null outside one.
  std::vector<StepPlan>* pending_steps_ = nullptr;
  absl::flat_hash_map<const Node*, size_t> operations_;
  absl::flat_hash_map<const syntax::PipelineValue*, graph::RefId>
      extraction_sources_;
  absl::flat_hash_map<graph::RefId, graph::StepId> last_extraction_;
  absl::flat_hash_map<graph::RefId, const Node*> dynamic_extractions_;
  absl::flat_hash_map<graph::BodyId, absl::flat_hash_set<graph::RefId>>
      body_extractions_;
  absl::flat_hash_set<const Node*> scalar_tips_;
  bool inherited_it_ = false;
  size_t anonymous_count_ = 0;
  absl::flat_hash_map<std::string, int> labels_;
  std::string node_map_;
  /// The loops enclosing the statement being resolved, innermost last.
  std::vector<LoopState> loops_;
  /// Whether a statement is being resolved on behalf of a name.
  ///
  /// Only a tolerated pipe asks: an unnamed one has nowhere to report its
  /// failure, and the statement itself cannot otherwise tell.
  int binding_ = 0;
};

}  // namespace

const PortPlan* absl_nullable FlowPlan::Port(
    std::string_view port_name, syntax::PortDirection direction) const {
  for (const PortPlan& port : ports) {
    if (port.name == port_name && port.direction == direction) {
      return &port;
    }
  }
  return nullptr;
}

std::vector<std::string> FlowPlan::PortNames(
    syntax::PortDirection direction) const {
  std::vector<std::string> names;
  for (const PortPlan& port : ports) {
    if (port.direction == direction) {
      names.push_back(port.name);
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

const FlowPlan* absl_nullable Program::Flow(std::string_view name) const {
  // An empty name matches nothing, so the entry flow cannot be reached here --
  // which is what makes it unaddressable from a `run` or a `call` rather than
  // merely undocumented.
  if (name.empty()) {
    for (const auto& flow : context_flows) {
      if (flow.name == name) {
        return &flow;
      }
    }
    return nullptr;
  }
  for (const FlowPlan& flow : flows) {
    if (!flow.entry && flow.name == name) {
      return &flow;
    }
  }
  for (const auto& flow : context_flows) {
    if (flow.name == name) {
      return &flow;
    }
  }
  return nullptr;
}

const FlowPlan* absl_nullable Program::Entry() const {
  for (const FlowPlan& flow : flows) {
    if (flow.entry) {
      return &flow;
    }
  }
  return nullptr;
}

const DtoPlan* absl_nullable Program::Dto(std::string_view name) const {
  for (const DtoPlan& dto : dtos) {
    if (dto.name == name) {
      return &dto;
    }
  }
  for (const auto& dto : context_dtos) {
    if (dto.name == name) {
      return &dto;
    }
  }
  return nullptr;
}

const FieldPlan* absl_nullable DtoPlan::Field(
    std::string_view field_name) const {
  for (const FieldPlan& field : fields) {
    if (field.name == field_name) {
      return &field;
    }
  }
  return nullptr;
}

std::vector<std::string> DtoPlan::FieldNames() const {
  std::vector<std::string> names;
  names.reserve(fields.size());
  for (const FieldPlan& field : fields) {
    names.push_back(field.name);
  }
  return names;
}

bool ResolveResult::HasErrors() const {
  return FirstError() != nullptr;
}

const Diagnostic* absl_nullable ResolveResult::FirstError() const {
  return flow::FirstError(diagnostics);
}

std::string_view SymbolKindName(SymbolKind kind) {
  switch (kind) {
    case SymbolKind::kInputPort:
      return "input-port";
    case SymbolKind::kOutputPort:
      return "output-port";
    case SymbolKind::kHeader:
      return "header";
    case SymbolKind::kCall:
      return "call";
    case SymbolKind::kNode:
      return "node";
    case SymbolKind::kNodeMap:
      return "node-map";
    case SymbolKind::kSink:
      return "continuation-sink";
    case SymbolKind::kBarrier:
      return "barrier";
    case SymbolKind::kLoopVariable:
      return "loop-variable";
    case SymbolKind::kCarry:
      return "carry";
    case SymbolKind::kValue:
      return "value";
  }
  return "header";
}

ResolveResult Resolve(std::string_view source, const ParseResult& parsed,
                      bool build_graph, const Program* context,
                      CompilationLimits limits, CompilationBudget* budget) {
  CompilationBudget local_budget(limits);
  if (budget == nullptr) {
    budget = &local_budget;
  }
  ResolveResult result;
  result.diagnostics = parsed.diagnostics;
  const LineIndex lines(source);

  // The shapes first, and all of them: a port may be typed with one, and a
  // shape may name another, so neither declaration order nor the flows can be
  // waited for.
  result.program.dtos = DtoResolver(lines, absl::MakeConstSpan(parsed.dtos),
                                    result.diagnostics, *budget)
                            .Run();
  if (context != nullptr) {
    for (const auto& shape : context->dtos) {
      if (result.program.Dto(shape.name) == nullptr) {
        result.program.context_dtos.push_back(shape);
      }
    }
  }

  absl::flat_hash_map<std::string, std::vector<const syntax::FlowDeclaration*>>
      groups;
  std::vector<std::string> order;
  for (const auto& declaration : parsed.flows) {
    auto [at, added] = groups.try_emplace(declaration->name.text);
    if (added) {
      order.push_back(declaration->name.text);
    }
    at->second.push_back(declaration.get());
  }
  const auto report = [&](std::string code, std::string message,
                          const syntax::FlowDeclaration& declaration,
                          Severity severity = Severity::kError) {
    Diagnostic d;
    d.code = std::move(code);
    d.family = Family::kForm;
    d.severity = severity;
    d.message = std::move(message);
    d.flow = declaration.name.text;
    d.range =
        lines.Between(declaration.location.start, declaration.location.end);
    const auto group = groups.find(declaration.name.text);
    size_t related_count = 0;
    if (group != groups.end()) {
      for (const auto* previous : group->second) {
        if (related_count++ >= 64) {
          break;
        }
        if (previous == &declaration) {
          continue;
        }
        d.related.push_back({"",
                             lines.Between(previous->name.location.start,
                                           previous->name.location.end),
                             "Another contribution to this callable."});
      }
    }
    result.diagnostics.push_back(std::move(d));
  };
  result.flows.resize(order.size());
  result.program.flows.reserve(
      order.size() + (context == nullptr ? 0 : context->flows.size()));
  std::vector<const syntax::FlowDeclaration*> selected;
  for (size_t i = 0; i < order.size(); ++i) {
    if (!budget->Check()) {
      break;
    }
    ResolvedFlow& resolved = result.flows[i];
    auto& contributions = groups.at(order[i]);
    if (contributions.size() > 256) {
      report("flow.form.declaration-conflict",
             "A callable exceeds the limit of 256 schema contributions.",
             *contributions[256]);
      contributions.resize(256);
    }
    const syntax::FlowDeclaration* implementation = nullptr;
    bool first = true;
    for (const syntax::FlowDeclaration* declaration : contributions) {
      ResolvedFlow member;
      FlowResolver(lines, *declaration, result.program, member,
                   result.diagnostics, *budget)
          .Declare();
      resolved.contracts.push_back(member.plan);
      if (first) {
        resolved.plan = member.plan;
        first = false;
      } else {
        bool strengthened = false;
        for (const auto& port : member.plan.ports) {
          const auto* previous = resolved.plan.Port(port.name, port.direction);
          if (previous == nullptr || (port.required && !previous->required)) {
            strengthened = true;
            continue;
          }
          const auto current_type =
              port.declared.empty() ? port.type : port.declared;
          const auto previous_type =
              previous->declared.empty() ? previous->type : previous->declared;
          strengthened |= RefinesType(current_type, previous_type,
                                      result.program) == Refinement::kYes &&
                          RefinesType(previous_type, current_type,
                                      result.program) != Refinement::kYes;
        }
        const absl::Status merged =
            MergeContract(resolved.plan, member.plan, result.program);
        if (absl::IsUnavailable(merged)) {
          report("flow.sequence.schema-refinement-unproven",
                 std::string(merged.message()), *declaration,
                 Severity::kInformation);
        } else if (!merged.ok()) {
          report("flow.form.declaration-conflict",
                 std::string(merged.message()), *declaration);
        } else {
          report(strengthened ? "flow.sequence.schema-refined"
                              : "flow.sequence.redundant-declaration",
                 "Compatible callable contracts share one effective interface.",
                 *declaration, Severity::kInformation);
        }
      }
      if (!declaration->interface_only) {
        if (implementation != nullptr) {
          resolved.ambiguous = true;
          resolved.plan.ambiguous = true;
          report("flow.form.multiple-implementations",
                 "Compatible schemas have distinct executable bodies; a use "
                 "must resolve one implementation.",
                 *declaration, Severity::kWarning);
          if (declaration->entry) {
            report("flow.form.duplicate-flow",
                   "A file may define one entry flow.", *declaration);
          }
        } else {
          implementation = declaration;
          resolved.implementation = member.plan;
        }
      }
    }
    if (context != nullptr) {
      if (const auto* evidence = context->Flow(order[i]); evidence != nullptr) {
        resolved.contracts.push_back(*evidence);
        const auto merged =
            MergeContract(resolved.plan, *evidence, result.program);
        if (absl::IsUnavailable(merged)) {
          report("flow.sequence.schema-refinement-unproven",
                 std::string(merged.message()), *contributions.front(),
                 Severity::kInformation);
        } else if (!merged.ok()) {
          report("flow.form.declaration-conflict",
                 std::string(merged.message()), *contributions.front());
        }
      }
    }
    if (implementation != nullptr) {
      for (const auto& actual : resolved.contracts) {
        if (actual.interface_only) {
          continue;
        }
        for (size_t expected_index = 0;
             expected_index < resolved.contracts.size(); ++expected_index) {
          const auto& expected = resolved.contracts[expected_index];
          const auto* requirement = expected_index < contributions.size()
                                        ? contributions[expected_index]
                                        : implementation;
          const auto checked =
              CheckImplementation(actual, expected, result.program);
          if (absl::IsUnavailable(checked)) {
            report("flow.sequence.schema-refinement-unproven",
                   std::string(checked.message()), *requirement,
                   Severity::kInformation);
          } else if (!checked.ok()) {
            report("flow.form.declaration-schema-mismatch",
                   std::string(checked.message()), *requirement);
          }
        }
      }
      resolved.plan.interface_only = false;
      resolved.plan.imported = implementation->imported;
      resolved.plan.registerable = implementation->registerable;
    }
    const auto* body =
        implementation == nullptr ? contributions.front() : implementation;
    resolved.declaration = body;
    selected.push_back(body);
    result.program.flows.push_back(resolved.plan);
  }
  if (context != nullptr) {
    for (const auto& evidence : context->flows) {
      if (result.program.Flow(evidence.name) == nullptr) {
        result.program.context_flows.push_back(evidence);
      }
    }
  }
  for (size_t i = 0; i < selected.size(); ++i) {
    if (!budget->Check()) {
      break;
    }
    graph::GraphBuilder builder(result.flows[i].graph);
    FlowResolver(lines, *selected[i], result.program, result.flows[i],
                 result.diagnostics, *budget, &builder)
        .ResolveBody();
    result.program.flows[i] = result.flows[i].plan;
    if (!build_graph) {
      result.flows[i].graph = {};
    }
  }
  if (budget->exhausted()) {
    Diagnostic diagnostic;
    diagnostic.code = "flow.form.resource-limit";
    diagnostic.family = Family::kForm;
    diagnostic.severity = Severity::kError;
    diagnostic.message =
        "Compilation exceeded the configured work or time limit.";
    result.diagnostics.push_back(std::move(diagnostic));
  }
  return result;
}

}  // namespace a11::flow
