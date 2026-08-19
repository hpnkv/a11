// Copyright 2026 The A11 Authors.

#include "a11/flow/inspect.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/strings/str_cat.h>

#include "a11/flow/diagnostic.h"
#include "a11/flow/plan.h"
#include "a11/flow/resolve.h"
#include "a11/flow/syntax.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow {
namespace {

using syntax::Node;
using syntax::NodeKind;

std::string Quoted(std::string_view text) {
  return absl::StrCat("'", text, "'");
}

/// A stage as a message names it: `'| drop 3'`.
std::string StageName(const syntax::Stage& stage) {
  if (stage.takes == vocabulary::StageArgument::kNumber) {
    return absl::StrCat("'| ", stage.name, " ",
                        static_cast<long long>(stage.number), "'");
  }
  return absl::StrCat("'| ", stage.name, "'");
}

/// Whether a stage reads the whole stream and leaves exactly one value.
bool Reduces(const syntax::Stage& stage) {
  return vocabulary::ReducingStages().contains(stage.name);
}

/// Whether a stage's answer depends on *which* values are in the stream, rather
/// than on what each one holds. These are the stages a reduced stream breaks.
bool IsPositional(const syntax::Stage& stage) {
  return vocabulary::PositionalStages().contains(stage.name);
}

/// The stages that reshape every value the same way, so twice is once.
bool IsIdempotent(std::string_view stage) {
  return stage == "collect" || stage == "distinct" || stage == "text" ||
         stage == "json" || stage == "packb";
}

class Inspector {
 public:
  Inspector(const LineIndex& lines, std::vector<Diagnostic>& found)
      : lines_(lines), found_(found) {}

  void Run(const ParseResult& parsed, const ResolveResult& resolved) {
    for (const ResolvedFlow& flow : resolved.flows) {
      if (flow.declaration == nullptr) continue;
      flow_ = flow.plan.name;
      Unused(flow);
      Statements(flow.declaration->body);
      Barriers(flow.plan.steps);
    }
    flow_.clear();
    UnusedShapes(parsed, resolved);
  }

 private:
  void Report(std::string_view code, std::string message,
              const syntax::Location& location, Severity severity,
              Family family) {
    Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = severity;
    diagnostic.family = family;
    diagnostic.message = std::move(message);
    diagnostic.range = lines_.Between(location.start, location.end);
    diagnostic.flow = flow_;
    found_.push_back(std::move(diagnostic));
  }

  /// Shapes nothing in the file names.
  ///
  /// A shape is used when a port is typed with it, another shape has a field of
  /// it, or a value is made one. Unlike a port or a name, it is not scoped to a
  /// flow -- so this asks the whole file at once, once, rather than once per
  /// flow, which would report every shape as unused by every flow that did not
  /// happen to mention it.
  ///
  /// A file that declares only shapes is a file of types, which is a reasonable
  /// thing to write and is what a JSONSchema is turned into; nothing there is
  /// unused, so nothing is said about it.
  void UnusedShapes(const ParseResult& parsed, const ResolveResult& resolved) {
    if (resolved.program.dtos.empty() || resolved.flows.empty()) return;
    absl::flat_hash_set<std::string> named;
    for (const DtoPlan& dto : resolved.program.dtos) {
      for (const FieldPlan& field : dto.fields) {
        if (!field.dto_name.empty()) named.insert(field.dto_name);
        if (!field.element_dto_name.empty()) {
          named.insert(field.element_dto_name);
        }
      }
    }
    for (const ResolvedFlow& flow : resolved.flows) {
      for (const PortPlan& port : flow.plan.ports) named.insert(port.type);
      if (flow.declaration != nullptr) NamedTypes(flow.declaration->body, named);
    }
    for (const syntax::DtoDeclarationPtr& declaration : parsed.dtos) {
      if (named.contains(declaration->name.text)) continue;
      if (resolved.program.Dto(declaration->name.text) == nullptr) continue;
      Report("flow.unused.struct",
             absl::StrCat("Nothing names the shape ",
                          Quoted(declaration->name.text),
                          ": no port carries it and no value is made one."),
             declaration->name.location, Severity::kWeakWarning,
             Family::kUnused);
    }
  }

  /// Every type a body writes down, however deep: `Shape{..}` and `x as Shape`.
  void NamedTypes(const std::vector<syntax::NodePtr>& body,
                  absl::flat_hash_set<std::string>& named) {
    for (const syntax::NodePtr& node : body) NamedTypes(node.get(), named);
  }

