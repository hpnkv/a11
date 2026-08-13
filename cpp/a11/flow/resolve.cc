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

/// One stage as it was written: `truncate 200`, `where it.ok`.
std::string StageLabel(const syntax::Stage& stage);

/// An expression as a person would read it back, for a label or a message.
///
/// Not a formatter: this is the shortest faithful spelling of one expression, the
/// way `a11.flow.plan.unparse` writes it, so a step's label says what it does.
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
std::string StageLabel(const syntax::Stage& stage) {
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
    case vocabulary::StageArgument::kNone:
      return stage.name;
  }
  return stage.name;
}

/// What a reference resolved to.
///
/// Enough for the questions the resolver has to answer -- may this be read, may it
/// be written, does it have a status, is it the front of a stream a counted `skip`
/// could take values off -- and no more. The runtime's ref, with its buffers and
/// its readers, is a different object for a different job.
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
  /// Whether a counted `skip` may take values off the front of it: only a stream
  /// that is genuinely produced somewhere has a front.
  bool has_front = false;
  /// The graph ref this answer corresponds to, when a graph is being built.
  ///
  /// One extra field rather than a second resolver: ~25 sites return one of
  /// these, and what the runtime needs from them is the *identity* of the stream
  /// they named. [graph::kNone] on the editor path, and wherever the reference
  /// did not resolve.
  graph::RefId node = graph::kNone;
  /// For an outcome: whether a bad one is the flow's business or the subject's.
  /// True where the subject is a `try` call, or a barrier on one.
  bool tolerant = false;
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
};

/// The two passes over one flow declaration.
class FlowResolver {
 public:
  FlowResolver(const LineIndex& lines, const syntax::FlowDeclaration& declaration,
               const Program& known, ResolvedFlow& resolved,
               std::vector<Diagnostic>& diagnostics,
               graph::GraphBuilder* absl_nullable builder = nullptr)
      : lines_(lines), declaration_(declaration), known_(known),
        resolved_(resolved), diagnostics_(diagnostics), builder_(builder) {}

