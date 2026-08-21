// Copyright 2026 The A11 Authors.

#include "a11/flow/resolve.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/time/time.h>
#include <absl/types/span.h>

#include "a11/flow/diagnostic.h"
#include "a11/flow/pattern.h"
#include "a11/flow/plan.h"
#include "a11/flow/syntax.h"
#include "a11/flow/vocabulary.h"

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
  using Reporter = std::function<void(std::string_view code, std::string message,
                                      const Location& location)>;

  TypeReader(const DtoNames& dtos, Reporter report)
      : dtos_(dtos), report_(std::move(report)) {}

  /// What the type *is*, as a plan records it.
  std::string Read(const syntax::TypeExpression& type) {
    if (type.quoted) {
      CheckParameters(type, {0});
      return type.name;
    }
    const std::string declared = vocabulary::Canonical(type.name);
    if (vocabulary::TypeNames().contains(declared)) {
      const absl::Span<const int> allowed = vocabulary::TypeParameters(declared);
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
    if (type.name.find('/') != std::string::npos) {
      CheckParameters(type, {0});
      return type.name;
    }
    // A dotted name is the tag a serialisation registry knows a type by. An
    // undotted one is nothing the language knows, and is far more often a
    // misspelt built-in than a tag somebody meant.
    if (type.name.find('.') != std::string::npos) {
      CheckParameters(type, {0});
      return type.name;
    }
    report_("flow.form.unknown-type",
            absl::StrCat("Unknown type ", Quoted(type.name), " (known: ",
                         absl::StrJoin(Known(), ", "),
                         ", a shape this file declares, a serialisation tag "
                         "like 'a11.sdk.AudioBuffer', or a quoted mimetype)."),
            type.location);
    return type.name;
  }

  /// The shape `type` names, or empty where it names something else.
  std::string DtoOf(const syntax::TypeExpression& type) const {
    if (type.quoted) return "";
    if (vocabulary::TypeNames().contains(vocabulary::Canonical(type.name))) {
      return "";
    }
    return dtos_.contains(type.name) ? type.name : "";
  }

 private:
  std::vector<std::string> Known() const {
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
    for (const int count : allowed) counts.push_back(absl::StrCat(count));
    report_("flow.form.unknown-type",
            absl::StrCat(Quoted(type.name), " takes ",
                         absl::StrJoin(counts, " or "),
                         " type parameter(s), but ", type.ToString(), " gives ",
                         given, "."),
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
/// Deliberately generous: this catches a `default "yes"` on a `bool`, not every
/// way a value could later fail to fit. A field's real validation happens where
/// a value arrives, and a resolver that tried to do it here would be a second,
/// weaker copy of it.
bool ConstantFits(const syntax::Constant& value, std::string_view type) {
  using Kind = syntax::Constant::Kind;
  if (type == "any" || type == "json" || value.kind == Kind::kNull) return true;
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
              std::vector<Diagnostic>& diagnostics)
      : lines_(lines), declared_(declared), diagnostics_(diagnostics) {}

  std::vector<DtoPlan> Run() {
    Collect();
    std::vector<DtoPlan> plans;
    plans.reserve(kept_.size());
    for (const syntax::DtoDeclaration* declaration : kept_) {
      plans.push_back(Resolve(*declaration));
    }
    MarkBinary(plans);
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
      const std::string& name = declaration->name.text;
      if (name.empty()) continue;
      if (vocabulary::TypeNames().contains(vocabulary::Canonical(name))) {
        Report("flow.form.struct-shadows-builtin",
               absl::StrCat("Shape ", Quoted(name),
                            " is named after a built-in type, which nothing "
                            "could then write."),
               declaration->name.location, name);
        continue;
      }
      if (!seen.insert(name).second) {
        Report("flow.form.duplicate-struct",
               absl::StrCat("Shape ", Quoted(name), " is declared twice."),
               declaration->name.location, name);
        continue;
      }
      names_.insert(name);
      kept_.push_back(declaration.get());
    }
  }

  DtoPlan Resolve(const syntax::DtoDeclaration& declaration) {
    DtoPlan plan;
    plan.name = declaration.name.text;
    plan.description = declaration.description;
    plan.location = declaration.location;

    TypeReader reader(names_, [&](std::string_view code, std::string message,
                                  const Location& location) {
      Report(code, std::move(message), location, plan.name);
    });

    absl::flat_hash_set<std::string> seen;
    for (const syntax::FieldDeclarationPtr& field : declaration.fields) {
      if (field->name.text.empty()) continue;
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
  void CheckField(const FieldPlan& entry,
                  const syntax::FieldDeclaration& field,
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
        if (ConstantFits(allowed, type)) continue;
        Report("flow.form.field-type-mismatch",
               absl::StrCat("'one of' on ", Quoted(entry.name), " allows a ",
                            syntax::ConstantKindName(allowed.kind),
                            ", and the field holds ", entry.declared, "."),
               field.location, owner);
        break;
      }
    }
  }

  /// Which shapes hold bytes, following the shapes they name.
  ///
  /// A fixed point rather than a recursive walk, because shapes may name each
  /// other in a cycle and a walk would not come back. Each round marks a shape
  /// that names a marked one; when a round marks nothing, it is done.
  static void MarkBinary(std::vector<DtoPlan>& plans) {
    const auto holds_bytes = [](const FieldPlan& field) {
      return field.type == "bytes" || field.element == "bytes";
    };
    for (DtoPlan& plan : plans) {
      for (const FieldPlan& field : plan.fields) {
        if (holds_bytes(field)) plan.binary = true;
      }
    }
    bool changed = true;
    while (changed) {
      changed = false;
      for (DtoPlan& plan : plans) {
        if (plan.binary) continue;
        for (const FieldPlan& field : plan.fields) {
          for (const std::string& named :
               {field.dto_name, field.element_dto_name}) {
            if (named.empty()) continue;
            for (const DtoPlan& other : plans) {
              if (other.name == named && other.binary) {
                plan.binary = true;
                changed = true;
              }
            }
          }
        }
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
  if (node == nullptr) return "";
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
    case NodeKind::kAttr: {
      const auto* attr = syntax::As<syntax::Attr>(node);
      return absl::StrCat(Unparse(attr->base.get()), ".", attr->name);
    }
    case NodeKind::kIndex: {
      const auto* index = syntax::As<syntax::Index>(node);
      return absl::StrCat(Unparse(index->base.get()), "[",
                          Unparse(index->index.get()), "]");
    }
    case NodeKind::kBuiltin: {
      const auto* builtin = syntax::As<syntax::Builtin>(node);
      std::vector<std::string> args;
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
          "(",
          Unparse(syntax::As<syntax::PipelineValue>(node)->pipeline.get()), ")");
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
  std::string tail;
  if (stage.parallel > 1) {
    absl::StrAppend(&tail, " parallel ", stage.parallel);
    if (!stage.ordered) absl::StrAppend(&tail, " unordered");
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
      if (stage.argument == nullptr) return written(stage.name);
      return written(
          absl::StrCat(stage.name, " ", Unparse(stage.argument.get())));
    case vocabulary::StageArgument::kSortKey: {
      std::string body = stage.name;
      if (stage.argument != nullptr) {
        absl::StrAppend(&body, " by ", Unparse(stage.argument.get()));
      }
      if (stage.descending) absl::StrAppend(&body, " desc");
      return written(body);
    }
    case vocabulary::StageArgument::kFold:
      return written(absl::StrCat(stage.name, " ",
                                  ConstantText(stage.start), " as ",
                                  stage.carried.text, ", ",
                                  Unparse(stage.argument.get())));
    case vocabulary::StageArgument::kDuration:
      return written(absl::StrCat(stage.name, " ",
                                  absl::FormatDuration(stage.duration)));
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
                          stage.is_integer
                              ? absl::StrCat(
                                    static_cast<long long>(stage.number))
                              : absl::StrCat(stage.number));
    case vocabulary::StageArgument::kString:
    case vocabulary::StageArgument::kOptionalString:
      if (stage.text.empty()) return stage.name;
      return absl::StrCat(stage.name, " \"", stage.text, "\"");
    case vocabulary::StageArgument::kExpression:
    case vocabulary::StageArgument::kStream:
      return absl::StrCat(stage.name, " ", Unparse(stage.argument.get()));
    case vocabulary::StageArgument::kLog:
    case vocabulary::StageArgument::kLogFormat: {
      if (stage.log.has_format) {
        return absl::StrCat(stage.name, " \"", stage.log.format, "\"");
      }
      if (stage.log.arguments.empty()) return stage.name;
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
  /// Whether this scope is the body of a `for` or a `repeat`.
  ///
  /// Only `advance` asks, and it has to: its offset is fixed when the file is
  /// compiled, so advancing a name from *outside* a loop moves nothing and every
  /// pass sees the same value. Knowing whether a lookup crossed a loop is the
  /// difference between reporting that and letting it look like it works.
  bool loop_body = false;
};

/// The two passes over one flow declaration.
class FlowResolver {
 public:
  FlowResolver(const LineIndex& lines, const syntax::FlowDeclaration& declaration,
               const Program& known, ResolvedFlow& resolved,
               std::vector<Diagnostic>& diagnostics,
               graph::GraphBuilder* absl_nullable builder = nullptr)
      : lines_(lines), declaration_(declaration), known_(known),
        resolved_(resolved), diagnostics_(diagnostics), builder_(builder) {
    // The shapes are resolved before any flow is, so this is complete by the
    // time a port names one.
    for (const DtoPlan& dto : known.dtos) dtos_.insert(dto.name);
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
    plan.description = declaration_.description;
    plan.location = declaration_.location;
    resolved_.declaration = &declaration_;

    absl::flat_hash_set<std::string> seen;
    if (declaration_.entry) {
      // The arguments, declared by nobody. Every program of this shape wants
      // them and a file that had to write the same two `in` lines each time
      // would be saying what the language already knows. They go in first, so a
      // file that declares one of them by hand is reported as a duplicate
      // rather than silently shadowing the real one.
      for (const vocabulary::EntryPort& argument : vocabulary::EntryPorts()) {
        const std::string_view name = argument.name;
        const std::string_view type = argument.type;
        const bool unary = argument.unary;
        const std::string_view description = argument.description;
        PortPlan entry;
        entry.name = std::string(name);
        entry.direction = syntax::PortDirection::kInput;
        entry.declared = std::string(type);
        entry.type = PortType(syntax::TypeExpression{.name = std::string(type)});
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
      entry.type = PortType(port->type);
      entry.unary = port->unary;
      entry.required = port->required;
      entry.description = port->description;
      entry.location = port->location;
      plan.ports.push_back(std::move(entry));
    }
    for (const syntax::HeaderDeclarationPtr& header : declaration_.headers) {
      HeaderPlan entry;
      entry.name = header->name;
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
    if (builder_ != nullptr) {
      builder_->flow().name = resolved_.plan.name;
      body_ = builder_->AddBody("root");
      builder_->flow().root = body_;
    }
    Scope root;
    for (const PortPlan& port : resolved_.plan.ports) {
      const bool input = port.direction == syntax::PortDirection::kInput;
      Symbol symbol;
      symbol.kind =
          input ? SymbolKind::kInputPort : SymbolKind::kOutputPort;
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
      symbol.readable = true;
      if (builder_ != nullptr) {
        graph::Ref ref;
        ref.kind = graph::RefKind::kHeader;
        ref.label = absl::StrCat("header \"", header.name, "\"");
        ref.owner = body_;
        ref.header = header.name;
        ref.fallback = header.default_value;
        ref.has_fallback = header.has_default;
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
    if (builder_ == nullptr || source == graph::kNone) return graph::kNone;
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
    if (builder_ == nullptr || step == graph::kNone) return graph::kNone;
    graph::Step& one = builder_->step(step);
    if (one.status != graph::kNone) return one.status;
    graph::Ref ref;
    ref.kind = graph::RefKind::kStatus;
    ref.label = absl::StrCat("status ", one.label);
    ref.owner = one.body;
    ref.subject_step = step;
    const graph::RefId made = builder_->AddRef(std::move(ref));
    builder_->step(step).status = made;
    return made;
  }

  /// Which subject of a race won, as the value that barrier is.
  ///
  /// Memoised on the step the way a call's status is: `n = wait first of a, b`
  /// and `wait first of a, b -> n` are one race however many read it.
  graph::RefId WinnerOfStep(graph::StepId step) {
    if (builder_ == nullptr || step == graph::kNone) return graph::kNone;
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
    if (builder_ == nullptr || step == graph::kNone) return graph::kNone;
    graph::Ref ref;
    ref.kind = graph::RefKind::kStatus;
    ref.label = absl::StrCat("status ", builder_->step(step).label);
    ref.owner = builder_->step(step).body;
    ref.subject_step = step;
    return builder_->AddRef(std::move(ref));
  }

  graph::RefId StatusOfRef(graph::RefId subject, std::string_view label) {
    if (builder_ == nullptr || subject == graph::kNone) return graph::kNone;
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
        made.count = static_cast<long long>(stage.number);
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
    resolved_.symbols.push_back(std::move(symbol));
    scope.names[resolved_.symbols.back().name] = resolved_.symbols.size() - 1;
  }

  size_t Lookup(const Scope& scope, std::string_view name) const {
    for (const Scope* at = &scope; at != nullptr; at = at->parent) {
      const auto found = at->names.find(name);
      if (found != at->names.end()) return found->second;
    }
    return kNoSymbol;
  }

  Symbol* Find(const Scope& scope, std::string_view name) {
    const size_t index = Lookup(scope, name);
    return index == kNoSymbol ? nullptr : &resolved_.symbols[index];
  }

  /// Whether @p name is declared outside the innermost loop body enclosing
  /// @p scope. False when there is no loop, and false when the name is the
  /// loop's own or was declared beside the `advance`.
  bool DeclaredOutsideTheLoop(const Scope& scope, std::string_view name) const {
    bool crossed = false;
    for (const Scope* at = &scope; at != nullptr; at = at->parent) {
      if (at->names.contains(name)) return crossed;
      // Read *after* the lookup: a name declared in the loop body is found in
      // the scope that is the loop body, and that is not a crossing.
      if (at->loop_body) crossed = true;
    }
    return false;
  }

  /// Every name in scope, sorted -- what a message lists as what was available.
  std::string Known(const Scope& scope) const {
    std::vector<std::string> names;
    for (const Scope* at = &scope; at != nullptr; at = at->parent) {
      for (const auto& [name, index] : at->names) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return absl::StrJoin(names, ", ");
  }

  std::string Label(std::string_view base) {
    const int count = ++labels_[std::string(base)];
    return count == 1 ? std::string(base)
                      : absl::StrCat(base, "#", count);
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
    return TypeReader(dtos_, [this](std::string_view code, std::string message,
                                    const Location& location) {
             Report(code, std::move(message), location, Severity::kError,
                    Family::kForm);
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
    const graph::BodyId outer = body_;
    if (body != graph::kNone) body_ = body;
    if (guarded) ++guarded_;
    std::vector<StepPlan> steps;
    for (const syntax::NodePtr& statement : statements) {
      // A statement may make a step *while resolving its own source*: a race
      // read where a value is expected is a barrier, and the plan should say so.
      // Kept here rather than threaded through every source: the pointer is the
      // list this statement is being appended to, and nothing else appends.
      std::vector<StepPlan>* const outer_pending = pending_steps_;
      pending_steps_ = &steps;
      ResolveStatement(statement.get(), scope, steps);
      pending_steps_ = outer_pending;
    }
    if (guarded) --guarded_;
    body_ = outer;
    return steps;
  }

  /// Whether a statement that ends the flow may stand where this one does.
  ///
  /// `fail` and `cancel` take no input and wait for nothing, so at the top of a
  /// flow's body they run *at once*, racing every other statement: the flow is
  /// over before the call three lines up has been dispatched. Read top to
  /// bottom they look like a last resort; they are the first thing that
  /// happens.
  ///
  /// Inside an `if`, a `for` or a `repeat` they are conditional on something,
  /// and an `after` is the author saying in the source what they are waiting
  /// for. Either is enough. A `nodes` block is not, because it joins the body
  /// around it and changes nothing about when its statements run; nor is a `{
  /// ... }` block, whose statements race each other exactly as a flow's own do.
  void CheckReachedByChoice(std::string_view code, std::string_view what,
                            const std::vector<syntax::Word>& after,
                            const Location& location) {
    if (guarded_ > 0 || !after.empty()) return;
    Report(code,
           absl::StrCat("This '", what,
                        "' is at the top of a body with no 'after', so it runs "
                        "at once and races every other statement in it. Put it "
                        "in an 'if', or give it an 'after'."),
           location, Severity::kError, Family::kForm);
  }

  /// A stream the runtime binds per pass: a loop's value, its index, a carry.
  graph::RefId BoundRef(graph::BodyId owner, graph::StepId step,
                        std::string role) {
    if (builder_ == nullptr) return graph::kNone;
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
      case NodeKind::kLet:
        return ResolveLet(syntax::As<syntax::Let>(statement), scope, steps);
      case NodeKind::kAdvance:
        return ResolveAdvance(syntax::As<syntax::Advance>(statement), scope,
                              steps);
      case NodeKind::kBlock:
        return ResolveBlock(syntax::As<syntax::Block>(statement), scope, steps);
      case NodeKind::kCallStatement: {
        const auto* held = syntax::As<syntax::CallStatement>(statement);
        ResolveCall(*held->call, scope, Label(held->call->action), steps);
        return;
      }
      case NodeKind::kPipe: {
        const auto* pipe = syntax::As<syntax::Pipe>(statement);
        if (pipe->tolerant && binding_ == 0) {
          // The same complaint `try run` gets when nothing reads its status,
          // and it matters more here: a tolerated pipe that failed leaves a
          // *truncated* stream behind, and its readers see a clean early end.
          Report("flow.unused.try-pipe",
                 "'try' lets this pipe fail without ending the flow, and "
                 "nothing here reads how it went: its destination is then "
                 "closed early and every reader of it sees an ordinary end of "
                 "stream. Give it a name and read it with 'status'.",
                 pipe->location, Severity::kWeakWarning, Family::kUnused);
        }
        const Ref source = ResolvePipeline(*pipe->pipeline, scope);
        // One `after` for however many targets: the barriers are made once and
        // every pipe of the statement waits for the same ones.
        const std::vector<std::string> after = ResolveAfter(pipe->after, scope);
        const std::vector<graph::StepId> held = after_waits_;
        for (const syntax::NodePtr& target : pipe->targets) {
          const Ref destination = ResolveDestination(target.get(), scope);
          StepPlan step;
          step.kind = "pipe";
          step.label = absl::StrCat(source.label, " -> ", destination.label);
          step.source = source.label;
          step.destination = destination.label;
          step.after = after;
          step.location = pipe->location;
          if (builder_ != nullptr) {
            const graph::StepId made =
                NewStep(graph::StepKind::kPipe, step.label, pipe->location);
            graph::Step& one = builder_->step(made);
            one.source = source.node;
            one.destination = destination.node;
            one.after = held;
            one.tolerant = pipe->tolerant;
          }
          steps.push_back(std::move(step));
        }
        return;
      }
      case NodeKind::kSkip: {
        const auto* skip = syntax::As<syntax::Skip>(statement);
        // One `after` for however many targets, as `kPipe` does: the barriers
        // are made once and every target of the statement waits for them.
        const std::vector<std::string> after = ResolveAfter(skip->after, scope);
        const std::vector<graph::StepId> held = after_waits_;
        for (const syntax::SkipTarget& target : skip->targets) {
          ResolveSkipTarget(target, skip->count, scope, after, held, steps);
        }
        return;
      }
      case NodeKind::kWait: {
        const auto* wait = syntax::As<syntax::Wait>(statement);
        // `wait first of a, b` and `wait all of a, b` read several outcomes.
        // One step, several outcomes: what a reader of the plan wants to know
        // is that this statement is one barrier over a set, and whether the
        // first of them is enough.
        if (!wait->subjects.empty()) {
          std::vector<Ref> outcomes;
          std::vector<std::string> labels;
          outcomes.reserve(wait->subjects.size());
          for (const syntax::NodePtr& subject : wait->subjects) {
            outcomes.push_back(ResolveOutcome(subject.get(), scope));
            labels.push_back(SubjectLabel(outcomes.back().label));
          }
          const std::string written = absl::StrCat(
              "wait ", wait->race ? "first" : "all", " of ",
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
              if (!outcome.tolerant) tolerant = false;
            }
            // The first outcome is also the step's own, so everything that
            // reads a wait's outcome keeps working: with `first`, that is
            // whichever won, and the runtime fills it in.
            one.outcome = outcomes.front().node;
            one.tolerant = tolerant;
            if (wait->race) builder_->step(made).winner = WinnerOfStep(made);
          }
          step.after = ResolveAfter(wait->after, scope, made);
          const graph::RefId winner =
              made == graph::kNone ? graph::kNone
                                   : builder_->step(made).winner;
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
              const graph::StepId piped = NewStep(graph::StepKind::kPipe,
                                                  pipe.label, wait->location);
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
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kDrain: {
        const auto* drain = syntax::As<syntax::Drain>(statement);
        const Ref target = ResolveDestination(drain->target.get(), scope);
        StepPlan step;
        step.kind = "drain";
        step.label = target.label;
        step.destination = target.label;
        step.location = drain->location;
        graph::StepId made = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kDrain,
                         absl::StrCat("drain ", target.label), drain->location);
          builder_->step(made).outcome =
              StatusOfRef(target.node, target.label);
        }
        step.after = ResolveAfter(drain->after, scope, made);
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kCancel: {
        const auto* cancel = syntax::As<syntax::Cancel>(statement);
        const size_t called =
            ExpectCall(cancel->name.text, cancel->location, scope);
        StepPlan step;
        step.kind = "cancel";
        step.label = cancel->name.text;
        step.location = cancel->location;
        graph::StepId made = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kCancel,
                         absl::StrCat("cancel ", cancel->name.text),
                         cancel->location);
          builder_->step(made).target =
              called == kNoSymbol ? graph::kNone
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
        CheckReachedByChoice("flow.form.unconditional-abort", "abort",
                             abort->after, abort->location);
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
                             fail->after, fail->location);
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kLog: {
        const auto* log = syntax::As<syntax::Log>(statement);
        const bool formatted = log->tail.has_format;
        const graph::LogTail tail =
            ResolveLogTail(log->tail, scope, /*allow_it=*/false,
                           log->location);
        StepPlan step;
        step.kind = formatted ? "logf" : "log";
        const syntax::Node* subject =
            log->tail.arguments.empty() ? nullptr
                                        : log->tail.arguments.front().get();
        step.label = formatted ? tail.format : Unparse(subject);
        step.location = log->location;
        graph::StepId made = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kLog, step.kind, log->location);
          builder_->step(made).log = tail;
        }
        step.after = ResolveAfter(log->after, scope, made);
        // A log says what just happened, so one at the top of a body with
        // nothing to wait for describes something that has not happened yet.
        // Same rule as `fail` and `cancel`, and for the same reason.
        CheckReachedByChoice(
            formatted ? "flow.form.unconditional-logf"
                      : "flow.form.unconditional-log",
            step.kind, log->after, log->location);
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kNodes:
        return ResolveNodes(syntax::As<syntax::Nodes>(statement), scope, steps);
      case NodeKind::kForEach: {
        const auto* loop = syntax::As<syntax::ForEach>(statement);
        const Ref source = ResolvePipeline(*loop->pipeline, scope);
        StepPlan step;
        step.kind = "for";
        step.label = Label("for");
        step.source = source.label;
        step.location = loop->location;
        graph::StepId made = graph::kNone;
        graph::BodyId inner_body = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kForEach, step.label, loop->location);
          inner_body = builder_->AddBody(absl::StrCat(step.label, ".body"),
                                         body_, made);
          graph::Step& one = builder_->step(made);
          one.source = source.node;
          one.parallel = loop->parallel;
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
        inner.loop_body = true;
        // One name is the value; several take it apart by position, each a
        // derived stream over the one the loop binds -- so `for a, b in
        // zip(x, y)` costs the same as reading `it[0]` and `it[1]` would, and
        // the refs are the ones every other part of the runtime already knows
        // how to materialise and replay.
        const graph::BodyId outer_body = body_;
        body_ = inner_body;
        for (size_t at = 0; at < loop->variables.size(); ++at) {
          Symbol variable;
          variable.kind = SymbolKind::kLoopVariable;
          variable.name = loop->variables[at].text;
          variable.location = loop->variables[at].location;
          variable.ref =
              loop->variables.size() == 1
                  ? item
                  : Derive(item,
                           AtStage(syntax::Constant::Integer(
                               static_cast<long long>(at))),
                           absl::StrCat(loop->variables[at].text));
          Define(inner, variable);
        }
        body_ = outer_body;
        DefineIndex(inner, loop->location, index);
        // A `for` is a loop, so `until`/`while` may end it. It carries nothing,
        // which is what `carries_a_value` tells `<-`.
        LoopState state;
        state.label = step.label;
        state.step = made;
        state.carries_a_value = false;
        state.parallel = std::max(loop->parallel, 1);
        loops_.push_back(state);
        step.bodies.push_back(
            ResolveStatements(loop->body, inner, inner_body, true));
        loops_.pop_back();
        // After the loop's own NewStep, and that order matters: ResolveAfter
        // makes `kWait` steps of its own, and ResolveBind names a statement's
        // *first* step -- so resolving `after` earlier would quietly bind a
        // named loop to one of those waits instead of to the loop.
        step.after = ResolveAfter(loop->after, scope, made);
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kRepeat: {
        const auto* repeat = syntax::As<syntax::Repeat>(statement);
        if (repeat->start != nullptr) Constant(repeat->start.get());
        StepPlan step;
        step.kind = "repeat";
        step.label = Label("repeat");
        step.location = repeat->location;
        graph::StepId made = graph::kNone;
        graph::BodyId inner_body = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kRepeat, step.label, repeat->location);
          inner_body = builder_->AddBody(absl::StrCat(step.label, ".body"),
                                         body_, made);
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
        inner.loop_body = true;
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
        loops_.push_back(state);
        step.bodies.push_back(
            ResolveStatements(repeat->body, inner, inner_body, true));
        // Read before the pop: `until` sets the flag on the entry the body was
        // resolved against, not on the copy pushed in.
        const bool stopped = loops_.back().stopped;
        loops_.pop_back();
        // Nothing ends it. With the old default of `max 16` this was a loop
        // that quietly did sixteen passes and reported success; now that a
        // bound is only ever the author's, a loop with neither is one that runs
        // until something cancels the flow, which nobody writes on purpose.
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
                 absl::StrCat("This repeat carries ",
                              state.has_carry_name ? Quoted(state.carries)
                                                   : "'nothing'",
                              ", not ", Quoted(carry->name.text), "."),
                 carry->location, Severity::kError, Family::kBarrier);
        } else if (state.carried) {
          Report("flow.barrier.duplicate-carry",
                 absl::StrCat(Quoted(carry->name.text),
                              " is already carried."),
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
          const graph::StepId made =
              NewStep(graph::StepKind::kCapture, "capture carry",
                      carry->location);
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
          // The question is asked of the pass that just finished, and with
          // several in flight there is no such pass -- whichever answered first
          // would end the loop while others were still running, so which values
          // were seen would depend on scheduling.
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
            const graph::StepId made = NewStep(
                graph::StepKind::kCapture, "capture condition", until->location);
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
        StepPlan step;
        step.kind = "if";
        step.label = Label("if");
        step.location = branch->location;
        graph::StepId made = graph::kNone;
        graph::BodyId then_body = graph::kNone;
        graph::BodyId else_body = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kIf, step.label, branch->location);
          then_body = builder_->AddBody(absl::StrCat(step.label, ".then"), body_,
                                        made);
          else_body = builder_->AddBody(absl::StrCat(step.label, ".else"), body_,
                                        made);
          graph::Step& one = builder_->step(made);
          one.condition = condition;
          one.bodies.push_back(then_body);
          one.bodies.push_back(else_body);
        }
        Scope then_scope;
        then_scope.parent = &scope;
        step.bodies.push_back(
            ResolveStatements(branch->then_body, then_scope, then_body, true));
        Scope else_scope;
        else_scope.parent = &scope;
        step.bodies.push_back(
            ResolveStatements(branch->else_body, else_scope, else_body, true));
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kError:
        // The parser already said what it could not read; nothing to add.
        return;
      default:
        Report("flow.syntax.unexpected",
               absl::StrCat("Cannot run a ",
                            syntax::NodeKindName(statement->kind),
                            " here."),
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

  /// `let name = pipeline`: one value of that stream, under a name.
  ///
  /// Compiled to the stream with a `first 1` on the end, and a symbol that says
  /// the name is a *value*. Everything after that is machinery the language
  /// already has: an expression mentioning a name reads its first value, a
  /// pipeline whose source is a name reads its stream, and a stream read by two
  /// things is materialised once and replayed. So `let` costs one derived ref
  /// and buys a name that can be compared, branched on and piped.
  ///
  /// It is **lazy**, like every other stream here: nothing is read until
  /// something reads the name. That is what makes `let` free to write next to
  /// the thing it describes rather than where the value is first needed -- and
  /// a `let` nothing reads is reported, because a value nobody looked at is a
  /// line that does nothing.
  /// The literal `match` pattern a pipeline's values came out of, if any.
  ///
  /// Either the pipeline *is* a `match(..)` call or its last stage was a
  /// `match`. Anything after that reshapes the value into something the pattern
  /// no longer describes, so only the last stage is looked at: `| match "p" |
  /// count` is a number, not a record.
  static std::string PatternOf(const syntax::Pipeline& pipeline) {
    if (!pipeline.stages.empty()) {
      const syntax::Stage& last = *pipeline.stages.back();
      return vocabulary::Canonical(last.name) == "match" ? last.text : "";
    }
    const auto* call = syntax::As<syntax::Builtin>(pipeline.source.get());
    if (call == nullptr || vocabulary::Canonical(call->name) != "match") {
      return "";
    }
    if (call->args.empty()) return "";
    const auto* literal = syntax::As<syntax::Literal>(call->args.front().get());
    if (literal == nullptr ||
        literal->value.kind != syntax::Constant::Kind::kString) {
      return "";
    }
    return literal->value.text;
  }

  void ResolveLet(const syntax::Let* let, Scope& scope,
                  std::vector<StepPlan>& steps) {
    absl::flat_hash_set<std::string> taken;
    for (const syntax::Word& name : let->names) {
      // Its own names as well as the scope's: `let age, age = u` defines one
      // and shadows it with the other, which nobody writes on purpose.
      if (Lookup(scope, name.text) != kNoSymbol || !taken.insert(name.text).second) {
        Report("flow.name.taken",
               absl::StrCat(Quoted(name.text),
                            " is already taken in this scope."),
               let->location);
      }
    }
    Ref source = ResolvePipeline(*let->pipeline, scope);
    graph::Stage one;
    one.name = "first";
    one.takes = vocabulary::StageArgument::kNumber;
    one.count = 1;

    const std::string written = absl::StrJoin(
        let->names, ", ",
        [](std::string* out, const syntax::Word& name) {
          absl::StrAppend(out, name.text);
        });

    StepPlan step;
    step.kind = "let";
    step.label = written;
    step.source = source.label;
    step.destination = written;
    step.location = let->location;
    steps.push_back(std::move(step));

    const std::string pattern = PatternOf(*let->pipeline);

    // The one value, whatever it is taken apart into.
    const graph::RefId value_ref =
        Derive(source.node, std::move(one),
               absl::StrCat(source.label, " | first 1"));

    for (size_t at = 0; at < let->names.size(); ++at) {
      const syntax::Word& name = let->names[at];
      Symbol value;
      value.kind = SymbolKind::kValue;
      value.name = name.text;
      value.location = name.location;
      value.readable = true;
      value.writable = false;
      if (let->names.size() == 1) {
        value.ref = value_ref;
        value.pattern = pattern;
        // Where it came from, so `advance` can name the value after this one.
        value.value_source = source.node;
        value.value_offset = 0;
      } else {
        // A part of the value: its field where the value has one, and its
        // position where the value is a list. Which of the two is settled by
        // the value rather than by the text, because `let name, age = user` and
        // `let first, second = pair` are the same statement written twice.
        graph::Stage part;
        part.name = "at";
        part.takes = vocabulary::StageArgument::kString;
        part.text = name.text;
        part.index = static_cast<long long>(at);
        part.named_or_indexed = true;
        value.ref = Derive(value_ref, std::move(part),
                           absl::StrCat(source.label, " | first 1 | ",
                                        name.text));
        // A part has no next one of its own: the stream's next value is another
        // whole tuple, not another `age`.
        value.value_part = true;
      }
      Define(scope, value);
    }
  }

  /// `advance name` -- the same stream's next value, under the same name.
  ///
  /// **Why this is an offset and not a barrier.** Written out, the second
  /// binding is `let x = src` again with an `after` on the first, and what the
  /// `after` buys is that the earlier binding took the earlier value. An offset
  /// gives the same guarantee without the ordering: the *k*th binding of a name
  /// reads the *k*th value of its stream whenever it happens to run, so the
  /// guarantee holds however the flow is scheduled rather than because of how
  /// it was scheduled. That is a stronger promise than a sync point, and it
  /// needs nothing to synchronise.
  ///
  /// The name is rebound rather than redefined: statements written before this
  /// one already hold the ref they resolved against, and everything after it
  /// reads the new one. Which is what shadowing is, and is why `advance` reads
  /// as a statement rather than as a declaration.
  void ResolveAdvance(const syntax::Advance* advance, Scope& scope,
                      std::vector<StepPlan>& steps) {
    Symbol* held = Find(scope, advance->name.text);
    if (held == nullptr) {
      Report("flow.name.unknown",
             absl::StrCat("Unknown name ", Quoted(advance->name.text),
                          " (known: ", Known(scope), ")."),
             advance->location);
      return;
    }
    if (held->kind != SymbolKind::kValue) {
      Report("flow.name.not-advanceable",
             absl::StrCat(Quoted(advance->name.text), " is a ",
                          SymbolKindName(held->kind),
                          ", and only a value a 'let' bound has a next one."),
             advance->location);
      return;
    }
    if (DeclaredOutsideTheLoop(scope, advance->name.text)) {
      // The trap this exists for. `advance` is resolved to
      // `source | drop k | first 1` with `k` fixed while the file is compiled,
      // and a loop body is resolved *once* -- so every pass carries the same
      // offset, a node replays from its start for each pass, and every pass
      // binds the same value. Measured: four passes over `a b c d e` bound `a`
      // four times, silently, and looked like it worked.
      //
      // An error rather than a warning because there is no reading of it that
      // is right, and `for` is the construct that does what this was reaching
      // for.
      Report("flow.form.advance-in-loop",
             absl::StrCat(
                 Quoted(advance->name.text),
                 " was bound outside this loop, and 'advance' moves by a fixed "
                 "step worked out while the file is compiled -- so every pass "
                 "would bind the same value. Walk the stream with "
                 "'for x in ", advance->name.text,
                 "' instead, and end it early with 'until' if it should stop."),
             advance->location, Severity::kError, Family::kForm);
      return;
    }
    // Read before defining: the new symbol shadows this one, and `Define` may
    // move the vector these point into.
    if (held->value_part) {
      Report("flow.name.not-advanceable",
             absl::StrCat(Quoted(advance->name.text),
                          " was taken apart from another value, so it has no "
                          "next one of its own: advance what it came from."),
             advance->location);
      return;
    }
    const graph::RefId from = held->value_source;
    const int offset = held->value_offset + 1;
    const std::string label = held->name;

    StepPlan step;
    step.kind = "advance";
    step.label = label;
    step.location = advance->location;
    steps.push_back(std::move(step));

    Symbol value;
    value.kind = SymbolKind::kValue;
    value.name = label;
    value.location = advance->name.location;
    value.readable = true;
    value.writable = false;
    value.value_source = from;
    value.value_offset = offset;
    if (from != graph::kNone) {
      // `| drop k | first 1` is the value after the last one this name had. An
      // empty stream past that point binds nothing, exactly as a `let` on an
      // empty stream does.
      graph::Stage drop;
      drop.name = "drop";
      drop.takes = vocabulary::StageArgument::kNumber;
      drop.count = offset;
      const graph::RefId dropped =
          Derive(from, std::move(drop),
                 absl::StrCat(builder_->flow().refs[from].label, " | drop ",
                              offset));
      graph::Stage first;
      first.name = "first";
      first.takes = vocabulary::StageArgument::kNumber;
      first.count = 1;
      value.ref = Derive(dropped, std::move(first),
                         absl::StrCat(builder_->flow().refs[dropped].label,
                                      " | first 1"));
    }
    Define(scope, value);
  }

  void ResolveBind(const syntax::Bind* bind, Scope& scope,
                   std::vector<StepPlan>& steps) {
    if (Lookup(scope, bind->name.text) != kNoSymbol) {
      Report("flow.name.taken",
             absl::StrCat(Quoted(bind->name.text),
                          " is already taken in this scope."),
             bind->location);
    }
    const Node* value = bind->value.get();
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
    // A bound `wait`/`drain`: the barrier is the step, and the name reads as
    // the outcome it waited for.
    const size_t before = steps.size();
    const size_t graph_before =
        builder_ == nullptr ? 0 : builder_->flow().steps.size();
    // A pipe with several targets is several steps, and a name can only be one
    // of them -- so refuse it rather than quietly naming the first.
    if (const auto* piped = syntax::As<syntax::Pipe>(value);
        piped != nullptr && piped->targets.size() > 1) {
      Report("flow.form.bind-many-targets",
             absl::StrCat("This pipe writes ", piped->targets.size(),
                          " destinations, which is that many steps, so one name "
                          "cannot stand for it. Write one pipe per destination, "
                          "or leave it unnamed."),
             bind->location, Severity::kError, Family::kForm);
    }
    ++binding_;
    ResolveStatement(value, scope, steps);
    --binding_;
    Symbol barrier;
    barrier.kind = SymbolKind::kBarrier;
    barrier.name = bind->name.text;
    barrier.location = bind->name.location;
    barrier.readable = true;
    if (builder_ != nullptr && builder_->flow().steps.size() > graph_before) {
      // The statement's own step is the first one it made; anything after it is
      // a barrier `after` grew, which belongs to nobody's name.
      barrier.step = graph_before;
      // A race is a *value* -- which of them won -- so that is what its name
      // reads. Every other barrier's name reads the outcome it waited for,
      // which for a race is available as `status a` of the subject itself.
      const graph::RefId winner = builder_->step(graph_before).winner;
      barrier.ref = winner != graph::kNone
                        ? winner
                        : builder_->step(graph_before).outcome;
      if (barrier.ref == graph::kNone &&
          graph::RecordsOutcome(builder_->step(graph_before).kind)) {
        // A loop makes its outcome ref only when something names it, unlike a
        // block, which makes one up front. Lazily, so an unnamed loop's graph
        // is exactly what it was before loops could be named -- which is what
        // keeps `testdata/flow/syntax.json` and every plan consumer unchanged.
        barrier.ref = StatusOfStep(graph_before);
      }
      barrier.tolerant = builder_->step(graph_before).tolerant;
      builder_->step(graph_before).label = bind->name.text;
    }
    Define(scope, barrier);
    if (steps.size() > before) steps[before].label = bind->name.text;
  }

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
    step.label = Label("block");
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
    Scope inner;
    inner.parent = &scope;
    step.bodies.push_back(ResolveStatements(block->body, inner, inner_body));
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
    if (nodes->body.empty()) return;
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

  /// The steps a statement waits for, from its `after` names.
  ///
  /// A name may be a step -- a call, a bound `wait`/`drain` -- or a port or
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
      Symbol* found = Find(scope, word.text);
      if (found == nullptr) {
        Report("flow.name.unknown",
               absl::StrCat("Unknown name ", Quoted(word.text), " (known: ",
                            Known(scope), ")."),
               word.location);
        continue;
      }
      switch (found->kind) {
        case SymbolKind::kCall:
        case SymbolKind::kBarrier:
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
      if (builder_ == nullptr) continue;
      if (found->kind == SymbolKind::kCall ||
          found->kind == SymbolKind::kBarrier) {
        if (found->step != graph::kNone) waits.push_back(found->step);
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
             absl::StrCat("Unknown name ", Quoted(name), " (known: ",
                          Known(scope), ")."),
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

  /// One target of a `skip`, expanded into the plan/graph steps it becomes.
  ///
  /// `after`/`held` are the statement's own barrier, resolved once and applied
  /// to every step a target expands to, the way `kPipe` shares one `after`
  /// across several targets.
  void ResolveSkipTarget(const syntax::SkipTarget& target,
                         std::optional<long long> count, Scope& scope,
                         const std::vector<std::string>& after,
                         const std::vector<graph::StepId>& held,
                         std::vector<StepPlan>& steps) {
    if (target.call.Empty()) {
      // An ordinary pipeline -- unless its source turns out to be a call, in
      // which case `skip act` means every output of it. The parser cannot
      // tell a call from a port, so that is decided here.
      if (target.pipeline->stages.empty()) {
        if (const auto* name =
                syntax::As<syntax::Name>(target.pipeline->source.get());
            name != nullptr) {
          const size_t index = Lookup(scope, name->name);
          if (index != kNoSymbol &&
              resolved_.symbols[index].kind == SymbolKind::kCall) {
            ++resolved_.symbols[index].reads;
            EmitCallOutputSkips(index, {}, name->location, after, held, steps);
            return;
          }
        }
      }
      EmitPlainSkip(*target.pipeline, count, scope, after, held, steps);
      return;
    }
    // `skip o1, o2 of act` / `skip (o1, o2) of act`: named outputs of a call,
    // which is the only thing this shape can mean.
    const size_t index = ExpectCall(target.call.text, target.call.location, scope);
    if (index == kNoSymbol) return;
    EmitCallOutputSkips(index, target.outputs, target.call.location, after,
                        held, steps);
  }

  /// `skip pipeline`, or `skip n reference`: today's single-subject form.
  void EmitPlainSkip(const syntax::Pipeline& pipeline,
                     std::optional<long long> count, Scope& scope,
                     const std::vector<std::string>& after,
                     const std::vector<graph::StepId>& held,
                     std::vector<StepPlan>& steps) {
    const Ref source = ResolvePipeline(pipeline, scope);
    if (count.has_value() && !source.has_front) {
      Report("flow.sequence.skip-count-target",
             absl::StrCat("'skip ", *count, "' takes a port or a node, and ",
                          source.label, " is not one. Use '| drop ", *count,
                          "' to drop values from a pipeline instead."),
             pipeline.location, Severity::kError, Family::kSequence);
    } else if (count.has_value() && builder_ != nullptr &&
               source.node != graph::kNone) {
      // The count is the node's, not this statement's: it accumulates on the
      // one ref, and every reader of it inherits what was taken off.
      builder_->ref(source.node).skip += *count;
    }
    StepPlan step;
    step.kind = "skip";
    step.label = source.label;
    step.source = source.label;
    step.location = pipeline.location;
    if (builder_ != nullptr) {
      const graph::StepId made = NewStep(
          graph::StepKind::kSkip,
          count.has_value()
              ? absl::StrCat("skip ", *count, " of ", source.label)
              : absl::StrCat("skip ", source.label),
          pipeline.location);
      builder_->step(made).source = source.node;
      builder_->step(made).count = count;
      builder_->step(made).after = held;
    }
    step.after = after;
    steps.push_back(std::move(step));
  }

  /// `outputs` of `call`, or (when `outputs` is empty) every one of them: from
  /// a sibling flow's declared ports when they are known here, or as a single
  /// step against the call itself when they are not (an action from a
  /// registry) -- the runtime already drains every output nobody reads, so
  /// there is nothing this step needs to enumerate to be correct.
  void EmitCallOutputSkips(size_t call, std::vector<syntax::Word> outputs,
                           const Location& location,
                           const std::vector<std::string>& after,
                           const std::vector<graph::StepId>& held,
                           std::vector<StepPlan>& steps) {
    const Symbol& symbol = resolved_.symbols[call];
    if (outputs.empty() && symbol.target != nullptr) {
      for (const std::string& name :
           symbol.target->PortNames(syntax::PortDirection::kOutput)) {
        outputs.push_back(syntax::Word{name, location});
      }
    }
    if (outputs.empty() && symbol.target == nullptr) {
      StepPlan step;
      step.kind = "skip";
      step.label = symbol.name;
      step.source = symbol.name;
      step.location = location;
      if (builder_ != nullptr) {
        const graph::StepId made = NewStep(
            graph::StepKind::kSkip, absl::StrCat("skip ", symbol.name),
            location);
        builder_->step(made).call = symbol.step;
        builder_->step(made).after = held;
      }
      step.after = after;
      steps.push_back(std::move(step));
      return;
    }
    for (const syntax::Word& name : outputs) {
      const Ref port = CallPort(call, name.text, syntax::PortDirection::kOutput,
                                name.location);
      StepPlan step;
      step.kind = "skip";
      step.label = port.label;
      step.source = port.label;
      step.location = name.location;
      if (builder_ != nullptr) {
        const graph::StepId made = NewStep(
            graph::StepKind::kSkip, absl::StrCat("skip ", port.label),
            name.location);
        builder_->step(made).source = port.node;
        builder_->step(made).after = held;
      }
      step.after = after;
      steps.push_back(std::move(step));
    }
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
               absl::StrCat("Unknown log level ", Quoted(tail.level.text),
                            " (known: ",
                            absl::StrJoin(vocabulary::LogLevels(), ", "),
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
    if (code == nullptr) return graph::kNone;
    if (const auto* name = syntax::As<syntax::Name>(code); name != nullptr) {
      if (vocabulary::IsStatusCode(name->name)) {
        if (named != nullptr) *named = name->name;
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

  size_t ResolveCall(const syntax::CallExpression& call, Scope& scope,
                     std::string_view label, std::vector<StepPlan>& steps) {
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
    symbol.name = std::string(label);
    symbol.location = call.location;
    symbol.action = call.action;
    symbol.tolerant = call.tolerant;
    symbol.target = known_.Flow(call.action);
    symbol.readable = false;
    Define(scope, symbol);
    const size_t index = resolved_.symbols.size() - 1;

    StepPlan step;
    // The verb it was written with, not "a call": a described step says which
    // of `run` and `call` it is, the way a `wait`/`drain` step says which of
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
      action_id = ResolveExpression(call.modifiers->action_id.get(), scope, false);
    }
    graph::StepId made = graph::kNone;
    if (builder_ != nullptr) {
      made = NewStep(graph::StepKind::kCall, std::string(label), call.location);
      graph::Step& one = builder_->step(made);
      one.action = call.action;
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
    // The call first, then one pipe per argument feeding it -- the order
    // `a11.flow.plan` puts them in, and the order they read in.
    steps.push_back(std::move(step));
    for (const syntax::CallExpression::Argument& argument : call.args) {
      const Ref destination =
          CallPort(index, argument.port.text, syntax::PortDirection::kInput,
                   argument.port.location);
      const Ref source = ResolvePipeline(*argument.pipeline, scope);
      StepPlan feed;
      feed.kind = "pipe";
      feed.label =
          absl::StrCat(source.label, " -> ", label, ".", argument.port.text);
      feed.source = source.label;
      feed.destination = absl::StrCat(label, ".", argument.port.text);
      feed.location = argument.port.location;
      if (builder_ != nullptr) {
        const graph::StepId pipe = NewStep(graph::StepKind::kPipe, feed.label,
                                           argument.port.location);
        builder_->step(pipe).source = source.node;
        builder_->step(pipe).destination = destination.node;
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
    if (symbol.target != nullptr && !symbol.target->HasPort(name, direction)) {
      const std::vector<std::string> declared =
          symbol.target->PortNames(direction);
      Report("flow.name.unknown-port",
             absl::StrCat(symbol.action, " has no ",
                          direction == syntax::PortDirection::kInput ? "input"
                                                                    : "output",
                          " port ", Quoted(name), " (declared: ",
                          declared.empty() ? "none"
                                           : absl::StrJoin(declared, ", "),
                          ")."),
             location);
      ref.kind = Ref::Kind::kUnknown;
      return ref;
    }
    if (builder_ == nullptr) return ref;
    // Memoised per `direction:name`: the flow may name one port of one call
    // from several places, and it is one stream however often it is written.
    const graph::StepId step = symbol.step;
    if (step == graph::kNone) return ref;
    const std::string key = absl::StrCat(
        syntax::PortDirectionName(direction), ":", name);
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
    // A sibling flow declared its ports here, so this one is knowable. An
    // action from a registry did not: the resolver has its name and nothing
    // else, and guessing would be claiming something it cannot see. The runtime
    // has the real schema and refines it there.
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

  Ref ResolvePipeline(const syntax::Pipeline& pipeline, Scope& scope) {
    Ref ref = ResolveSource(pipeline.source.get(), scope);
    // The pattern the values carry as the pipeline is walked, so `it` in a
    // stage knows what the stage before made of them.
    std::string previous_pattern = PatternOf(pipeline);
    if (!pipeline.stages.empty()) previous_pattern.clear();
    for (const syntax::StagePtr& stage : pipeline.stages) {
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
        // the name the author gave what the last value produced. The name is an
        // ordinary name in an ordinary scope -- so a typo in it is the same
        // diagnostic as any other unknown name -- bound to a ref the runtime
        // fills in per value rather than one it reads a stream for.
        // Its own role, because a fold's accumulator is not a stream: nothing
        // produces it and nothing may subscribe to it. See [Scope::Prepare].
        carry = BoundRef(body_, graph::kNone, "fold");
        Scope folding;
        folding.parent = &scope;
        if (!stage->carried.text.empty()) {
          Symbol carried;
          carried.kind = SymbolKind::kLoopVariable;
          carried.name = stage->carried.text;
          carried.location = stage->carried.location;
          carried.ref = carry;
          Define(folding, carried);
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
      previous_pattern =
          vocabulary::Canonical(stage->name) == "match" ? stage->text : "";
      CheckShapeStage(*stage, ref);
      CheckPatternStage(*stage);
      // `into failures` names a destination, so it resolves the way a pipe's
      // target does: as something this flow may write.
      graph::RefId failures = graph::kNone;
      if (stage->failures != nullptr) {
        const Ref target =
            ResolveDestination(stage->failures.get(), scope);
        failures = target.node;
      }
      const graph::RefId source = ref.node;
      absl::StrAppend(&ref.label, " | ", StageLabel(*stage));
      ref.kind = Ref::Kind::kDerived;
      ref.has_front = false;
      ref.writable = false;
      ref.tolerant = false;
      ref.shape = ShapeAfter(*stage, ref.shape);
      graph::Stage made = GraphStage(*stage, stream, expr, std::move(log),
                                     failures);
      made.carry = carry;
      ref.node = Derive(source, std::move(made), ref.label);
    }
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
      // `map Shape{..}` and `map it as Shape` are the two ways a pipeline says
      // what it is making. Anything else makes something the language cannot
      // name, which is honestly reported as nothing.
      const auto* typed = syntax::As<syntax::TypedValue>(stage.argument.get());
      if (typed == nullptr) return "";
      return known_.Dto(typed->type.name) != nullptr ? typed->type.name : "";
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
    if (vocabulary::Canonical(stage.name) != "match") return;
    const pattern::Compiled compiled = pattern::Compile(stage.text);
    if (compiled.ok()) return;
    Report("flow.form.bad-pattern", std::string(compiled.error),
           stage.location, Severity::kError, Family::kForm);
  }

  /// `| json` on a stream of a shape that holds bytes.
  ///
  /// JSON has nothing to carry a byte string in, so this is not a value that
  /// would render oddly -- it is one that cannot be rendered at all. `packb`
  /// can, which is what the message says.
  void CheckShapeStage(const syntax::Stage& stage, const Ref& ref) {
    if (ref.shape.empty()) return;
    if (vocabulary::Canonical(stage.name) != "json") return;
    const DtoPlan* shape = known_.Dto(ref.shape);
    if (shape == nullptr || !shape->binary) return;
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
    Ref ref;
    ref.kind = Ref::Kind::kDerived;
    // A zip has a front -- the tuples are produced here -- but a counted `skip`
    // on it would have to take tuples off a stream nothing else holds, and
    // there is nowhere upstream to apply the count. `| drop n` is the one that
    // works.
    ref.has_front = false;
    std::vector<std::string> labels;
    std::vector<graph::RefId> sources;
    for (const syntax::NodePtr& source : zip.sources) {
      const Ref one = ResolveSource(source.get(), scope);
      labels.push_back(one.label);
      sources.push_back(one.node);
    }
    ref.label = absl::StrCat(zip.name, "(", absl::StrJoin(labels, ", "), ")");
    if (builder_ == nullptr) return ref;
    graph::Ref made;
    made.kind = zip.name == "interleave" ? graph::RefKind::kMerge
                                        : graph::RefKind::kZip;
    made.label = ref.label;
    made.owner = body_;
    made.sources = std::move(sources);
    ref.node = builder_->AddRef(std::move(made));
    return ref;
  }

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
      for (StepPlan& one : made) pending_steps_->push_back(std::move(one));
    }
    if (builder_ == nullptr ||
        builder_->flow().steps.size() <= graph_before) {
      return Ref{Ref::Kind::kUnknown, "wait first of"};
    }
    const graph::Step& step = builder_->step(graph_before);
    ref.kind = Ref::Kind::kValue;
    ref.label = step.label;
    ref.node = step.winner;
    ref.has_front = true;
    return ref;
  }

  /// A pipeline source: a stream, a path over one, or a single value.
  Ref ResolveSource(const Node* expression, Scope& scope) {
    std::optional<Ref> stream = AsStream(expression, scope);
    if (stream.has_value()) return *std::move(stream);
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

  /// A name or a path rooted at one, as a stream; `nullopt` if it is a value.
  std::optional<Ref> AsStream(const Node* expression, Scope& scope) {
    if (expression == nullptr) return std::nullopt;
    switch (expression->kind) {
      case NodeKind::kPipelineValue:
        return ResolvePipeline(
            *syntax::As<syntax::PipelineValue>(expression)->pipeline, scope);
      case NodeKind::kZip:
        return ResolveZip(*syntax::As<syntax::Zip>(expression), scope);
      case NodeKind::kWait:
        return ResolveRaceValue(*syntax::As<syntax::Wait>(expression), scope);
      case NodeKind::kOutcome:
        return ResolveOutcome(
            syntax::As<syntax::Outcome>(expression)->subject.get(), scope);
      case NodeKind::kName: {
        const auto* name = syntax::As<syntax::Name>(expression);
        Symbol* found = Find(scope, name->name);
        if (found == nullptr) {
          Report("flow.name.unknown",
                 absl::StrCat("Unknown name ", Quoted(name->name), " (known: ",
                              Known(scope), ")."),
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
          Report("flow.name.not-a-stream",
                 absl::StrCat(Quoted(name->name),
                              " is a node map, not a stream."),
                 expression->location);
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
        if (found->kind == SymbolKind::kBarrier) ++found->status_reads;
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
            // what lets `image | chunk 65536` be written of a `let`.
            ref.kind = Ref::Kind::kValue;
            break;
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
        if (named.has_value()) return named;
        std::optional<Ref> inner = AsStream(attr->base.get(), scope);
        if (!inner.has_value()) return std::nullopt;
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
        if (!inner.has_value()) return std::nullopt;
        Constant(index->index.get());
        Ref ref = *std::move(inner);
        const graph::RefId source = ref.node;
        absl::StrAppend(&ref.label, "[", Unparse(index->index.get()), "]");
        ref.kind = Ref::Kind::kDerived;
        ref.has_front = false;
        ref.writable = false;
        ref.tolerant = false;
        ref.node = Derive(
            source,
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
    if (base == nullptr) return std::nullopt;
    const size_t index = Lookup(scope, base->name);
    if (index == kNoSymbol) return std::nullopt;
    Symbol& symbol = resolved_.symbols[index];
    if (symbol.kind == SymbolKind::kCall) {
      ++symbol.reads;
      if (attr->name == "status") {
        ++symbol.status_reads;
        Ref ref;
        ref.kind = Ref::Kind::kStatus;
        ref.label = absl::StrCat("status ", base->name);
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
      ref.label = absl::StrCat(base->name, ".id");
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
    if (symbol.kind == SymbolKind::kBarrier) {
      ++symbol.reads;
      ++symbol.status_reads;
      Ref ref;
      ref.kind = Ref::Kind::kDerived;
      ref.label = absl::StrCat(base->name, ".", attr->name);
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
    std::vector<std::string> path;
    const Node* cursor = expression;
    // A trailing field of the record belongs to the record, not to a port of
    // that name: `status x.code` is what x finished with.
    while (true) {
      const auto* attr = syntax::As<syntax::Attr>(cursor);
      if (attr == nullptr) break;
      if (!vocabulary::StatusFields().contains(attr->name)) break;
      if (!HasStatus(attr->base.get(), scope)) break;
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
                 absl::StrCat("Unknown name ", Quoted(name->name), " (known: ",
                              Known(scope), ")."),
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
    for (auto at = path.rbegin(); at != path.rend(); ++at) {
      const graph::RefId source = ref.node;
      absl::StrAppend(&ref.label, ".", *at);
      // A field of the record: the status is still the thing waited for, and
      // the field is read out of the value it gives.
      ref.node = Derive(source, AtStage(*at), ref.label);
    }
    return ref;
  }

  /// The graph ref that is the status of what `cursor` names.
  ///
  /// A call's and a node's are fresh per mention; a bound barrier's is the one
  /// it already waited for, because `s` and `wait ...` are the same moment.
  void GraphSubject(const Node* cursor, Scope& scope, Ref& out) {
    if (builder_ == nullptr) return;
    if (const auto* name = syntax::As<syntax::Name>(cursor); name != nullptr) {
      const size_t index = Lookup(scope, name->name);
      if (index == kNoSymbol) return;
      const Symbol& symbol = resolved_.symbols[index];
      if (symbol.kind == SymbolKind::kCall) {
        out.tolerant = symbol.tolerant;
        out.node = FreshStatusOfStep(symbol.step);
        return;
      }
      if (symbol.kind == SymbolKind::kBarrier) {
        out.tolerant = symbol.tolerant;
        out.node = symbol.ref;
        return;
      }
      out.node = StatusOfRef(symbol.ref, symbol.name);
      return;
    }
    const auto* attr = syntax::As<syntax::Attr>(cursor);
    if (attr == nullptr) return;
    const auto* base = syntax::As<syntax::Name>(attr->base.get());
    if (base == nullptr) return;
    const size_t index = Lookup(scope, base->name);
    if (index == kNoSymbol) return;
    if (resolved_.symbols[index].kind != SymbolKind::kCall) return;
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
      if (index == kNoSymbol) return false;
      switch (resolved_.symbols[index].kind) {
        case SymbolKind::kCall:
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
      if (base == nullptr) return false;
      const size_t index = Lookup(scope, base->name);
      if (index == kNoSymbol) return false;
      const Symbol& symbol = resolved_.symbols[index];
      if (symbol.kind != SymbolKind::kCall) return false;
      if (attr->name == "status") return true;
      if (symbol.target == nullptr) return true;
      return symbol.target->HasPort(attr->name,
                                    syntax::PortDirection::kOutput) ||
             symbol.target->HasPort(attr->name, syntax::PortDirection::kInput);
    }
    return false;
  }

  Ref ResolveDestination(const Node* expression, Scope& scope) {
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
                              Quoted(attr->name),
                              " is not a port to write."),
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
    if (expression == nullptr) return graph::kNone;
    if (builder_ == nullptr) {
      WalkExpression(expression, scope, allow_it, nullptr);
      return graph::kNone;
    }
    graph::Expr expr;
    expr.node = expression;
    WalkExpression(expression, scope, allow_it, &expr);
    return builder_->AddExpr(std::move(expr));
  }

  /// Note that `node` reads `ref`, for the expression being built.
  void Remember(graph::Expr* absl_nullable out, const Node* node,
                graph::RefId ref) {
    if (out == nullptr || ref == graph::kNone) return;
    out->bound.emplace_back(node, ref);
    if (std::find(out->refs.begin(), out->refs.end(), ref) == out->refs.end()) {
      out->refs.push_back(ref);
    }
  }

  void WalkExpression(const Node* expression, Scope& scope, bool allow_it,
                      graph::Expr* absl_nullable out) {
    if (expression == nullptr) return;
    switch (expression->kind) {
      case NodeKind::kIt:
        if (!allow_it) {
          Report("flow.name.it-outside-stage",
                 "'it' names the value a 'where' or 'map' stage is looking at, "
                 "and there is none here.",
                 expression->location);
        }
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
      case NodeKind::kBuiltin:
        for (const syntax::NodePtr& argument :
             syntax::As<syntax::Builtin>(expression)->args) {
          WalkExpression(argument.get(), scope, allow_it, out);
        }
        return;
      case NodeKind::kTypedValue: {
        const auto* typed = syntax::As<syntax::TypedValue>(expression);
        // The type is checked as far as it can be here -- a built-in name is
        // either known or misspelt -- and a tag is left to the runtime, which
        // is the only place that knows what has been registered.
        if (typed->type.name.find('.') == std::string::npos &&
            typed->type.name.find('/') == std::string::npos &&
            !typed->type.quoted) {
          PortType(typed->type);
        }
        // A shape declared in this file is the one type whose fields are known
        // here, so it is the one whose literal can be checked before it runs.
        if (const DtoPlan* shape = known_.Dto(typed->type.name);
            shape != nullptr) {
          CheckShapeLiteral(*shape, typed->value.get());
        }
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
      case NodeKind::kPipelineValue:
        Remember(out, expression,
                 ResolvePipeline(
                     *syntax::As<syntax::PipelineValue>(expression)->pipeline,
                     scope)
                     .node);
        return;
      case NodeKind::kOutcome:
        Remember(out, expression,
                 ResolveOutcome(
                     syntax::As<syntax::Outcome>(expression)->subject.get(),
                     scope)
                     .node);
        return;
      case NodeKind::kName: {
        const std::optional<Ref> stream = AsStream(expression, scope);
        if (stream.has_value()) Remember(out, expression, stream->node);
        return;
      }
      case NodeKind::kAttr: {
        const auto* attr = syntax::As<syntax::Attr>(expression);
        if (const std::optional<Ref> named = AttrStream(attr, scope);
            named.has_value()) {
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
        Report("flow.syntax.unexpected",
               absl::StrCat("Cannot use a ",
                            syntax::NodeKindName(expression->kind),
                            " as a value."),
               expression->location, Severity::kError, Family::kSyntax);
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

  std::optional<Fields> FieldsOfPattern(const std::string& text) const {
    if (text.empty()) return std::nullopt;
    const pattern::Compiled compiled = pattern::Compile(text);
    // A pattern that does not read is reported where it is written; nothing is
    // known about what it names, so nothing is checked here. Nor is a
    // positional one, which names no fields at all.
    if (!compiled.ok() || !compiled.pattern.AllNamed()) return std::nullopt;
    Fields found;
    found.subject = "the pattern";
    for (const pattern::Hole& hole : compiled.pattern.holes) {
      found.names.push_back(hole.name);
    }
    return found;
  }

  std::optional<Fields> FieldsOfSymbol(const Symbol& symbol) const {
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
    if (port == nullptr) return std::nullopt;
    const DtoPlan* shape = known_.Dto(port->type);
    if (shape == nullptr) return std::nullopt;
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
    std::optional<Fields> known;
    if (syntax::As<syntax::It>(attr->base.get()) != nullptr) {
      known = FieldsOfPattern(it_pattern_);
    } else if (const auto* base = syntax::As<syntax::Name>(attr->base.get());
               base != nullptr) {
      const size_t index = Lookup(scope, base->name);
      if (index == kNoSymbol) return;
      known = FieldsOfSymbol(resolved_.symbols[index]);
    }
    if (!known.has_value() || known->names.empty()) return;
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
    if (object == nullptr) return;
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
               absl::StrCat(Quoted(shape.name), " has no field ", Quoted(key),
                            " (it has: ",
                            absl::StrJoin(shape.FieldNames(), ", "), ")."),
               held == nullptr ? value->location : held->location,
               Severity::kError, Family::kForm);
        continue;
      }
      // A constant is the only value whose kind is known here. Anything read at
      // run time is checked when it arrives.
      const std::optional<syntax::Constant> constant =
          syntax::ConstantValue(held.get());
      if (!constant.has_value() || !field->dto_name.empty()) continue;
      if (ConstantFits(*constant, field->type)) continue;
      Report("flow.form.field-type-mismatch",
             absl::StrCat(Quoted(key), " of ", Quoted(shape.name), " holds ",
                          field->declared, ", and this is a ",
                          syntax::ConstantKindName(constant->kind), "."),
             held->location, Severity::kError, Family::kForm);
    }
    if (spread) return;
    std::vector<std::string> missing;
    for (const FieldPlan& field : shape.fields) {
      if (!field.required || field.has_default) continue;
      if (given.contains(field.name)) continue;
      missing.push_back(field.name);
    }
    if (missing.empty()) return;
    Report("flow.form.missing-field",
           absl::StrCat(Quoted(shape.name), " requires ",
                        absl::StrJoin(missing, ", "), ", which this leaves out."),
           value->location, Severity::kError, Family::kForm);
  }

  void Constant(const Node* expression) {
    if (expression == nullptr) return;
    if (syntax::ConstantValue(expression).has_value()) return;
    if (expression->kind == NodeKind::kError) return;
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
  /// The graph steps the last [ResolveAfter] found, for the one statement that
  /// makes several steps out of one `after`.
  std::vector<graph::StepId> after_waits_;
  /// The plan steps the statement being resolved is appending to; see
  /// [ResolveStatements]. Null outside one.
  std::vector<StepPlan>* pending_steps_ = nullptr;
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
    if (port.name == port_name && port.direction == direction) return &port;
  }
  return nullptr;
}

std::vector<std::string> FlowPlan::PortNames(
    syntax::PortDirection direction) const {
  std::vector<std::string> names;
  for (const PortPlan& port : ports) {
    if (port.direction == direction) names.push_back(port.name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

const FlowPlan* absl_nullable Program::Flow(std::string_view name) const {
  // An empty name matches nothing, so the entry flow cannot be reached here --
  // which is what makes it unaddressable from a `run` or a `call` rather than
  // merely undocumented.
  if (name.empty()) return nullptr;
  for (const FlowPlan& flow : flows) {
    if (!flow.entry && flow.name == name) return &flow;
  }
  return nullptr;
}

const FlowPlan* absl_nullable Program::Entry() const {
  for (const FlowPlan& flow : flows) {
    if (flow.entry) return &flow;
  }
  return nullptr;
}

const DtoPlan* absl_nullable Program::Dto(std::string_view name) const {
  for (const DtoPlan& dto : dtos) {
    if (dto.name == name) return &dto;
  }
  return nullptr;
}

const FieldPlan* absl_nullable DtoPlan::Field(
    std::string_view field_name) const {
  for (const FieldPlan& field : fields) {
    if (field.name == field_name) return &field;
  }
  return nullptr;
}

std::vector<std::string> DtoPlan::FieldNames() const {
  std::vector<std::string> names;
  names.reserve(fields.size());
  for (const FieldPlan& field : fields) names.push_back(field.name);
  return names;
}

bool ResolveResult::HasErrors() const { return FirstError() != nullptr; }

const Diagnostic* absl_nullable ResolveResult::FirstError() const {
  for (const Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == Severity::kError) return &diagnostic;
  }
  return nullptr;
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
                      bool build_graph) {
  ResolveResult result;
  result.diagnostics = parsed.diagnostics;
  const LineIndex lines(source);

  // The shapes first, and all of them: a port may be typed with one, and a
  // shape may name another, so neither declaration order nor the flows can be
  // waited for.
  result.program.dtos =
      DtoResolver(lines, absl::MakeConstSpan(parsed.dtos), result.diagnostics)
          .Run();

  // Then two passes over the flows, for the reason a program is a set of flows
  // rather than a sequence: every flow declares its ports before any body is
  // read, so a call to a sibling is checked whichever order the two were
  // written in.
  absl::flat_hash_set<std::string> seen;
  std::vector<const syntax::FlowDeclaration*> declared;
  for (const syntax::FlowDeclarationPtr& declaration : parsed.flows) {
    if (!seen.insert(declaration->name.text).second) {
      Diagnostic diagnostic;
      diagnostic.code = "flow.form.duplicate-flow";
      diagnostic.severity = Severity::kError;
      diagnostic.family = Family::kForm;
      diagnostic.message =
          declaration->entry
              ? std::string("A file may declare one entry flow, and this one "
                            "declares two. A flow that is meant to be called "
                            "needs a name.")
              : absl::StrCat("Flow ", Quoted(declaration->name.text),
                             " is declared twice.");
      diagnostic.range =
          lines.Between(declaration->location.start, declaration->location.end);
      diagnostic.flow = declaration->name.text;
      result.diagnostics.push_back(std::move(diagnostic));
      continue;
    }
    declared.push_back(declaration.get());
  }

  result.flows.resize(declared.size());
  // Reserved rather than grown: a call's symbol points at the plan of the flow
  // it names, and a reallocation would leave that pointer behind.
  result.program.flows.reserve(declared.size());
  for (size_t index = 0; index < declared.size(); ++index) {
    FlowResolver(lines, *declared[index], result.program, result.flows[index],
                 result.diagnostics)
        .Declare();
    result.program.flows.push_back(result.flows[index].plan);
  }
  // Every flow's ports are in place, so a body may now be read against all of
  // them -- including its own, which is how a recursive call is checked.
  for (size_t index = 0; index < declared.size(); ++index) {
    graph::GraphBuilder builder(result.flows[index].graph);
    FlowResolver(lines, *declared[index], result.program, result.flows[index],
                 result.diagnostics, build_graph ? &builder : nullptr)
        .ResolveBody();
    result.program.flows[index] = result.flows[index].plan;
  }
  return result;
}

}  // namespace a11::flow