  void NamedTypes(const syntax::Node* absl_nullable node,
                  absl::flat_hash_set<std::string>& named) {
    if (node == nullptr) return;
    if (const auto* typed = syntax::As<syntax::TypedValue>(node);
        typed != nullptr) {
      named.insert(typed->type.name);
    }
    // Every child, whatever the node is: a cast can be anywhere an expression
    // can, and enumerating the places would be a list to keep in step with the
    // grammar for no gain.
    syntax::VisitChildren(
        *node, [&](const syntax::Node& child) { NamedTypes(&child, named); });
  }

  // --- what nothing uses ----------------------------------------------------

  void Unused(const ResolvedFlow& flow) {
    for (const Symbol& symbol : flow.symbols) {
      if (symbol.implicit || symbol.name.empty()) continue;
      switch (symbol.kind) {
        case SymbolKind::kHeader:
          if (symbol.reads == 0) {
            Report("flow.unused.header",
                   absl::StrCat("Nothing uses the header alias ",
                                Quoted(symbol.name),
                                ". A step is sent every 'x-a11-' header "
                                "anyway; declare one to *read* its value."),
                   symbol.location, Severity::kWeakWarning, Family::kUnused);
          }
          break;
        case SymbolKind::kOutputPort:
          if (symbol.writes == 0) {
            Report("flow.unused.output-port",
                   absl::StrCat("Nothing in this flow writes ",
                                Quoted(symbol.name),
                                ", so a caller reading it gets nothing."),
                   symbol.location, Severity::kWarning, Family::kUnused);
          }
          break;
        case SymbolKind::kNodeMap:
          if (symbol.reads == 0) {
            Report("flow.unused.node-map",
                   absl::StrCat("Nothing is placed in the node map ",
                                Quoted(symbol.name),
                                ". Give it a block, or name it with 'via'."),
                   symbol.location, Severity::kWeakWarning, Family::kUnused);
          }
          break;
        case SymbolKind::kValue:
          if (symbol.reads == 0) {
            // A `let` is lazy: nothing is read until the name is, so one nobody
            // reads is not merely a dead name -- the stream behind it is never
            // touched either, and whatever produces it may be left waiting.
            Report("flow.unused.value",
                   absl::StrCat("Nothing reads ", Quoted(symbol.name),
                                ", so the stream behind it is never read. Use "
                                "it, or 'skip' the stream instead."),
                   symbol.location, Severity::kWarning, Family::kUnused);
          }
          break;
        case SymbolKind::kLoopVariable:
          if (symbol.reads == 0) {
            Report("flow.unused.loop-variable",
                   absl::StrCat("No pass of this loop reads ",
                                Quoted(symbol.name), "."),
                   symbol.location, Severity::kWeakWarning, Family::kUnused);
          }
          break;
        case SymbolKind::kBarrier:
          if (symbol.reads == 0) {
            Report("flow.unused.barrier-name",
                   absl::StrCat("Nothing reads ", Quoted(symbol.name),
                                ". The flow still waits here, so the name is "
                                "the part doing nothing."),
                   symbol.location, Severity::kWeakWarning, Family::kUnused);
          }
          break;
        case SymbolKind::kCall:
          if (symbol.tolerant && symbol.status_reads == 0) {
            Report("flow.unused.try-status",
                   absl::StrCat("'try' lets ", symbol.action,
                                " fail without ending the flow, and nothing "
                                "here reads its status: a failure leaves the "
                                "ports it feeds silently empty. Read it with "
                                "'wait ", symbol.name, "' or 'status ",
                                symbol.name, "'."),
                   symbol.location, Severity::kWeakWarning, Family::kUnused);
          }
          break;
        default:
          break;
      }
    }
  }

  // --- sequences that cannot do what they look like -------------------------

  void Statements(const std::vector<syntax::NodePtr>& statements) {
    for (const syntax::NodePtr& statement : statements) {
      Statement(statement.get());
    }
  }

