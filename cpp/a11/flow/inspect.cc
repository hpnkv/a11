// Copyright 2026 The A11 Authors.

#include "a11/flow/inspect.h"

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
///
/// `sort` is here because sorting a sorted stream is the same stream, and
/// `flatten` is deliberately *not*: a stream of lists of lists is flattened one
/// level at a time, so twice is a second level and not a repeat.
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
      if (flow.declaration == nullptr) {
        continue;
      }
      flow_ = flow.plan.name;
      current_ = &flow;
      Unused(flow);
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
    if (resolved.program.dtos.empty() || resolved.flows.empty()) {
      return;
    }
    absl::flat_hash_set<std::string> named;
    for (const DtoPlan& dto : resolved.program.dtos) {
      for (const FieldPlan& field : dto.fields) {
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
      }
      if (flow.declaration != nullptr) {
        NamedTypes(flow.declaration->body, named);
      }
    }
    for (const syntax::DtoDeclarationPtr& declaration : parsed.dtos) {
      if (named.contains(declaration->name.text)) {
        continue;
      }
      if (resolved.program.Dto(declaration->name.text) == nullptr) {
        continue;
      }
      Report("flow.unused.struct",
             absl::StrCat("Nothing names the shape ",
                          Quoted(declaration->name.text),
                          ": no port carries it and no value is made one."),
             declaration->name.location, Severity::kWeakWarning,
             Family::kUnused);
    }
  }

  /// Every type a body writes down, however deep: `Shape{..}` and `x as Shape`.
  ///
  /// Every node, whatever its kind: a cast can be anywhere an expression can,
  /// and enumerating the places would be a list to keep in step with the
  /// grammar for no gain. VisitSubtree rather than a recursive walk of its own,
  /// so a deeply nested document costs heap rather than fibre stack.
  static void NamedTypes(const std::vector<syntax::NodePtr>& body,
                         absl::flat_hash_set<std::string>& named) {
    for (const syntax::NodePtr& node : body) {
      NamedTypes(node.get(), named);
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
        named.insert(typed->type.name);
      }
    });
  }

  // --- what nothing uses ----------------------------------------------------

  void Unused(const ResolvedFlow& flow) {
    for (const Symbol& symbol : flow.symbols) {
      if (symbol.implicit || symbol.name.empty()) {
        continue;
      }
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
            Report(
                "flow.unused.try-status",
                absl::StrCat("'try' lets ", symbol.action,
                             " fail without ending the flow, and nothing "
                             "here reads its status: a failure leaves the "
                             "ports it feeds silently empty. Read it with "
                             "'wait ",
                             symbol.name, "' or 'status ", symbol.name, "'."),
                symbol.location, Severity::kWeakWarning, Family::kUnused);
          }
          break;
        default:
          break;
      }
    }
  }

  // --- a moment nothing pins down -------------------------------------------

  /// The symbol a name resolves to, or null.
  ///
  /// Flat, so a name declared in two scopes answers with the first: enough for
  /// the questions below, which ask what *kind* of thing a name is, and a flow
  /// that gives one name to a node here and a loop variable there is a flow
  /// with a worse problem than the one being reported.
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

  /// Whether a name stands for something that only exists once the flow runs.
  ///
  /// An `in` port and a header are there before the first statement, so reading
  /// one says nothing about when a statement happened. A node, a call's port and
  /// a barrier are filled in *while* the flow runs, and reading one at a moment
  /// nothing pins down is the mistake this file is about.
  bool ProducedWhileRunning(std::string_view name) const {
    const Symbol* symbol = SymbolNamed(name);
    if (symbol == nullptr) {
      return false;
    }
    return symbol->kind == SymbolKind::kNode ||
           symbol->kind == SymbolKind::kCall ||
           symbol->kind == SymbolKind::kBarrier;
  }

  /// A clock read in a statement nothing orders.
  ///
  /// `now() - started -> elapsed` at the top of a body runs at once, with every
  /// other statement in it, so what it measures is the flow starting rather
  /// than whatever it was written to time -- and it is written where it reads
  /// as the last thing that happens.
  ///
  /// `now() -> started` is the other half of the same idiom and is exactly
  /// right, which is why a value read is asked for too: a clock read on its own
  /// is a *start*, a clock read against something the flow produced is a
  /// *measurement*, and only a measurement needs a moment.
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
           absl::StrCat(
               "This reads the clock against ", Quoted(read->name),
               " at the top of a body with no 'after', so it runs at once with "
               "everything else here and measures the flow starting rather "
               "than what it was written under. Give it an 'after', or put it "
               "in an 'if' or a loop body."),
           clock->location, Severity::kWarning, Family::kBarrier);
  }

  /// One statement reading one stream twice where a value belongs.
  ///
  /// Two value reads of one stream take *turns on one view of it*, so they see
  /// two different values rather than two copies of the first -- and a node
  /// carrying one value gives the second read the end of the stream, which
  /// renders as nothing at all. Within one statement there is no `after` that
  /// could say which read comes first, so the only fix is to stop reading twice:
  /// `let` names a value, and a value is shared.
  ///
  /// Only nodes of the flow's own: everything else a flow reads for a value
  /// either provably carries one (a port that did not say `stream`, a header, a
  /// status) or is a `let` value already.
  void ValueReadTwice(const std::vector<const Node*>& roots) {
    // Counted first and reported afterwards, at the *second* read in the text:
    // [syntax::VisitSubtree] promises only that a parent comes before its
    // children, so reporting as they arrive would point at either one of them
    // depending on the shape of the statement.
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
             absl::StrCat("This statement reads ", Quoted(name),
                          " twice where a value belongs, and two reads of one "
                          "stream take turns on one view of it: they see two "
                          "different values, and the second sees nothing at "
                          "all when the node carries one. Read it once into a "
                          "value -- 'let one = ",
                          name, "' -- and use that twice."),
             second->location, Severity::kWarning, Family::kBarrier);
    }
  }

  /// `wait n` on a node this flow lends out rather than writes.
  ///
  /// Nothing here writes it, so nothing here would close it either -- and the
  /// barrier does, which makes it an *ending* rather than a wait: it returns as
  /// soon as it has closed the node, and the callee that was lent `n.id` finds
  /// its writer shut. `drain n after <call>` is the one that waits, and says
  /// out loud that the flow which lent the node is the flow that ends it.
  void LentNodes(const ResolvedFlow& flow) {
    absl::flat_hash_set<std::string> written;
    Written(flow.plan.steps, written);
    Holds(flow.plan.steps, written);
  }

  /// Every node a pipe of this flow puts values into, however deeply nested.
  ///
  /// Pipes only: a `drain` carries its node as a destination too, and counting
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
      if (step.kind != "wait" && step.kind != "drain") {
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
      Report("flow.barrier.wait-lends-node",
             absl::StrCat("Nothing in this flow writes ", Quoted(subject),
                          ", so this does not wait for it -- it *ends* it, and "
                          "at once: whoever was lent '",
                          subject,
                          ".id' finds its writer closed. Say which step fills "
                          "it: 'drain ",
                          subject, " after <that step>'."),
             step.location, Severity::kWarning, Family::kBarrier);
    }
  }

  // --- sequences that cannot do what they look like -------------------------

  void Statements(const std::vector<syntax::NodePtr>& statements) {
    for (const syntax::NodePtr& statement : statements) {
      Statement(statement.get());
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
      case NodeKind::kPipe: {
        const auto* pipe = syntax::As<syntax::Pipe>(statement);
        Pipeline(*pipe->pipeline);
        const std::vector<const Node*> read = Sources(*pipe->pipeline);
        ClockRace(read, pipe->after);
        ValueReadTwice(read);
        return;
      }
      case NodeKind::kSkip:
        for (const syntax::SkipTarget& target :
             syntax::As<syntax::Skip>(statement)->targets) {
          if (target.pipeline != nullptr) {
            Pipeline(*target.pipeline);
          }
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

  /// Pipelines hidden inside an expression: `(hits | count) > 0`.
  /// Every pipeline written inside an expression, however deeply nested.
  ///
  /// A work list rather than recursion: the depth here is the document's
  /// nesting, and A11 runs this on pooled fibres whose stacks are fixed and
  /// small. Children are pushed in reverse so the visit order is the one a
  /// recursive descent would have produced.
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

  // --- barriers that cannot hold --------------------------------------------

  /// The orderings a body's own steps make impossible.
  ///
  /// Read off the resolved steps rather than the text, because that is where
  /// the order is: a barrier the flow reaches twice, and a `cancel` of
  /// something this body already waited for, are both statements about the
  /// sequence.
  void Barriers(const std::vector<StepPlan>& steps) {
    absl::flat_hash_map<std::string, size_t> waited;
    for (const StepPlan& step : steps) {
      if (step.kind == "wait" || step.kind == "drain") {
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
  /// How many `if`/`for`/`repeat` bodies deep this statement is. What it
  /// answers is the one `CheckReachedByChoice` asks in the resolver: whether
  /// anything at all says when this statement runs. A `nodes` block does not
  /// count -- it joins the body around it and changes nothing about when its
  /// statements run.
  int guarded_ = 0;
};

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
  SortDiagnostics(found);
  return found;
}

}  // namespace a11::flow