  /// The flow's ports and headers, without resolving its body.
  ///
  /// The first of two passes, for the reason `a11/flow/plan.py` gives: a flow's
  /// ports are what a *sibling* calling it needs, and which flow is written first
  /// is the author's convenience rather than a dependency order.
  void Declare() {
    FlowPlan& plan = resolved_.plan;
    plan.name = declaration_.name.text;
    plan.description = declaration_.description;
    plan.location = declaration_.location;
    resolved_.declaration = &declaration_;

    absl::flat_hash_set<std::string> seen;
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
      // An `in` port is read and an `out` port is written. Reading an `out` port
      // back is what a node of the flow's own is for, and the message for it says
      // so rather than leaving the author guessing.
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
  /// A call's is memoised on its step; a node's or a port's is fresh per mention,
  /// exactly as `a11.flow.plan` makes them, because the counts follow from it.
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

  /// The status of a call, fresh per mention.
  ///
  /// `wait x` and `status x` each make their own, exactly as `a11.flow.plan`
  /// does -- only `x.status` is memoised -- and the reader counts follow from it.
  /// A status is one value however many refs name it, so this costs nothing but
  /// keeps the two implementations countable against each other.
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
                          graph::ExprId expr) {
    graph::Stage made;
    made.name = stage.name;
    made.takes = stage.takes;
    switch (stage.takes) {
      case vocabulary::StageArgument::kNumber:
        made.count = static_cast<long long>(stage.number);
        break;
      case vocabulary::StageArgument::kString:
      case vocabulary::StageArgument::kOptionalString:
        made.text = stage.text;
        break;
      case vocabulary::StageArgument::kExpression:
        made.expr = expr;
        break;
      case vocabulary::StageArgument::kStream:
        made.stream = stream;
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

  /// What a declared type gives a port, and a diagnostic where it gives nothing.
  std::string PortType(const syntax::TypeExpression& type) {
    if (type.quoted) {
      CheckParameters(type, {0});
      return type.name;
    }
    const std::string declared = vocabulary::Canonical(type.name);
    if (vocabulary::TypeNames().contains(declared)) {
      const absl::Span<const int> allowed =
          vocabulary::TypeParameters(declared);
      CheckParameters(type, std::vector<int>(allowed.begin(), allowed.end()));
      return declared;
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
    std::vector<std::string> known;
    for (const std::string_view name : vocabulary::TypeNames()) {
      known.emplace_back(name);
    }
    std::sort(known.begin(), known.end());
    Report("flow.form.unknown-type",
           absl::StrCat("Unknown port type ", Quoted(type.name), " (known: ",
                        absl::StrJoin(known, ", "),
                        ", a serialisation tag like 'a11.sdk.AudioBuffer', or "
                        "a quoted mimetype)."),
           type.location, Severity::kError, Family::kForm);
    return type.name;
  }

  void CheckParameters(const syntax::TypeExpression& type,
                       const std::vector<int>& allowed) {
    for (const syntax::TypeExpression& parameter : type.parameters) {
      PortType(parameter);
    }
    const int given = static_cast<int>(type.parameters.size());
    if (std::find(allowed.begin(), allowed.end(), given) != allowed.end()) {
      return;
    }
    std::vector<std::string> counts;
    for (const int count : allowed) counts.push_back(absl::StrCat(count));
    Report("flow.form.unknown-type",
           absl::StrCat(Quoted(type.name), " takes ",
                        absl::StrJoin(counts, " or "),
                        " type parameter(s), but ", type.ToString(), " gives ",
                        given, "."),
           type.location, Severity::kError, Family::kForm);
  }

  // -- statements ------------------------------------------------------------

  /// Read a block of statements, into `body` when a graph is being built.
  std::vector<StepPlan> ResolveStatements(
      const std::vector<syntax::NodePtr>& statements, Scope& scope,
      graph::BodyId body = graph::kNone) {
    const graph::BodyId outer = body_;
    if (body != graph::kNone) body_ = body;
    std::vector<StepPlan> steps;
    for (const syntax::NodePtr& statement : statements) {
      ResolveStatement(statement.get(), scope, steps);
    }
    body_ = outer;
    return steps;
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
      case NodeKind::kCallStatement: {
        const auto* held = syntax::As<syntax::CallStatement>(statement);
        ResolveCall(*held->call, scope, Label(held->call->action), steps);
        return;
      }
      case NodeKind::kPipe: {
        const auto* pipe = syntax::As<syntax::Pipe>(statement);
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
          }
          steps.push_back(std::move(step));
        }
        return;
      }
      case NodeKind::kSkip: {
        const auto* skip = syntax::As<syntax::Skip>(statement);
        const Ref source = ResolvePipeline(*skip->pipeline, scope);
        if (skip->count.has_value() && !source.has_front) {
          Report("flow.sequence.skip-count-target",
                 absl::StrCat("'skip ", *skip->count,
                              "' takes a port or a node, and ", source.label,
                              " is not one. Use '| drop ", *skip->count,
                              "' to drop values from a pipeline instead."),
                 skip->location, Severity::kError, Family::kSequence);
        } else if (skip->count.has_value() && builder_ != nullptr &&
                   source.node != graph::kNone) {
          // The count is the node's, not this statement's: it accumulates on the
          // one ref, and every reader of it inherits what was taken off.
          builder_->ref(source.node).skip += *skip->count;
        }
        StepPlan step;
        step.kind = "skip";
        step.label = source.label;
        step.source = source.label;
        step.location = skip->location;
        graph::StepId made = graph::kNone;
        if (builder_ != nullptr) {
          made = NewStep(graph::StepKind::kSkip,
                         skip->count.has_value()
                             ? absl::StrCat("skip ", *skip->count, " of ",
                                            source.label)
                             : absl::StrCat("skip ", source.label),
                         skip->location);
          builder_->step(made).source = source.node;
          builder_->step(made).count = skip->count;
        }
        step.after = ResolveAfter(skip->after, scope, made);
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kWait: {
        const auto* wait = syntax::As<syntax::Wait>(statement);
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
        Symbol variable;
        variable.kind = SymbolKind::kLoopVariable;
        variable.name = loop->variable.text;
        variable.location = loop->variable.location;
        variable.ref = item;
        Define(inner, variable);
        DefineIndex(inner, loop->location, index);
        step.bodies.push_back(
            ResolveStatements(loop->body, inner, inner_body));
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
        RepeatState state;
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
        repeats_.push_back(state);
        step.bodies.push_back(
            ResolveStatements(repeat->body, inner, inner_body));
        repeats_.pop_back();
        steps.push_back(std::move(step));
        return;
      }
      case NodeKind::kCarry: {
        const auto* carry = syntax::As<syntax::Carry>(statement);
        if (repeats_.empty()) {
          Report("flow.barrier.carry-outside-repeat",
                 "'<-' carries a value into the next pass of a 'repeat', and "
                 "there is no repeat here.",
                 carry->location, Severity::kError, Family::kBarrier);
          ResolvePipeline(*carry->pipeline, scope);
          return;
        }
        RepeatState& state = repeats_.back();
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
        if (repeats_.empty()) {
          Report("flow.barrier.until-outside-repeat",
                 "'until'/'while' ends a 'repeat', and there is no repeat "
                 "here.",
                 until->location, Severity::kError, Family::kBarrier);
        } else if (repeats_.back().stopped) {
          Report("flow.barrier.duplicate-until",
                 absl::StrCat(repeats_.back().label,
                              " already has a stop condition."),
                 until->location, Severity::kError, Family::kBarrier);
        } else {
          repeats_.back().stopped = true;
        }
        const graph::StepId owner =
            repeats_.empty() ? graph::kNone : repeats_.back().step;
        const graph::ExprId condition =
            ResolveExpression(until->condition.get(), scope, false);
        if (builder_ != nullptr && owner != graph::kNone) {
          graph::Step& one = builder_->step(owner);
          one.condition = condition;
          one.stop_when = until->stop_when;
          // Each stream the condition reads is captured inside the pass, because
          // the question is asked once the pass is over and the streams are gone
          // by then.
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
            ResolveStatements(branch->then_body, then_scope, then_body));
        Scope else_scope;
        else_scope.parent = &scope;
        step.bodies.push_back(
            ResolveStatements(branch->else_body, else_scope, else_body));
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
    // A bound `wait`/`drain`: the barrier is the step, and the name reads as the
    // outcome it waited for.
    const size_t before = steps.size();
    const size_t graph_before =
        builder_ == nullptr ? 0 : builder_->flow().steps.size();
    ResolveStatement(value, scope, steps);
    Symbol barrier;
    barrier.kind = SymbolKind::kBarrier;
    barrier.name = bind->name.text;
    barrier.location = bind->name.location;
    barrier.readable = true;
    if (builder_ != nullptr && builder_->flow().steps.size() > graph_before) {
      // The statement's own step is the first one it made; anything after it is a
      // barrier `after` grew, which belongs to nobody's name.
      barrier.step = graph_before;
      barrier.ref = builder_->step(graph_before).outcome;
      barrier.tolerant = builder_->step(graph_before).tolerant;
      builder_->step(graph_before).label = bind->name.text;
    }
    Define(scope, barrier);
    if (steps.size() > before) steps[before].label = bind->name.text;
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
  /// A name may be a step -- a call, a bound `wait`/`drain` -- or a port or node,
  /// in which case the statement waits for that stream to be finished. The second
  /// reading is the one an author reaches for without thinking.
  ///
  /// With `step` it also hangs the graph's own answer on that step: a name that is
  /// a call or a barrier waits for its step, and a port or a node grows a `wait`
  /// step of its own here -- the same step a written-out `x = wait port` would
  /// have made, so the two spellings cannot drift apart.
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
    // The verb it was written with, not "a call": a described step says which of
    // `run` and `call` it is, the way a `wait`/`drain` step says which of those it
    // was, because that is the distinction a reader is checking for.
    step.kind = call.mode;
    step.label = std::string(label);
    step.action = call.action;
    step.mode = call.mode;
    step.node_map = map;
    step.timeout = call.modifiers->timeout;
    step.tolerant = call.tolerant;
    step.tee = call.modifiers->tee;
    step.location = call.location;
    // The headers and the id first: they are read where the call is written, and
    // the step carries what they resolved to.
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
    // Memoised per `direction:name`: the flow may name one port of one call from
    // several places, and it is one stream however often it is written.
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
    ref.node = builder_->AddRef(std::move(port));
    builder_->step(step).ports.emplace(key, ref.node);
    return ref;
  }

  // -- pipelines and references ----------------------------------------------

  Ref ResolvePipeline(const syntax::Pipeline& pipeline, Scope& scope) {
    Ref ref = ResolveSource(pipeline.source.get(), scope);
    for (const syntax::StagePtr& stage : pipeline.stages) {
      graph::RefId stream = graph::kNone;
      graph::ExprId expr = graph::kNone;
      if (stage->takes == vocabulary::StageArgument::kStream) {
        stream = ResolveSource(stage->argument.get(), scope).node;
      } else if (stage->argument != nullptr) {
        expr = ResolveExpression(stage->argument.get(), scope, true);
      }
      const graph::RefId source = ref.node;
      absl::StrAppend(&ref.label, " | ", StageLabel(*stage));
      ref.kind = Ref::Kind::kDerived;
      ref.has_front = false;
      ref.writable = false;
      ref.tolerant = false;
      ref.node = Derive(source, GraphStage(*stage, stream, expr), ref.label);
    }
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
          case SymbolKind::kOutputPort:
            ref.kind = Ref::Kind::kPort;
            ref.has_front = true;
            ref.writable = found->writable;
            break;
          case SymbolKind::kNode:
            ref.kind = Ref::Kind::kNode;
            ref.has_front = true;
            ref.writable = true;
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
      // An output unless the flow it names declares it as an input, which is what
      // lets `x.in-port` be written and `x.out-port` be read.
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
  /// The longest prefix of the reference that names something with a status wins,
  /// and anything left over reads into the record: `status x.ok` is the call's
  /// status asked whether it is ok, and `status x.out` is that port's.
  Ref ResolveOutcome(const Node* expression, Scope& scope) {
    std::vector<std::string> path;
    const Node* cursor = expression;
    // A trailing field of the record belongs to the record, not to a port of that
    // name: `status x.code` is what x finished with.
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
    // Reading a status is reading the subject: count it, so a `try` whose status
    // nothing reads can be told from one that is waited on.
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
      // A field of the record: the status is still the thing waited for, and the
      // field is read out of the value it gives.
      ref.node = Derive(source, AtStage(*at), ref.label);
    }
    return ref;
  }

  /// The graph ref that is the status of what `cursor` names.
  ///
  /// A call's and a node's are fresh per mention; a bound barrier's is the one it
  /// already waited for, because `s` and `wait ...` are the same moment.
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
      case NodeKind::kBuiltin:
        for (const syntax::NodePtr& argument :
             syntax::As<syntax::Builtin>(expression)->args) {
          WalkExpression(argument.get(), scope, allow_it, out);
        }
        return;
      case NodeKind::kTypedValue: {
        const auto* typed = syntax::As<syntax::TypedValue>(expression);
        // The type is checked as far as it can be here -- a built-in name is
        // either known or misspelt -- and a tag is left to the runtime, which is
        // the only place that knows what has been registered.
        if (typed->type.name.find('.') == std::string::npos &&
            typed->type.name.find('/') == std::string::npos &&
            !typed->type.quoted) {
          PortType(typed->type);
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

  void Constant(const Node* expression) {
    if (expression == nullptr) return;
    if (syntax::ConstantValue(expression).has_value()) return;
    if (expression->kind == NodeKind::kError) return;
    Report("flow.syntax.constant-required",
           "Expected a constant value here (a literal, list or object).",
           expression->location, Severity::kError, Family::kSyntax);
  }

  /// What one `repeat` knows about itself while its body is being read.
  struct RepeatState {
    std::string label;
    /// The graph step it is, so `<-` and `until` can fill it in.
    graph::StepId step = graph::kNone;
    std::string carries;
    bool has_carry_name = false;
    bool carried = false;
    bool stopped = false;
  };

  const LineIndex& lines_;
  const syntax::FlowDeclaration& declaration_;
  const Program& known_;
  ResolvedFlow& resolved_;
  std::vector<Diagnostic>& diagnostics_;
  /// Null on the editor path: no graph, and none of the work of building one.
  graph::GraphBuilder* absl_nullable builder_ = nullptr;
  /// The body statements are being read into, which is what a ref made here owns.
  graph::BodyId body_ = graph::kNone;
  /// The graph steps the last [ResolveAfter] found, for the one statement that
  /// makes several steps out of one `after`.
  std::vector<graph::StepId> after_waits_;
  absl::flat_hash_map<std::string, int> labels_;
  std::string node_map_;
  std::vector<RepeatState> repeats_;
};

}  // namespace

const PortPlan* absl_nullable FlowPlan::Port(
    std::string_view name, syntax::PortDirection direction) const {
  for (const PortPlan& port : ports) {
    if (port.name == name && port.direction == direction) return &port;
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
  for (const FlowPlan& flow : flows) {
    if (flow.name == name) return &flow;
  }
  return nullptr;
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
  }
  return "header";
}

ResolveResult Resolve(std::string_view source, const ParseResult& parsed,
                      bool build_graph) {
  ResolveResult result;
  result.diagnostics = parsed.diagnostics;
  const LineIndex lines(source);

  // Two passes over the file, for the reason a program is a set of flows rather
  // than a sequence: every flow declares its ports before any body is read, so a
  // call to a sibling is checked whichever order the two were written in.
  absl::flat_hash_set<std::string> seen;
  std::vector<const syntax::FlowDeclaration*> declared;
  for (const syntax::FlowDeclarationPtr& declaration : parsed.flows) {
    if (!seen.insert(declaration->name.text).second) {
      Diagnostic diagnostic;
      diagnostic.code = "flow.form.duplicate-flow";
      diagnostic.severity = Severity::kError;
      diagnostic.family = Family::kForm;
      diagnostic.message = absl::StrCat("Flow ", Quoted(declaration->name.text),
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
  // Reserved rather than grown: a call's symbol points at the plan of the flow it
  // names, and a reallocation would leave that pointer behind.
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