  void Statement(const Node* statement) {
    if (statement == nullptr) return;
    switch (statement->kind) {
      case NodeKind::kPipe: {
        const auto* pipe = syntax::As<syntax::Pipe>(statement);
        Pipeline(*pipe->pipeline);
        return;
      }
      case NodeKind::kSkip:
        for (const syntax::SkipTarget& target :
             syntax::As<syntax::Skip>(statement)->targets) {
          if (target.pipeline != nullptr) Pipeline(*target.pipeline);
        }
        return;
      case NodeKind::kCarry:
        Pipeline(*syntax::As<syntax::Carry>(statement)->pipeline);
        return;
      case NodeKind::kBind:
        Statement(syntax::As<syntax::Bind>(statement)->value.get());
        return;
      case NodeKind::kCallStatement:
        Call(*syntax::As<syntax::CallStatement>(statement)->call);
        return;
      case NodeKind::kCallExpression:
        Call(*syntax::As<syntax::CallExpression>(statement));
        return;
      case NodeKind::kForEach: {
        const auto* loop = syntax::As<syntax::ForEach>(statement);
        Pipeline(*loop->pipeline);
        Count(loop->parallel, "parallel", loop->location);
        Statements(loop->body);
        return;
      }
      case NodeKind::kRepeat: {
        const auto* repeat = syntax::As<syntax::Repeat>(statement);
        if (repeat->max_iterations.has_value()) {
          Count(*repeat->max_iterations, "max", repeat->location);
        }
        Statements(repeat->body);
        return;
      }
      case NodeKind::kIf: {
        const auto* branch = syntax::As<syntax::If>(statement);
        Expression(branch->condition.get());
        Statements(branch->then_body);
        Statements(branch->else_body);
        return;
      }
      case NodeKind::kUntil:
        Expression(syntax::As<syntax::Until>(statement)->condition.get());
        return;
      case NodeKind::kFail: {
        const auto* fail = syntax::As<syntax::Fail>(statement);
        Expression(fail->code.get());
        Expression(fail->message.get());
        return;
      }
      case NodeKind::kLog:
        for (const syntax::NodePtr& argument :
             syntax::As<syntax::Log>(statement)->tail.arguments) {
          Expression(argument.get());
        }
        return;
      case NodeKind::kNodes:
        Statements(syntax::As<syntax::Nodes>(statement)->body);
        return;
      default:
        return;
    }
  }

  void Call(const syntax::CallExpression& call) {
    for (const syntax::CallExpression::Argument& argument : call.args) {
      Pipeline(*argument.pipeline);
    }
  }

  void Count(int count, std::string_view word,
             const syntax::Location& location) {
    if (count >= 1) return;
    Report("flow.form.count-not-positive",
           absl::StrCat("'", word, " ", count,
                        "' is not a number of anything."),
           location, Severity::kWarning, Family::kForm);
  }

  /// The stage arithmetic, over one pipeline.
  ///
  /// One fact carries down the chain: after a reducing stage there is exactly one
  /// value, whatever the stream held before it. Everything here follows from that,
  /// which is why it is a walk rather than a table of pairs.
  void Pipeline(const syntax::Pipeline& pipeline) {
    const syntax::Stage* reduced_by = nullptr;
    const syntax::Stage* previous = nullptr;
    for (const syntax::StagePtr& held : pipeline.stages) {
      const syntax::Stage& stage = *held;
      if (stage.takes == vocabulary::StageArgument::kExpression &&
          stage.argument != nullptr) {
        // `| where (x | count) > 0` -- a pipeline inside a stage is a pipeline.
        Expression(stage.argument.get());
      }
      for (const syntax::NodePtr& argument : stage.log.arguments) {
        Expression(argument.get());
      }
      if (stage.takes == vocabulary::StageArgument::kNumber &&
          stage.number < 1) {
        Report("flow.sequence.impossible",
               absl::StrCat(StageName(stage), " keeps nothing."),
               stage.location, Severity::kWarning, Family::kSequence);
      } else if (reduced_by != nullptr && IsPositional(stage)) {
        // One value is left, so choosing *which* values to keep is either
        // impossible or a no-op.
        const bool keeps_one =
            (stage.name == "first" || stage.name == "last") &&
            stage.number == 1;
        if (keeps_one) {
          Report("flow.sequence.redundant-stage",
                 absl::StrCat(StageName(stage), " has nothing to choose from: ",
                              StageName(*reduced_by),
                              " left one value."),
                 stage.location, Severity::kWeakWarning, Family::kSequence);
        } else if (stage.name == "count") {
          Report("flow.sequence.impossible",
                 absl::StrCat(StageName(stage), " is 1 here, however long the "
                              "stream was: ",
                              StageName(*reduced_by), " left one value."),
                 stage.location, Severity::kWarning, Family::kSequence);
        } else {
          Report("flow.sequence.impossible",
                 absl::StrCat(StageName(stage),
                              " reads a stream of values, and ",
                              StageName(*reduced_by), " left one."),
                 stage.location, Severity::kWarning, Family::kSequence);
        }
      } else if (previous != nullptr && previous->name == stage.name &&
                 IsIdempotent(stage.name)) {
        Report("flow.sequence.redundant-stage",
               absl::StrCat(StageName(stage), " twice does what one does."),
               stage.location, Severity::kWeakWarning, Family::kSequence);
      }
      if (reduced_by == nullptr && Reduces(stage)) reduced_by = &stage;
      previous = &stage;
    }
    Expression(pipeline.source.get());
  }

