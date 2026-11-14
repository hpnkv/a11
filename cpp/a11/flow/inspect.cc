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

#include "a11/flow/inspect.h"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/functional/function_ref.h>
#include <absl/strings/str_cat.h>
#include <nlohmann/json.hpp>

#include "a11/flow/diagnostic.h"
#include "a11/flow/emit_json.h"
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
  if (stage.takes == vocabulary::StageArgument::kNumber && stage.is_integer) {
    return absl::StrCat("'| ", stage.name, " ", stage.integer, "'");
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

/// Idempotent stages. `flatten` is excluded because it removes one level.
bool IsIdempotent(std::string_view stage) {
  return stage == "collect" || stage == "distinct" || stage == "text" ||
         stage == "json" || stage == "packb" || stage == "sort";
}

class Inspector {
 public:
  Inspector(const LineIndex& lines, std::vector<Diagnostic>& found)
      : lines_(lines), found_(found) {}

  void Run(const ParseResult& parsed, const ResolveResult& resolved) {
    for (const ResolvedFlow& flow : resolved.flows) {
      if (flow.declaration == nullptr || flow.plan.interface_only) {
        continue;
      }
      flow_ = flow.plan.name;
      current_ = &flow;
      Unused(flow);
      UnreadPipelines(flow);
      Statements(flow.declaration->body);
      Barriers(flow.plan.steps);
      LentNodes(flow);
      current_ = nullptr;
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

  /// Report shapes with no references across a file containing flows.
  /// Shape-only files are valid type declarations and produce no warning.
  void UnusedShapes(const ParseResult& parsed, const ResolveResult& resolved) {
    if (resolved.program.dtos.empty() || resolved.flows.empty()) {
      return;
    }
    absl::flat_hash_set<std::string> named;
    for (const DtoPlan& dto : resolved.program.dtos) {
      for (const FieldPlan& field : dto.fields) {
        if (field.syntax_type.has_value()) {
          NamedType(*field.syntax_type, named);
        }
        if (!field.dto_name.empty()) {
          named.insert(field.dto_name);
        }
        if (!field.element_dto_name.empty()) {
          named.insert(field.element_dto_name);
        }
      }
    }
    for (const ResolvedFlow& flow : resolved.flows) {
      for (const PortPlan& port : flow.plan.ports) {
        named.insert(port.type);
        if (port.syntax_type.has_value()) {
          NamedType(*port.syntax_type, named);
        }
      }
      if (flow.declaration != nullptr) {
        NamedTypes(flow.declaration->body, named);
      }
    }
    for (const syntax::DtoDeclarationPtr& declaration : parsed.dtos) {
      if (declaration->exported) {
        continue;
      }
      if (named.contains(declaration->name.text)) {
        continue;
      }
      if (resolved.program.Dto(declaration->name.text) == nullptr) {
        continue;
      }
      Report("flow.unused.struct",
             absl::StrCat("Struct ", Quoted(declaration->name.text),
                          " has no references from ports, casts, or values."),
             declaration->name.location, Severity::kWeakWarning,
             Family::kUnused);
    }
  }

  /// Find type names throughout a body using the stack-bounded VisitSubtree.
  static void NamedTypes(const std::vector<syntax::NodePtr>& body,
                         absl::flat_hash_set<std::string>& named) {
    for (const syntax::NodePtr& node : body) {
      NamedTypes(node.get(), named);
    }
  }

  static void NamedType(const syntax::TypeExpression& type,
                        absl::flat_hash_set<std::string>& named) {
    std::vector<const syntax::TypeExpression*> pending{&type};
    while (!pending.empty()) {
      const auto* current = pending.back();
      pending.pop_back();
      named.insert(current->name);
      for (const auto& parameter : current->parameters) {
        pending.push_back(&parameter);
      }
    }
  }

  static void NamedTypes(const syntax::Node* absl_nullable node,
                         absl::flat_hash_set<std::string>& named) {
    if (node == nullptr) {
      return;
    }
    syntax::VisitSubtree(*node, [&named](const syntax::Node& one) {
      if (const auto* typed = syntax::As<syntax::TypedValue>(&one);
          typed != nullptr) {
        NamedType(typed->type, named);
      } else if (const auto* bind = syntax::As<syntax::Bind>(&one);
                 bind != nullptr && bind->type.has_value()) {
        NamedType(*bind->type, named);
      }
    });
  }

  // --- Unused declarations -------------------------------------------------

  void Unused(const ResolvedFlow& flow) {
    for (const Symbol& symbol : flow.symbols) {
      if (symbol.implicit || symbol.name.empty()) {
        continue;
      }
      switch (symbol.kind) {
        case SymbolKind::kHeader:
          if (symbol.reads == 0) {
            Report("flow.unused.header",
                   absl::StrCat("Header alias ", Quoted(symbol.name),
                                " has no reads. 'x-a11-' headers propagate "
                                "without a declaration."),
                   symbol.location, Severity::kWeakWarning, Family::kUnused);
          }
          break;
        case SymbolKind::kOutputPort:
          if (symbol.writes == 0) {
            Report("flow.unused.output-port",
                   absl::StrCat("Output port ", Quoted(symbol.name),
                                " has no writes and produces no values."),
                   symbol.location, Severity::kWarning, Family::kUnused);
          }
          break;
        case SymbolKind::kNodeMap:
          if (symbol.reads == 0) {
            Report("flow.unused.node-map",
                   absl::StrCat("Node map ", Quoted(symbol.name),
                                " has no placements. Use a block or 'via'."),
                   symbol.location, Severity::kWeakWarning, Family::kUnused);
          }
          break;
        case SymbolKind::kValue:
          if (symbol.reads == 0) {
            Report("flow.unused.value",
                   absl::StrCat("Value ", Quoted(symbol.name),
                                " has no reads; its initializer executes."),
                   symbol.location, Severity::kWarning, Family::kUnused);
          }
          break;
        case SymbolKind::kLoopVariable:
          if (symbol.reads == 0) {
            Report("flow.unused.loop-variable",
                   absl::StrCat("Loop variable ", Quoted(symbol.name),
                                " has no reads."),
                   symbol.location, Severity::kWeakWarning, Family::kUnused);
          }
          break;
        case SymbolKind::kBarrier:
          if (symbol.reads == 0) {
            Report("flow.unused.barrier-name",
                   absl::StrCat("Barrier name ", Quoted(symbol.name),
                                " has no reads; the barrier still executes."),
                   symbol.location, Severity::kWeakWarning, Family::kUnused);
          }
          break;
        case SymbolKind::kCall:
          if (symbol.tolerant && symbol.status_reads == 0) {
            Report(
                "flow.unused.try-status",
                absl::StrCat("Status of 'try' call ", Quoted(symbol.name),
                             " is unread. A failure leaves its output ports "
                             "empty. Read it with 'wait ",
                             symbol.name, "' or 'status ", symbol.name, "'."),
                symbol.location, Severity::kWeakWarning, Family::kUnused);
          }
          break;
        default:
          break;
      }
    }
  }

  void UnreadPipelines(const ResolvedFlow& flow) {
    absl::flat_hash_map<size_t, size_t> bindings;
    for (size_t i = 0; i < flow.symbols.size(); ++i) {
      bindings.emplace(flow.symbols[i].location.start, i);
    }
    absl::flat_hash_map<size_t, absl::flat_hash_set<size_t>> dependencies;
    absl::flat_hash_set<const Node*> deferred_names;
    std::vector<std::pair<size_t, syntax::Location>> candidates;
    syntax::VisitSubtree(*flow.declaration, [&](const Node& node) {
      const auto* bind = syntax::As<syntax::Bind>(&node);
      if (bind == nullptr || bind->value == nullptr) {
        return;
      }
      const auto found = bindings.find(bind->name.location.start);
      if (found == bindings.end()) {
        return;
      }
      const auto* pipeline =
          syntax::As<syntax::PipelineValue>(bind->value.get());
      const bool deferred =
          syntax::As<syntax::Name>(bind->value.get()) != nullptr ||
          (pipeline != nullptr && !pipeline->take);
      if (!deferred) {
        return;
      }
      syntax::VisitSubtree(*bind->value, [&](const Node& child) {
        if (syntax::As<syntax::Name>(&child) == nullptr) {
          return;
        }
        deferred_names.insert(&child);
        const auto info = flow.expressions.find(&child);
        if (info != flow.expressions.end() &&
            info->second.symbol < flow.symbols.size()) {
          dependencies[found->second].insert(info->second.symbol);
        }
      });
      if (pipeline == nullptr) {
        return;
      }
      const bool starter = syntax::As<syntax::CallExpression>(
                               pipeline->pipeline->source.get()) != nullptr;
      const bool stage = std::any_of(
          pipeline->pipeline->stages.begin(), pipeline->pipeline->stages.end(),
          [](const syntax::StagePtr& one) {
            return one->name == "run" || one->name == "call";
          });
      if (starter || stage) {
        candidates.emplace_back(found->second, bind->name.location);
      }
    });
    absl::flat_hash_set<size_t> used;
    std::vector<size_t> pending;
    syntax::VisitSubtree(*flow.declaration, [&](const Node& node) {
      if (syntax::As<syntax::Name>(&node) == nullptr ||
          deferred_names.contains(&node)) {
        return;
      }
      const auto info = flow.expressions.find(&node);
      if (info != flow.expressions.end() &&
          info->second.symbol < flow.symbols.size() &&
          used.insert(info->second.symbol).second) {
        pending.push_back(info->second.symbol);
      }
    });
    while (!pending.empty()) {
      const size_t one = pending.back();
      pending.pop_back();
      const auto found = dependencies.find(one);
      if (found == dependencies.end()) {
        continue;
      }
      for (const size_t source : found->second) {
        if (used.insert(source).second) {
          pending.push_back(source);
        }
      }
    }
    for (const auto& [symbol, location] : candidates) {
      if (!used.contains(symbol)) {
        Report("flow.unused.pipeline-call",
               absl::StrCat("Pipeline ", Quoted(flow.symbols[symbol].name),
                            " has no reader, so its run/call invocations will "
                            "not start. Remove it or add '-> _' to execute "
                            "and discard its output."),
               location, Severity::kWarning, Family::kUnused);
      }
    }
  }

  // --- Unconstrained values ------------------------------------------------

  /// Returns the first matching symbol across scopes, or null.
  const Symbol* absl_nullable SymbolNamed(std::string_view name) const {
    if (current_ == nullptr) {
      return nullptr;
    }
    for (const Symbol& symbol : current_->symbols) {
      if (symbol.name == name) {
        return &symbol;
      }
    }
    return nullptr;
  }

  /// Whether a name is produced during execution: a node, call or barrier.
  bool ProducedWhileRunning(std::string_view name) const {
    const Symbol* symbol = SymbolNamed(name);
    if (symbol == nullptr) {
      return false;
    }
    return symbol->kind == SymbolKind::kNode ||
           symbol->kind == SymbolKind::kCall ||
           symbol->kind == SymbolKind::kBarrier;
  }

  /// Warn when an unordered clock read measures a value produced by the flow.
  /// A standalone `now() -> started` records a start time without a warning.
  void ClockRace(const std::vector<const Node*>& roots,
                 const std::vector<syntax::Word>& after) {
    if (guarded_ > 0 || !after.empty()) {
      return;
    }
    const syntax::Builtin* clock = nullptr;
    const syntax::Name* read = nullptr;
    for (const Node* root : roots) {
      if (root == nullptr) {
        continue;
      }
      syntax::VisitSubtree(*root, [&](const Node& one) {
        if (const auto* builtin = syntax::As<syntax::Builtin>(&one);
            builtin != nullptr) {
          if (builtin->name == "now" &&
              (clock == nullptr ||
               one.location.start < clock->location.start)) {
            clock = builtin;
          }
          return;
        }
        const auto* name = syntax::As<syntax::Name>(&one);
        if (name == nullptr || !ProducedWhileRunning(name->name)) {
          return;
        }
        if (read == nullptr || one.location.start < read->location.start) {
          read = name;
        }
      });
    }
    if (clock == nullptr || read == nullptr) {
      return;
    }
    Report("flow.barrier.unordered-clock",
           absl::StrCat("Clock read against ", Quoted(read->name),
                        " has no ordering dependency. Add 'after', or place "
                        "the measurement in an 'if' or loop body."),
           clock->location, Severity::kWarning, Family::kBarrier);
  }

  /// Warn when one statement reads the same local node as a value twice.
  /// Each read consumes a distinct stream item; name a value to share it.
  void ValueReadTwice(const std::vector<const Node*>& roots) {
    // Counted first and reported afterwards, at the *second* read in the text:
    // [syntax::VisitSubtree] promises only that a parent comes before its
    // children, so reporting as they arrive would point at either one of them.
    absl::flat_hash_map<std::string, std::pair<int, const syntax::Name*>> seen;
    for (const Node* root : roots) {
      if (root == nullptr) {
        continue;
      }
      // A bare name where a source belongs is the *stream*, read once by this
      // statement however long it is. Anything else is an expression, and the
      // names inside it are values.
      if (syntax::As<syntax::Name>(root) != nullptr ||
          syntax::As<syntax::Attr>(root) != nullptr) {
        continue;
      }
      syntax::VisitSubtree(*root, [&](const Node& one) {
        const auto* name = syntax::As<syntax::Name>(&one);
        if (name == nullptr) {
          return;
        }
        const Symbol* symbol = SymbolNamed(name->name);
        if (symbol == nullptr || symbol->kind != SymbolKind::kNode) {
          return;
        }
        auto [found, fresh] = seen.emplace(name->name, std::pair{1, name});
        if (fresh) {
          return;
        }
        ++found->second.first;
        if (name->location.start > found->second.second->location.start) {
          found->second.second = name;
        }
      });
    }
    for (const auto& [name, counted] : seen) {
      const auto [reads, second] = counted;
      if (reads < 2) {
        continue;
      }
      Report("flow.barrier.value-read-twice",
             absl::StrCat("Two scalar reads of ", Quoted(name),
                          " consume separate stream values. Bind one read "
                          "with 'one = take ",
                          name, "' and reuse the value."),
             second->location, Severity::kWarning, Family::kBarrier);
    }
  }

  /// Warn when `wait n` closes a lent node this flow never writes.
  /// Use `close n after <call>` to wait for the borrower before closing.
  void LentNodes(const ResolvedFlow& flow) {
    absl::flat_hash_set<std::string> written;
    Written(flow.plan.steps, written);
    Holds(flow.plan.steps, written);
  }

  /// Every node a pipe of this flow puts values into, however deeply nested.
  ///
  /// Pipes only: a `close` carries its node as a destination too, and counting
  /// that would be this check answering itself.
  static void Written(const std::vector<StepPlan>& steps,
                      absl::flat_hash_set<std::string>& written) {
    for (const StepPlan& step : steps) {
      if (step.kind == "pipe" && !step.destination.empty()) {
        written.insert(step.destination);
      }
      for (const std::vector<StepPlan>& body : step.bodies) {
        Written(body, written);
      }
    }
  }

  void Holds(const std::vector<StepPlan>& steps,
             const absl::flat_hash_set<std::string>& written) {
    for (const StepPlan& step : steps) {
      for (const std::vector<StepPlan>& body : step.bodies) {
        Holds(body, written);
      }
      if (step.kind != "wait" && step.kind != "close") {
        continue;
      }
      if (!step.after.empty()) {
        continue;
      }
      // A wait carries its subject as the outcome it reads: `status n`.
      std::string_view subject =
          step.kind == "wait" ? step.source : step.destination;
      constexpr std::string_view kStatusOf = "status ";
      if (subject.starts_with(kStatusOf)) {
        subject.remove_prefix(kStatusOf.size());
      }
      if (subject.empty() || written.contains(subject)) {
        continue;
      }
      const Symbol* symbol = SymbolNamed(subject);
      if (symbol == nullptr || symbol->kind != SymbolKind::kNode) {
        continue;
      }
      Report(
          "flow.barrier.wait-lends-node",
          absl::StrCat("Node ", Quoted(subject),
                       " has no local writer. This barrier closes it "
                       "immediately; use 'close ",
                       subject, " after <producer>' to wait for the producer."),
          step.location, Severity::kWarning, Family::kBarrier);
    }
  }

  // --- Invalid sequences ---------------------------------------------------

  void Statements(const std::vector<syntax::NodePtr>& statements) {
    bool phase = false;
    for (const syntax::NodePtr& statement : statements) {
      Statement(statement.get());
      const auto* wait = syntax::As<syntax::Wait>(statement.get());
      if (!phase && wait != nullptr && wait->scope_barrier) {
        ++guarded_;
        phase = true;
      }
    }
    if (phase) {
      --guarded_;
    }
  }

  /// Walk a body whose statements are conditional on something.
  void Guarded(absl::FunctionRef<void()> walk) {
    ++guarded_;
    walk();
    --guarded_;
  }

  void Statement(const Node* statement) {
    if (statement == nullptr) {
      return;
    }
    switch (statement->kind) {
      case NodeKind::kBlock: {
        const auto* block = syntax::As<syntax::Block>(statement);
        if (block->after.empty()) {
          Statements(block->body);
        } else {
          Guarded([&] { Statements(block->body); });
        }
        return;
      }
      case NodeKind::kPipe: {
        const auto* pipe = syntax::As<syntax::Pipe>(statement);
        Pipeline(*pipe->pipeline);
        const std::vector<const Node*> read = Sources(*pipe->pipeline);
        ClockRace(read, pipe->after);
        ValueReadTwice(read);
        for (const auto& target : pipe->targets) {
          if (target->kind == NodeKind::kPipe) {
            Statement(target.get());
          }
        }
        return;
      }
      case NodeKind::kSkip:
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
        Expression(loop->parallel_expression.get());
        Count(loop->parallel, "parallel", loop->location);
        Guarded([&] { Statements(loop->body); });
        return;
      }
      case NodeKind::kRepeat: {
        const auto* repeat = syntax::As<syntax::Repeat>(statement);
        if (repeat->max_iterations.has_value()) {
          Count(*repeat->max_iterations, "max", repeat->location);
        }
        Guarded([&] { Statements(repeat->body); });
        return;
      }
      case NodeKind::kIf: {
        const auto* branch = syntax::As<syntax::If>(statement);
        Expression(branch->condition.get());
        Guarded([&] {
          Statements(branch->then_body);
          Statements(branch->else_body);
        });
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
    std::vector<const Node*> read;
    for (const syntax::CallExpression::Argument& argument : call.args) {
      Pipeline(*argument.pipeline);
      for (const Node* one : Sources(*argument.pipeline)) {
        read.push_back(one);
      }
    }
    // One statement, so one question each: the arguments are read together.
    ClockRace(read, call.modifiers->after);
    ValueReadTwice(read);
  }

  /// What a pipeline reads: its source, and every expression its stages carry.
  static std::vector<const Node*> Sources(const syntax::Pipeline& pipeline) {
    std::vector<const Node*> found{pipeline.source.get()};
    for (const syntax::StagePtr& held : pipeline.stages) {
      if (held == nullptr) {
        continue;
      }
      found.push_back(held->argument.get());
      for (const syntax::NodePtr& argument : held->log.arguments) {
        found.push_back(argument.get());
      }
    }
    return found;
  }

  void Count(int count, std::string_view word,
             const syntax::Location& location) {
    if (count >= 1) {
      return;
    }
    Report(
        "flow.form.count-not-positive",
        absl::StrCat("'", word, " ", count, "' is not a number of anything."),
        location, Severity::kWarning, Family::kForm);
  }

  /// The stage arithmetic, over one pipeline.
  ///
  /// One fact carries down the chain: after a reducing stage there is exactly
  /// one value, whatever the stream held before it. Everything here follows
  /// from that, which is why it is a walk rather than a table of pairs.
  void Pipeline(const syntax::Pipeline& pipeline) {
    const syntax::Stage* reduced_by = nullptr;
    const syntax::Stage* previous = nullptr;
    for (const syntax::StagePtr& held : pipeline.stages) {
      const syntax::Stage& stage = *held;
      if (stage.argument != nullptr) {
        // `| where (x | count) > 0` -- a pipeline inside a stage is a pipeline.
        Expression(stage.argument.get());
      }
      Expression(stage.parallel_expression.get());
      Expression(stage.context_before.get());
      Expression(stage.context_after.get());
      Expression(stage.context_base.get());
      for (const syntax::NodePtr& argument : stage.log.arguments) {
        Expression(argument.get());
      }
      if (stage.takes == vocabulary::StageArgument::kNumber &&
          stage.is_integer && stage.integer < 1) {
        Report("flow.sequence.impossible",
               absl::StrCat(StageName(stage), " keeps nothing."),
               stage.location, Severity::kWarning, Family::kSequence);
      } else if (reduced_by != nullptr && IsPositional(stage)) {
        // One value is left, so choosing *which* values to keep is either
        // impossible or a no-op.
        const bool keeps_one =
            (stage.name == "first" || stage.name == "last") &&
            stage.is_integer && stage.integer == 1;
        if (keeps_one) {
          Report("flow.sequence.redundant-stage",
                 absl::StrCat(StageName(stage), " has nothing to choose from: ",
                              StageName(*reduced_by), " left one value."),
                 stage.location, Severity::kWeakWarning, Family::kSequence);
        } else if (stage.name == "count") {
          Report("flow.sequence.impossible",
                 absl::StrCat(StageName(stage),
                              " is 1 here, however long the "
                              "stream was: ",
                              StageName(*reduced_by), " left one value."),
                 stage.location, Severity::kWarning, Family::kSequence);
        } else {
          Report(
              "flow.sequence.impossible",
              absl::StrCat(StageName(stage), " reads a stream of values, and ",
                           StageName(*reduced_by), " left one."),
              stage.location, Severity::kWarning, Family::kSequence);
        }
      } else if (previous != nullptr && previous->name == stage.name &&
                 IsIdempotent(stage.name)) {
        Report("flow.sequence.redundant-stage",
               absl::StrCat(StageName(stage), " twice does what one does."),
               stage.location, Severity::kWeakWarning, Family::kSequence);
      }
      if (reduced_by == nullptr && Reduces(stage)) {
        reduced_by = &stage;
      }
      previous = &stage;
    }
    Expression(pipeline.source.get());
  }

  /// Visit nested expression pipelines with a work list for bounded stack use.
  void Expression(const Node* expression) {
    if (expression == nullptr) {
      return;
    }
    std::vector<const Node*> pending{expression};
    const auto push = [&pending](const Node* node) {
      if (node != nullptr) {
        pending.push_back(node);
      }
    };
    const auto push_all = [&push](const std::vector<syntax::NodePtr>& nodes) {
      for (const syntax::NodePtr& node : std::views::reverse(nodes)) {
        push(node.get());
      }
    };
    while (!pending.empty()) {
      const Node* one = pending.back();
      pending.pop_back();
      switch (one->kind) {
        case NodeKind::kPipelineValue:
          Pipeline(*syntax::As<syntax::PipelineValue>(one)->pipeline);
          break;
        case NodeKind::kListLiteral:
          push_all(syntax::As<syntax::ListLiteral>(one)->items);
          break;
        case NodeKind::kObjectLiteral: {
          const auto& pairs = syntax::As<syntax::ObjectLiteral>(one)->pairs;
          for (const auto& [key, value] : std::views::reverse(pairs)) {
            push(value.get());
          }
          break;
        }
        case NodeKind::kBuiltin:
          push_all(syntax::As<syntax::Builtin>(one)->args);
          break;
        case NodeKind::kUnary:
          push(syntax::As<syntax::Unary>(one)->operand.get());
          break;
        case NodeKind::kBinary: {
          const auto* binary = syntax::As<syntax::Binary>(one);
          push(binary->right.get());
          push(binary->left.get());
          break;
        }
        case NodeKind::kTypedValue:
          push(syntax::As<syntax::TypedValue>(one)->value.get());
          break;
        case NodeKind::kAttr:
          push(syntax::As<syntax::Attr>(one)->base.get());
          break;
        case NodeKind::kIndex: {
          const auto* index = syntax::As<syntax::Index>(one);
          push(index->index.get());
          push(index->base.get());
          break;
        }
        default:
          break;
      }
    }
  }

  // --- Invalid barriers ----------------------------------------------------

  /// Finds impossible barrier orderings in the resolved step sequence.
  void Barriers(const std::vector<StepPlan>& steps) {
    absl::flat_hash_map<std::string, size_t> waited;
    for (const StepPlan& step : steps) {
      if (step.kind == "wait" || step.kind == "close") {
        const std::string subject =
            step.kind == "wait" ? step.source : step.destination;
        if (subject.empty()) {
          continue;
        }
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
        // `wait x` puts the call's outcome under `status x`, which is the label
        // a wait step carries.
        if (waited.contains(absl::StrCat("status ", step.label))) {
          Report(
              "flow.barrier.cancel-after-wait",
              absl::StrCat("This body already waited for ", Quoted(step.label),
                           ", so there is nothing left to stop."),
              step.location, Severity::kWarning, Family::kBarrier);
        }
      }
    }
    for (const StepPlan& step : steps) {
      for (const std::vector<StepPlan>& body : step.bodies) {
        Barriers(body);
      }
    }
  }

  const LineIndex& lines_;
  std::vector<Diagnostic>& found_;
  std::string flow_;
  /// The flow being walked, for the questions that are about a *name*.
  const ResolvedFlow* absl_nullable current_ = nullptr;
  /// Number of enclosing conditions or execution phase gates. `nodes` blocks
  /// do not change when a statement runs and do not count.
  int guarded_ = 0;
};

void StripLocations(nlohmann::json& value) {
  if (value.is_object()) {
    value.erase("at");
    for (auto& child : value.items()) {
      StripLocations(child.value());
    }
  } else if (value.is_array()) {
    for (auto& child : value) {
      StripLocations(child);
    }
  }
}

void RedundantGroups(std::string_view source, const ParseResult& parsed,
                     std::vector<Diagnostic>& found) {
  auto original = SyntaxToJsonValue("", parsed);
  StripLocations(original);
  std::vector<syntax::Location> groups;
  for (const auto& flow : parsed.flows) {
    syntax::VisitSubtree(*flow, [&](const Node& node) {
      groups.insert(groups.end(), node.groups.begin(), node.groups.end());
    });
  }
  std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) {
    return a.end - a.start > b.end - b.start;
  });
  std::vector<bool> removed(source.size(), false);
  const LineIndex lines(source);
  size_t inspected = 0;
  for (const auto& group : groups) {
    if (inspected++ >= 64) {
      break;
    }
    if (group.end <= group.start + 1 || group.end > source.size()) {
      continue;
    }
    const auto text = source.substr(group.start, group.end - group.start);
    if (text.find_first_of("\n\r#") != std::string_view::npos) {
      continue;
    }
    std::string edited;
    edited.reserve(source.size());
    for (size_t offset = 0; offset < source.size(); ++offset) {
      if (!removed[offset] && offset != group.start &&
          offset != group.end - 1) {
        edited += source[offset];
      }
    }
    const ParseResult reparsed = Parse(edited);
    if (reparsed.HasErrors()) {
      continue;
    }
    auto compared = SyntaxToJsonValue("", reparsed);
    StripLocations(compared);
    if (compared != original || Resolve(edited, reparsed).HasErrors()) {
      continue;
    }
    removed[group.start] = true;
    removed[group.end - 1] = true;
    Diagnostic diagnostic;
    diagnostic.code = "flow.sequence.redundant-parentheses";
    diagnostic.severity = Severity::kInformation;
    diagnostic.family = Family::kSequence;
    diagnostic.message = "These parentheses can be removed.";
    diagnostic.range = lines.Between(group.start, group.start + 1);
    diagnostic.related.push_back({"", lines.Between(group.end - 1, group.end),
                                  "The matching closing parenthesis."});
    diagnostic.fixes.push_back(
        {"Remove redundant parentheses",
         {{group.start, group.start + 1, ""}, {group.end - 1, group.end, ""}}});
    found.push_back(std::move(diagnostic));
  }
}

}  // namespace

std::vector<Diagnostic> Inspect(std::string_view source,
                                const ParseResult& parsed,
                                const ResolveResult& resolved) {
  std::vector<Diagnostic> found;
  // A file that does not resolve is a file whose facts are unreliable: an
  // unknown name reads as nothing using the thing it meant to use, and every
  // "nothing uses this" would be noise on top of the real problem.
  if (resolved.HasErrors()) {
    return found;
  }
  const LineIndex lines(source);
  Inspector(lines, found).Run(parsed, resolved);
  RedundantGroups(source, parsed, found);
  SortDiagnostics(found);
  return found;
}

}  // namespace a11::flow