  /// Pipelines hidden inside an expression: `(hits | count) > 0`.
  void Expression(const Node* expression) {
    if (expression == nullptr) return;
    switch (expression->kind) {
      case NodeKind::kPipelineValue:
        Pipeline(*syntax::As<syntax::PipelineValue>(expression)->pipeline);
        return;
      case NodeKind::kListLiteral:
        for (const syntax::NodePtr& item :
             syntax::As<syntax::ListLiteral>(expression)->items) {
          Expression(item.get());
        }
        return;
      case NodeKind::kObjectLiteral:
        for (const auto& [key, value] :
             syntax::As<syntax::ObjectLiteral>(expression)->pairs) {
          Expression(value.get());
        }
        return;
      case NodeKind::kBuiltin:
        for (const syntax::NodePtr& argument :
             syntax::As<syntax::Builtin>(expression)->args) {
          Expression(argument.get());
        }
        return;
      case NodeKind::kUnary:
        Expression(syntax::As<syntax::Unary>(expression)->operand.get());
        return;
      case NodeKind::kBinary: {
        const auto* binary = syntax::As<syntax::Binary>(expression);
        Expression(binary->left.get());
        Expression(binary->right.get());
        return;
      }
      case NodeKind::kTypedValue:
        Expression(syntax::As<syntax::TypedValue>(expression)->value.get());
        return;
      case NodeKind::kAttr:
        Expression(syntax::As<syntax::Attr>(expression)->base.get());
        return;
      case NodeKind::kIndex: {
        const auto* index = syntax::As<syntax::Index>(expression);
        Expression(index->base.get());
        Expression(index->index.get());
        return;
      }
      default:
        return;
    }
  }

  // --- barriers that cannot hold --------------------------------------------

  /// The orderings a body's own steps make impossible.
  ///
  /// Read off the resolved steps rather than the text, because that is where the
  /// order is: a barrier the flow reaches twice, and a `cancel` of something this
  /// body already waited for, are both statements about the sequence.
  void Barriers(const std::vector<StepPlan>& steps) {
    absl::flat_hash_map<std::string, size_t> waited;
    for (const StepPlan& step : steps) {
      if (step.kind == "wait" || step.kind == "drain") {
        const std::string subject =
            step.kind == "wait" ? step.source : step.destination;
        if (subject.empty()) continue;
        const auto [found, fresh] = waited.emplace(subject, 0);
        if (!fresh) {
          Report("flow.barrier.duplicate",
                 absl::StrCat("This body already holds for ", Quoted(subject),
                              "; the second one is over as soon as it starts."),
                 step.location, Severity::kWeakWarning, Family::kBarrier);
        }
        continue;
      }
      if (step.kind == "cancel") {
        // `wait x` puts the call's outcome under `status x`, which is the label a
        // wait step carries.
        if (waited.contains(absl::StrCat("status ", step.label))) {
          Report("flow.barrier.cancel-after-wait",
                 absl::StrCat("This body already waited for ",
                              Quoted(step.label),
                              ", so there is nothing left to stop."),
                 step.location, Severity::kWarning, Family::kBarrier);
        }
      }
    }
    for (const StepPlan& step : steps) {
      for (const std::vector<StepPlan>& body : step.bodies) Barriers(body);
    }
  }

  const LineIndex& lines_;
  std::vector<Diagnostic>& found_;
  std::string flow_;
};

}  // namespace

std::vector<Diagnostic> Inspect(std::string_view source,
                                const ParseResult& parsed,
                                const ResolveResult& resolved) {
  std::vector<Diagnostic> found;
  // A file that does not resolve is a file whose facts are unreliable: an unknown
  // name reads as nothing using the thing it meant to use, and every "nothing uses
  // this" would be noise on top of the real problem.
  if (resolved.HasErrors()) return found;
  const LineIndex lines(source);
  Inspector(lines, found).Run(parsed, resolved);
  SortDiagnostics(found);
  return found;
}

}  // namespace a11::flow
