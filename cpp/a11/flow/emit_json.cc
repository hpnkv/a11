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

#include "a11/flow/emit_json.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>
#include <absl/types/span.h>
#include <nlohmann/json.hpp>

#include "a11/flow/diagnostic.h"
#include "a11/flow/generate.h"
#include "a11/flow/highlight.h"
#include "a11/flow/lexer.h"
#include "a11/flow/parser.h"
#include "a11/flow/syntax.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow {
namespace {

nlohmann::json ImportJson(const syntax::Import& item) {
  nlohmann::json symbols = nlohmann::json::array();
  for (const auto& symbol : item.symbols) {
    symbols.push_back(
        {{"name", symbol.name.text}, {"alias", symbol.alias.text}});
  }
  nlohmann::json value{{"source", item.source},
                       {"alias", item.alias.text},
                       {"namespace", item.namespace_source},
                       {"exported", item.exported},
                       {"scope", item.scope}};
  if (item.selective) {
    value["selective"] = true;
    value["symbols"] = std::move(symbols);
  }
  return value;
}

nlohmann::json PositionToJson(const Position& position) {
  return nlohmann::json{{"offset", position.offset},
                        {"line", position.line},
                        {"column", position.column}};
}

Position PositionFromJson(const nlohmann::json& value) {
  Position position;
  if (!value.is_object()) {
    return position;
  }
  position.offset = value.value("offset", static_cast<size_t>(0));
  position.line = value.value("line", 1);
  position.column = value.value("column", 1);
  return position;
}

// SARIF has three levels and no notion of a weak warning, so the two shades of
// "this does nothing" both land on `note`: the severity field of the diagnostic
// keeps the distinction for readers that want it.
std::string_view SarifLevel(Severity severity) {
  switch (severity) {
    case Severity::kError:
      return "error";
    case Severity::kWarning:
      return "warning";
    case Severity::kWeakWarning:
    case Severity::kInformation:
      return "note";
  }
  return "error";
}

// --- The syntax tree ---------------------------------------------------------
// One function, one switch, one case per node kind, and the compiler holds it
// to being exhaustive -- which is what makes a node added to the.

nlohmann::json LocationJson(const syntax::Location& location) {
  return {{"start", location.start},
          {"end", location.end},
          {"line", location.line},
          {"column", location.column}};
}

nlohmann::json NodeJson(const syntax::Node* node);

nlohmann::json NodeList(const std::vector<syntax::NodePtr>& nodes) {
  nlohmann::json list = nlohmann::json::array();
  for (const syntax::NodePtr& node : nodes) {
    list.push_back(NodeJson(node.get()));
  }
  return list;
}

nlohmann::json WordList(const std::vector<syntax::Word>& words) {
  nlohmann::json list = nlohmann::json::array();
  for (const syntax::Word& word : words) {
    list.push_back(word.expression == nullptr
                       ? nlohmann::json(word.text)
                       : NodeJson(word.expression.get()));
  }
  return list;
}

/// The tail a `log`/`logf` statement and the stages of the same names share.
///
/// One shape in the envelope because it is one shape in the grammar: a reader
/// that can display a logged line does not have to know which of the two it
/// came from.
nlohmann::json LogTailJson(const syntax::LogTail& tail) {
  nlohmann::json value = nlohmann::json::object();
  value["level"] = tail.level.Empty() ? nlohmann::json(nullptr)
                                      : nlohmann::json(tail.level.text);
  value["format"] =
      tail.has_format ? nlohmann::json(tail.format) : nlohmann::json(nullptr);
  value["arguments"] = NodeList(tail.arguments);
  return value;
}

nlohmann::json DurationJson(const std::optional<absl::Duration>& duration) {
  if (!duration.has_value()) {
    return nullptr;
  }
  return nlohmann::json{{"$duration", absl::ToDoubleSeconds(*duration)}};
}

nlohmann::json TypeJson(const syntax::TypeExpression& type) {
  nlohmann::json parameters = nlohmann::json::array();
  for (const syntax::TypeExpression& parameter : type.parameters) {
    parameters.push_back(TypeJson(parameter));
  }
  return nlohmann::json{{"name", type.name},
                        {"parameters", parameters},
                        {"quoted", type.quoted},
                        {"sugared", type.sugared},
                        {"written", type.ToString()}};
}

/// A field's bounds. Absent keys are open ends, which is what `1..` means.
nlohmann::json RangeJson(const syntax::FieldRange& range) {
  nlohmann::json written = nlohmann::json::object();
  if (range.has_minimum) {
    written["minimum"] = ConstantToJsonValue(range.minimum);
  }
  if (range.has_maximum) {
    written["maximum"] = ConstantToJsonValue(range.maximum);
  }
  return written;
}

nlohmann::json PairsJson(
    const std::vector<std::pair<std::string, syntax::NodePtr>>& pairs) {
  nlohmann::json list = nlohmann::json::array();
  for (const auto& [key, value] : pairs) {
    list.push_back(nlohmann::json::array({key, NodeJson(value.get())}));
  }
  return list;
}

nlohmann::json NodeJson(const syntax::Node* node) {
  if (node == nullptr) {
    return nullptr;
  }
  // The position goes under `at` rather than beside the node's own fields: a
  // `repeat` has a `start`, and a format where one key means two things is a
  // format somebody reads wrong exactly once.
  nlohmann::json value{
      {"kind", syntax::NodeKindName(node->kind)},
      {"at", nlohmann::json{{"start", node->location.start},
                            {"end", node->location.end},
                            {"line", node->location.line},
                            {"column", node->location.column}}},
  };
  switch (node->kind) {
    case syntax::NodeKind::kError:
      value["expected"] = syntax::As<syntax::ErrorNode>(node)->expected;
      break;
    case syntax::NodeKind::kLiteral:
      value["value"] =
          ConstantToJsonValue(syntax::As<syntax::Literal>(node)->value);
      break;
    case syntax::NodeKind::kListLiteral:
      value["items"] = NodeList(syntax::As<syntax::ListLiteral>(node)->items);
      break;
    case syntax::NodeKind::kObjectLiteral:
      value["pairs"] =
          PairsJson(syntax::As<syntax::ObjectLiteral>(node)->pairs);
      break;
    case syntax::NodeKind::kIt:
    case syntax::NodeKind::kDiscard:
      break;
    case syntax::NodeKind::kName:
      value["name"] = syntax::As<syntax::Name>(node)->name;
      break;
    case syntax::NodeKind::kAttr: {
      const auto* attr = syntax::As<syntax::Attr>(node);
      value["base"] = NodeJson(attr->base.get());
      value["name"] = attr->name;
      break;
    }
    case syntax::NodeKind::kIndex: {
      const auto* index = syntax::As<syntax::Index>(node);
      value["base"] = NodeJson(index->base.get());
      value["index"] = NodeJson(index->index.get());
      break;
    }
    case syntax::NodeKind::kBuiltin: {
      const auto* builtin = syntax::As<syntax::Builtin>(node);
      value["name"] = builtin->name;
      value["args"] = NodeList(builtin->args);
      break;
    }
    case syntax::NodeKind::kTypedValue: {
      const auto* typed = syntax::As<syntax::TypedValue>(node);
      value["type"] = TypeJson(typed->type);
      value["value"] = NodeJson(typed->value.get());
      break;
    }
    case syntax::NodeKind::kUnary: {
      const auto* unary = syntax::As<syntax::Unary>(node);
      value["op"] = unary->op;
      value["operand"] = NodeJson(unary->operand.get());
      break;
    }
    case syntax::NodeKind::kBinary: {
      const auto* binary = syntax::As<syntax::Binary>(node);
      value["op"] = binary->op;
      value["left"] = NodeJson(binary->left.get());
      value["right"] = NodeJson(binary->right.get());
      break;
    }
    case syntax::NodeKind::kStage: {
      const auto* stage = syntax::As<syntax::Stage>(node);
      value["name"] = stage->name;
      value["takes"] = vocabulary::StageArgumentName(stage->takes);
      if (syntax::As<syntax::CallExpression>(stage->argument.get()) !=
          nullptr) {
        value["invocation"] = NodeJson(stage->argument.get());
      }
      switch (stage->takes) {
        case vocabulary::StageArgument::kNumber:
          value["arg"] = stage->is_integer ? nlohmann::json(stage->integer)
                                           : nlohmann::json(stage->number);
          break;
        case vocabulary::StageArgument::kString:
        case vocabulary::StageArgument::kOptionalString:
          value["arg"] = stage->text;
          break;
        case vocabulary::StageArgument::kExpression:
        case vocabulary::StageArgument::kStream:
          value["arg"] = NodeJson(stage->argument.get());
          break;
        case vocabulary::StageArgument::kNone:
          value["arg"] = nullptr;
          break;
        case vocabulary::StageArgument::kLog:
        case vocabulary::StageArgument::kLogFormat:
          value["arg"] = LogTailJson(stage->log);
          break;
        case vocabulary::StageArgument::kOptionalExpression:
          value["arg"] = stage->argument == nullptr
                             ? nlohmann::json(nullptr)
                             : NodeJson(stage->argument.get());
          break;
        case vocabulary::StageArgument::kSortKey:
          value["arg"] = stage->argument == nullptr
                             ? nlohmann::json(nullptr)
                             : NodeJson(stage->argument.get());
          value["desc"] = stage->descending;
          break;
        case vocabulary::StageArgument::kFold:
          value["arg"] = NodeJson(stage->argument.get());
          value["start"] = ConstantToJsonValue(stage->start);
          value["carried"] = stage->carried.text;
          break;
        case vocabulary::StageArgument::kDuration:
          value["arg"] = absl::FormatDuration(stage->duration);
          break;
      }
      // The tail a stage may carry, always written so a frontend can read one
      // shape: a plugin asking what a stage does should not have to know which
      // of them may be tried, run wide, or routed.
      value["try"] = stage->tolerant;
      value["parallel"] = stage->parallel;
      if (stage->parallel_expression != nullptr) {
        value["parallel_expression"] =
            NodeJson(stage->parallel_expression.get());
      }
      value["ordered"] = stage->ordered;
      value["into"] = stage->failures == nullptr
                          ? nlohmann::json(nullptr)
                          : NodeJson(stage->failures.get());
      break;
    }
    case syntax::NodeKind::kPipeline: {
      const auto* pipeline = syntax::As<syntax::Pipeline>(node);
      value["source"] = NodeJson(pipeline->source.get());
      nlohmann::json stages = nlohmann::json::array();
      for (const syntax::StagePtr& stage : pipeline->stages) {
        stages.push_back(NodeJson(stage.get()));
      }
      value["stages"] = stages;
      break;
    }
    case syntax::NodeKind::kOutcome:
      value["subject"] =
          NodeJson(syntax::As<syntax::Outcome>(node)->subject.get());
      break;
    case syntax::NodeKind::kPipelineValue:
      value["take"] = syntax::As<syntax::PipelineValue>(node)->take;
      value["optional"] = syntax::As<syntax::PipelineValue>(node)->optional;
      value["pipeline"] =
          NodeJson(syntax::As<syntax::PipelineValue>(node)->pipeline.get());
      break;
    case syntax::NodeKind::kCallModifiers: {
      const auto* modifiers = syntax::As<syntax::CallModifiers>(node);
      value["tee"] = modifiers->tee;
      value["node_map"] = modifiers->node_map.Empty()
                              ? nlohmann::json(nullptr)
                              : nlohmann::json(modifiers->node_map.text);
      value["timeout"] = DurationJson(modifiers->timeout);
      value["deadline"] = NodeJson(modifiers->deadline.get());
      value["timeout_expression"] =
          NodeJson(modifiers->timeout_expression.get());
      value["after"] = WordList(modifiers->after);
      value["headers"] = PairsJson(modifiers->headers);
      value["action_id"] = NodeJson(modifiers->action_id.get());
      value["forward"] = modifiers->forward;
      break;
    }
    case syntax::NodeKind::kCallExpression: {
      const auto* call = syntax::As<syntax::CallExpression>(node);
      value["action"] = call->action;
      if (call->invalid_target) {
        value["invalid_target"] = true;
        value["target_suffixes"] = nlohmann::json::array();
        for (const auto& suffix : call->target_suffixes) {
          value["target_suffixes"].push_back(
              {{"member", suffix.member},
               {"index", NodeJson(suffix.index.get())}});
        }
      }
      value["mode"] = call->mode;
      nlohmann::json args = nlohmann::json::array();
      for (const syntax::CallExpression::Argument& argument : call->args) {
        args.push_back(nlohmann::json::array(
            {argument.port.text, NodeJson(argument.pipeline.get())}));
      }
      value["args"] = args;
      value["modifiers"] = NodeJson(call->modifiers.get());
      value["tolerant"] = call->tolerant;
      break;
    }
    case syntax::NodeKind::kBind: {
      const auto* bind = syntax::As<syntax::Bind>(node);
      value["name"] = bind->name.text;
      value["names"] = WordList(bind->names);
      value["value"] = NodeJson(bind->value.get());
      break;
    }
    case syntax::NodeKind::kBlock: {
      const auto* block = syntax::As<syntax::Block>(node);
      value["tolerant"] = block->tolerant;
      if (!block->after.empty()) {
        value["after"] = WordList(block->after);
      }
      value["body"] = NodeList(block->body);
      break;
    }
    case syntax::NodeKind::kCallStatement:
      value["call"] =
          NodeJson(syntax::As<syntax::CallStatement>(node)->call.get());
      break;
    case syntax::NodeKind::kPipe: {
      const auto* pipe = syntax::As<syntax::Pipe>(node);
      value["pipeline"] = NodeJson(pipe->pipeline.get());
      value["targets"] = NodeList(pipe->targets);
      value["result_category"] = "continuation-sink";
      value["associativity"] = "right";
      value["after"] = WordList(pipe->after);
      break;
    }
    case syntax::NodeKind::kSkip: {
      const auto* skip = syntax::As<syntax::Skip>(node);
      nlohmann::json targets = nlohmann::json::array();
      for (const syntax::SkipTarget& target : skip->targets) {
        nlohmann::json one = nlohmann::json::object();
        one["pipeline"] = NodeJson(target.pipeline.get());
        one["call"] = target.call.Empty() ? nlohmann::json(nullptr)
                                          : nlohmann::json(target.call.text);
        one["outputs"] = WordList(target.outputs);
        targets.push_back(std::move(one));
      }
      value["targets"] = std::move(targets);
      value["after"] = WordList(skip->after);
      value["count"] = skip->count.has_value() ? nlohmann::json(*skip->count)
                                               : nlohmann::json(nullptr);
      break;
    }
    case syntax::NodeKind::kWait: {
      const auto* wait = syntax::As<syntax::Wait>(node);
      if (wait->scope_barrier) {
        value["scope_barrier"] = true;
      }
      value["subject"] = NodeJson(wait->subject.get());
      value["subjects"] = NodeList(wait->subjects);
      value["race"] = wait->race;
      value["targets"] = NodeList(wait->targets);
      value["timeout"] = DurationJson(wait->timeout);
      value["deadline"] = NodeJson(wait->deadline.get());
      value["timeout_expression"] = NodeJson(wait->timeout_expression.get());
      value["after"] = WordList(wait->after);
      break;
    }
    case syntax::NodeKind::kClose: {
      const auto* drain = syntax::As<syntax::Close>(node);
      value["target"] = NodeJson(drain->target.get());
      value["after"] = WordList(drain->after);
      break;
    }
    case syntax::NodeKind::kCancel: {
      const auto* cancel = syntax::As<syntax::Cancel>(node);
      value["name"] = cancel->name.text;
      value["after"] = WordList(cancel->after);
      break;
    }
    case syntax::NodeKind::kAbort: {
      const auto* abort = syntax::As<syntax::Abort>(node);
      value["target"] = NodeJson(abort->target.get());
      value["code"] = NodeJson(abort->code.get());
      value["message"] = NodeJson(abort->message.get());
      value["after"] = WordList(abort->after);
      break;
    }
    case syntax::NodeKind::kFail: {
      const auto* fail = syntax::As<syntax::Fail>(node);
      value["code"] = NodeJson(fail->code.get());
      value["message"] = NodeJson(fail->message.get());
      value["after"] = WordList(fail->after);
      break;
    }
    case syntax::NodeKind::kLog: {
      const auto* log = syntax::As<syntax::Log>(node);
      value["log"] = LogTailJson(log->tail);
      value["after"] = WordList(log->after);
      break;
    }
    case syntax::NodeKind::kForEach: {
      const auto* loop = syntax::As<syntax::ForEach>(node);
      nlohmann::json names = nlohmann::json::array();
      for (const syntax::Word& name : loop->variables) {
        names.push_back(name.text);
      }
      value["variable"] = loop->variable().text;
      value["variables"] = std::move(names);
      value["pipeline"] = NodeJson(loop->pipeline.get());
      value["parallel"] = loop->parallel;
      if (loop->parallel_expression != nullptr) {
        value["parallel_expression"] =
            NodeJson(loop->parallel_expression.get());
      }
      value["termination"] = NodeJson(loop->termination.get());
      value["body"] = NodeList(loop->body);
      value["after"] = WordList(loop->after);
      break;
    }
    case syntax::NodeKind::kRepeat: {
      const auto* repeat = syntax::As<syntax::Repeat>(node);
      value["variable"] = repeat->variable.Empty()
                              ? nlohmann::json(nullptr)
                              : nlohmann::json(repeat->variable.text);
      value["start"] = NodeJson(repeat->start.get());
      value["termination"] = NodeJson(repeat->termination.get());
      if (repeat->max_iterations.has_value()) {
        value["max_iterations"] = *repeat->max_iterations;
      }
      value["body"] = NodeList(repeat->body);
      value["after"] = WordList(repeat->after);
      break;
    }
    case syntax::NodeKind::kCarry: {
      const auto* carry = syntax::As<syntax::Carry>(node);
      value["name"] = carry->name.text;
      value["pipeline"] = NodeJson(carry->pipeline.get());
      break;
    }
    case syntax::NodeKind::kUntil: {
      const auto* until = syntax::As<syntax::Until>(node);
      value["condition"] = NodeJson(until->condition.get());
      value["stop_when"] = until->stop_when;
      break;
    }
    case syntax::NodeKind::kIf: {
      const auto* branch = syntax::As<syntax::If>(node);
      value["condition"] = NodeJson(branch->condition.get());
      if (!branch->binding.Empty()) {
        value["optional_binding"] = branch->binding.text;
        value["binding_scope"] = "success-branch";
        value["condition_evaluation"] = "once";
      }
      value["then_body"] = NodeList(branch->then_body);
      value["else_body"] = NodeList(branch->else_body);
      break;
    }
    case syntax::NodeKind::kNodes: {
      const auto* nodes = syntax::As<syntax::Nodes>(node);
      value["name"] = nodes->name.text;
      value["body"] = NodeList(nodes->body);
      break;
    }
    case syntax::NodeKind::kNodeExpression: {
      const auto* made = syntax::As<syntax::NodeExpression>(node);
      value["id"] = NodeJson(made->id.get());
      value["node_map"] = made->node_map.Empty()
                              ? nlohmann::json(nullptr)
                              : nlohmann::json(made->node_map.text);
      break;
    }
    case syntax::NodeKind::kPortDeclaration: {
      const auto* port = syntax::As<syntax::PortDeclaration>(node);
      value["name"] = port->name.text;
      value["direction"] = syntax::PortDirectionName(port->direction);
      value["type"] = TypeJson(port->type);
      value["unary"] = port->unary;
      value["required"] = port->required;
      value["description"] = port->description;
      break;
    }
    case syntax::NodeKind::kHeaderDeclaration: {
      const auto* header = syntax::As<syntax::HeaderDeclaration>(node);
      value["name"] = header->name;
      value["alias"] = header->alias.text;
      value["default"] = header->has_default
                             ? ConstantToJsonValue(header->default_value)
                             : nlohmann::json(nullptr);
      value["description"] = header->description;
      break;
    }
    case syntax::NodeKind::kFlowDeclaration: {
      const auto* flow = syntax::As<syntax::FlowDeclaration>(node);
      value["name"] = flow->name.text;
      // Emitted even though the empty name implies it: a reader of this JSON
      // should not have to know that "no name" is how an entry point is
      // spelled.
      value["entry"] = flow->entry;
      value["declaration"] = flow->interface_only;
      value["exported"] = flow->exported;
      value["description"] = flow->description;
      nlohmann::json ports = nlohmann::json::array();
      for (const syntax::PortDeclarationPtr& port : flow->ports) {
        ports.push_back(NodeJson(port.get()));
      }
      value["ports"] = ports;
      nlohmann::json headers = nlohmann::json::array();
      for (const syntax::HeaderDeclarationPtr& header : flow->headers) {
        headers.push_back(NodeJson(header.get()));
      }
      value["headers"] = headers;
      if (!flow->declarations.empty()) {
        value["declarations"] = nlohmann::json::array();
      }
      for (const auto& declaration : flow->declarations) {
        value["declarations"].push_back(NodeJson(declaration.get()));
      }
      if (!flow->structs.empty()) {
        value["structs"] = nlohmann::json::array();
      }
      for (const auto& shape : flow->structs) {
        value["structs"].push_back(NodeJson(shape.get()));
      }
      if (!flow->imports.empty()) {
        value["imports"] = nlohmann::json::array();
        for (const auto& item : flow->imports) {
          value["imports"].push_back(ImportJson(item));
        }
      }
      value["body"] = NodeList(flow->body);
      break;
    }
    case syntax::NodeKind::kSpread:
      value["value"] = NodeJson(syntax::As<syntax::Spread>(node)->value.get());
      break;
    case syntax::NodeKind::kZip:
      value["sources"] = NodeList(syntax::As<syntax::Zip>(node)->sources);
      break;
    case syntax::NodeKind::kFieldDeclaration: {
      const auto* field = syntax::As<syntax::FieldDeclaration>(node);
      value["name"] = field->name.text;
      value["type"] = TypeJson(field->type);
      value["required"] = field->required;
      value["unique"] = field->unique;
      value["description"] = field->description;
      if (!field->range.Empty()) {
        value["range"] = RangeJson(field->range);
      }
      if (field->has_pattern) {
        value["pattern"] = field->pattern;
      }
      if (field->has_enumeration) {
        nlohmann::json allowed = nlohmann::json::array();
        for (const syntax::Constant& one : field->enumeration) {
          allowed.push_back(ConstantToJsonValue(one));
        }
        value["one_of"] = allowed;
      }
      if (field->has_default) {
        value["default"] = ConstantToJsonValue(field->default_value);
      }
      break;
    }
    case syntax::NodeKind::kDtoDeclaration: {
      const auto* dto = syntax::As<syntax::DtoDeclaration>(node);
      value["exported"] = dto->exported;
      value["name"] = dto->name.text;
      value["description"] = dto->description;
      nlohmann::json fields = nlohmann::json::array();
      for (const syntax::FieldDeclarationPtr& field : dto->fields) {
        fields.push_back(NodeJson(field.get()));
      }
      value["fields"] = fields;
      break;
    }
  }
  return value;
}

}  // namespace

nlohmann::json DiagnosticToJsonValue(const Diagnostic& diagnostic) {
  nlohmann::json fixes = nlohmann::json::array();
  for (const Fix& fix : diagnostic.fixes) {
    nlohmann::json edits = nlohmann::json::array();
    for (const Edit& edit : fix.edits) {
      edits.push_back(nlohmann::json{
          {"start", edit.start}, {"end", edit.end}, {"text", edit.text}});
    }
    nlohmann::json written{{"label", fix.label}, {"edits", edits}};
    if (!fix.automatic) {
      written["automatic"] = false;
    }
    fixes.push_back(std::move(written));
  }
  nlohmann::json value{
      {"code", diagnostic.code},
      {"severity", SeverityName(diagnostic.severity)},
      {"family", FamilyName(diagnostic.family)},
      {"message", diagnostic.message},
      {"range",
       nlohmann::json{{"start", PositionToJson(diagnostic.range.start)},
                      {"end", PositionToJson(diagnostic.range.end)}}},
      {"fixes", fixes},
  };
  // Absent rather than empty: the flow is unknown when the text did not get far
  // enough to name one, and "" would read as a flow called nothing.
  if (!diagnostic.source.empty()) {
    value["source"] = diagnostic.source;
  }
  if (!diagnostic.flow.empty()) {
    value["flow"] = diagnostic.flow;
  }
  if (!diagnostic.related.empty()) {
    value["related"] = nlohmann::json::array();
    for (const auto& related : diagnostic.related) {
      value["related"].push_back(
          {{"source", related.source},
           {"range",
            {{"start", PositionToJson(related.range.start)},
             {"end", PositionToJson(related.range.end)}}},
           {"message", related.message}});
    }
  }
  return value;
}

Diagnostic DiagnosticFromJsonValue(const nlohmann::json& value) {
  Diagnostic diagnostic;
  if (!value.is_object()) {
    return diagnostic;
  }
  diagnostic.source = value.value("source", std::string());
  diagnostic.code = value.value("code", std::string());
  diagnostic.severity =
      SeverityFromName(value.value("severity", std::string("error")));
  diagnostic.family =
      FamilyFromName(value.value("family", std::string("syntax")));
  diagnostic.message = value.value("message", std::string());
  diagnostic.flow = value.value("flow", std::string());
  const auto range = value.find("range");
  if (range != value.end() && range->is_object()) {
    diagnostic.range.start =
        PositionFromJson(range->value("start", nlohmann::json()));
    diagnostic.range.end =
        PositionFromJson(range->value("end", nlohmann::json()));
  }
  if (const auto related = value.find("related");
      related != value.end() && related->is_array()) {
    for (const auto& one : *related) {
      if (!one.is_object()) {
        continue;
      }
      RelatedDiagnostic location;
      location.source = one.value("source", std::string());
      location.message = one.value("message", std::string());
      if (const auto span = one.find("range");
          span != one.end() && span->is_object()) {
        location.range.start =
            PositionFromJson(span->value("start", nlohmann::json()));
        location.range.end =
            PositionFromJson(span->value("end", nlohmann::json()));
      }
      diagnostic.related.push_back(std::move(location));
    }
  }
  const auto fixes = value.find("fixes");
  if (fixes != value.end() && fixes->is_array()) {
    for (const nlohmann::json& entry : *fixes) {
      if (!entry.is_object()) {
        continue;
      }
      Fix fix;
      fix.automatic = entry.value("automatic", true);
      fix.label = entry.value("label", std::string());
      const auto edits = entry.find("edits");
      if (edits != entry.end() && edits->is_array()) {
        for (const nlohmann::json& edit : *edits) {
          if (!edit.is_object()) {
            continue;
          }
          Edit parsed;
          parsed.start = edit.value("start", static_cast<size_t>(0));
          parsed.end = edit.value("end", parsed.start);
          parsed.text = edit.value("text", std::string());
          fix.edits.push_back(std::move(parsed));
        }
      }
      diagnostic.fixes.push_back(std::move(fix));
    }
  }
  return diagnostic;
}

nlohmann::json DiagnosticsToJsonValue(
    std::string_view source, absl::Span<const Diagnostic> diagnostics) {
  nlohmann::json list = nlohmann::json::array();
  size_t errors = 0;
  size_t warnings = 0;
  size_t weak = 0;
  size_t information = 0;
  for (const Diagnostic& diagnostic : diagnostics) {
    list.push_back(DiagnosticToJsonValue(diagnostic));
    switch (diagnostic.severity) {
      case Severity::kError:
        ++errors;
        break;
      case Severity::kWarning:
        ++warnings;
        break;
      case Severity::kWeakWarning:
        ++weak;
        break;
      case Severity::kInformation:
        ++information;
        break;
    }
  }
  return nlohmann::json{
      {"format", kDiagnosticsFormat},
      {"source", std::string(source)},
      {"diagnostics", list},
      {"counts", nlohmann::json{{"error", errors},
                                {"warning", warnings},
                                {"weak-warning", weak},
                                {"information", information}}},
  };
}

std::string DiagnosticsToJson(std::string_view source,
                              absl::Span<const Diagnostic> diagnostics) {
  return absl::StrCat(DiagnosticsToJsonValue(source, diagnostics).dump(2),
                      "\n");
}

nlohmann::json CodesToJsonValue() {
  nlohmann::json list = nlohmann::json::array();
  for (const CodeInfo& info : KnownCodes()) {
    list.push_back(nlohmann::json{{"code", info.code},
                                  {"family", FamilyName(info.family)},
                                  {"severity", SeverityName(info.severity)},
                                  {"summary", info.summary}});
  }
  return nlohmann::json{{"format", kCodesFormat}, {"codes", list}};
}

nlohmann::json VocabularyToJsonValue() {
  // Ordered where the language has an order for the words and sorted where it
  // does not, so the payload is stable either way: a generated grammar file
  // must not change because a hash table was rebuilt.
  const auto ordered = [](absl::Span<const std::string_view> words) {
    nlohmann::json list = nlohmann::json::array();
    for (const std::string_view word : words) {
      list.push_back(word);
    }
    return list;
  };
  const auto sorted = [](const auto& words) {
    std::vector<std::string_view> out(words.begin(), words.end());
    std::sort(out.begin(), out.end());
    nlohmann::json list = nlohmann::json::array();
    for (const std::string_view word : out) {
      list.push_back(word);
    }
    return list;
  };
  nlohmann::json stages = nlohmann::json::object();
  for (const std::string_view stage : vocabulary::Stages()) {
    stages[std::string(stage)] =
        vocabulary::StageArgumentName(*vocabulary::StageTakes(stage));
  }
  // What each word *does*, beside the lists of which words there are.
  nlohmann::json documentation = nlohmann::json::object();
  for (const vocabulary::WordRole role : vocabulary::WordRoles()) {
    nlohmann::json words = nlohmann::json::object();
    for (const std::string_view word : vocabulary::WordsOf(role)) {
      // The role's own table first, then whichever documents the word: a set
      // may list a word another set documents, and `AnyDocumentation` says
      // which cases those are.
      const vocabulary::WordDoc* doc = vocabulary::Documentation(role, word);
      if (doc == nullptr) {
        doc = vocabulary::AnyDocumentation(word);
      }
      if (doc == nullptr) {
        continue;
      }
      nlohmann::json entry{{"summary", doc->summary}};
      if (!doc->takes.empty()) {
        entry["takes"] = doc->takes;
      }
      if (!doc->detail.empty()) {
        entry["detail"] = doc->detail;
      }
      if (!doc->example.empty()) {
        entry["example"] = doc->example;
      }
      words[std::string(word)] = std::move(entry);
    }
    documentation[std::string(vocabulary::WordRoleName(role))] =
        std::move(words);
  }

  return nlohmann::json{
      {"format", kVocabularyFormat},
      {"fragment_pattern", FragmentStartPattern()},
      {"stages", ordered(vocabulary::Stages())},
      {"stage_arguments", stages},
      {"bare_stages", sorted(vocabulary::BareStages())},
      {"reducing_stages", sorted(vocabulary::ReducingStages())},
      {"positional_stages", sorted(vocabulary::PositionalStages())},
      {"builtins", ordered(vocabulary::OrderedBuiltins())},
      {"types", ordered(vocabulary::OrderedTypeNames())},
      {"statements", ordered(vocabulary::OrderedStatements())},
      {"clause_words", ordered(vocabulary::OrderedClauseWords())},
      {"declarations", ordered(vocabulary::OrderedDeclarations())},
      {"modifiers", ordered(vocabulary::OrderedModifiers())},
      {"sources", sorted(vocabulary::SourceWords())},
      {"port_modifiers", ordered(vocabulary::OrderedPortModifiers())},
      {"status_codes", ordered(vocabulary::StatusCodes())},
      {"status_fields", ordered(vocabulary::OrderedStatusFields())},
      {"constants", sorted(vocabulary::ConstantWords())},
      {"operators", sorted(vocabulary::OperatorWords())},
      {"duration_units", ordered(vocabulary::DurationUnits())},
      {"field_modifiers", ordered(vocabulary::OrderedFieldModifiers())},
      {"symbols", ordered(vocabulary::OrderedSymbols())},
      {"documentation", std::move(documentation)},
  };
}

nlohmann::json DiagnosticsToSarifValue(
    std::string_view source, absl::Span<const Diagnostic> diagnostics) {
  nlohmann::json rules = nlohmann::json::array();
  for (const CodeInfo& info : KnownCodes()) {
    rules.push_back(nlohmann::json{
        {"id", info.code},
        {"name", info.code},
        {"shortDescription", nlohmann::json{{"text", info.summary}}},
        {"defaultConfiguration",
         nlohmann::json{{"level", SarifLevel(info.severity)}}},
        {"properties",
         nlohmann::json{{"family", FamilyName(info.family)},
                        {"tags", nlohmann::json::array({"a11-flow"})}}},
    });
  }

  nlohmann::json results = nlohmann::json::array();
  for (const Diagnostic& diagnostic : diagnostics) {
    results.push_back(nlohmann::json{
        {"ruleId", diagnostic.code},
        {"level", SarifLevel(diagnostic.severity)},
        {"message", nlohmann::json{{"text", diagnostic.message}}},
        {
            "locations",
            nlohmann::json::array({nlohmann::json{
                {"physicalLocation",
                 nlohmann::json{
                     {"artifactLocation",
                      nlohmann::json{{"uri", std::string(source)}}},
                     // SARIF regions are 1-based lines and columns, with the
                     // end column exclusive -- the same convention as the range
                     // here.
                     {"region",
                      nlohmann::json{
                          {"startLine", diagnostic.range.start.line},
                          {"startColumn", diagnostic.range.start.column},
                          {"endLine", diagnostic.range.end.line},
                          {"endColumn", diagnostic.range.end.column},
                          {"charOffset", diagnostic.range.start.offset},
                          {"charLength",
                           diagnostic.range.end.offset -
                               diagnostic.range.start.offset}}}}}}}),
        },
    });
  }

  return nlohmann::json{
      {"$schema", "https://json.schemastore.org/sarif-2.1.0.json"},
      {"version", "2.1.0"},
      {"runs",
       nlohmann::json::array({nlohmann::json{
           {"tool",
            nlohmann::json{
                {"driver", nlohmann::json{{"name", "a11 flow"},
                                          {"informationUri",
                                           "https://github.com/hpnkv/a11"},
                                          {"rules", rules}}}}},
           {"results", results}}})},
  };
}

std::string DiagnosticsToSarif(std::string_view source,
                               absl::Span<const Diagnostic> diagnostics) {
  return absl::StrCat(DiagnosticsToSarifValue(source, diagnostics).dump(2),
                      "\n");
}

nlohmann::json ConstantToJsonValue(const syntax::Constant& constant) {
  switch (constant.kind) {
    case syntax::Constant::Kind::kNull:
      return nullptr;
    case syntax::Constant::Kind::kBool:
      return constant.boolean;
    case syntax::Constant::Kind::kInteger:
      return constant.integer;
    case syntax::Constant::Kind::kDouble:
      return constant.number;
    case syntax::Constant::Kind::kString:
      return constant.text;
    case syntax::Constant::Kind::kDuration:
      // Tagged rather than a bare number: `250ms` and `0.25` are different
      // things, and a reader of this format should not have to guess which.
      return nlohmann::json{
          {"$duration", absl::ToDoubleSeconds(constant.duration)}};
    case syntax::Constant::Kind::kList: {
      nlohmann::json items = nlohmann::json::array();
      for (const syntax::Constant& item : constant.items) {
        items.push_back(ConstantToJsonValue(item));
      }
      return items;
    }
    case syntax::Constant::Kind::kObject: {
      nlohmann::json pairs = nlohmann::json::array();
      for (const auto& [key, value] : constant.pairs) {
        pairs.push_back(
            nlohmann::json::array({key, ConstantToJsonValue(value)}));
      }
      return nlohmann::json{{"$object", pairs}};
    }
  }
  return nullptr;
}

nlohmann::json NodeToJsonValue(const syntax::Node& node) {
  return NodeJson(&node);
}

nlohmann::json SyntaxToJsonValue(std::string_view source,
                                 const ParseResult& result) {
  nlohmann::json flows = nlohmann::json::array();
  for (const syntax::FlowDeclarationPtr& flow : result.flows) {
    flows.push_back(NodeJson(flow.get()));
  }
  nlohmann::json dtos = nlohmann::json::array();
  for (const syntax::DtoDeclarationPtr& dto : result.dtos) {
    dtos.push_back(NodeJson(dto.get()));
  }
  nlohmann::json diagnostics = nlohmann::json::array();
  for (const Diagnostic& diagnostic : result.diagnostics) {
    diagnostics.push_back(DiagnosticToJsonValue(diagnostic));
  }
  nlohmann::json imports = nlohmann::json::array();
  for (const auto& item : result.imports) {
    imports.push_back(ImportJson(item));
  }
  nlohmann::json modules = nlohmann::json::array();
  for (const auto& module : result.modules) {
    modules.push_back({{"name", module.name.text},
                       {"declaration", module.interface_only},
                       {"exported", module.exported},
                       {"at", LocationJson(module.extent)}});
  }
  nlohmann::json exports = nlohmann::json::array();
  for (const auto& symbol : result.exports) {
    exports.push_back({{"symbol", symbol.symbol.text},
                       {"alias", symbol.alias.text},
                       {"scope", symbol.scope},
                       {"at", LocationJson(symbol.extent)}});
  }
  return nlohmann::json{
      {"format", kSyntaxFormat}, {"source", std::string(source)},
      {"flows", flows},          {"imports", imports},
      {"modules", modules},      {"exports", exports},
      {"structs", dtos},         {"diagnostics", diagnostics},
  };
}

std::string SyntaxToJson(std::string_view source, const ParseResult& result) {
  return absl::StrCat(SyntaxToJsonValue(source, result).dump(2), "\n");
}

nlohmann::json TokensToJsonValue(std::string_view source_name,
                                 std::string_view source) {
  const LexResult lexed = Lex(source, LexOptions{.keep_comments = true});
  std::vector<SemanticToken> semantic = Highlight(lexed.tokens);
  // The one part of classification that needs name resolution: which
  // identifiers are ports of the flow they stand in. See [RefinePorts].
  RefinePorts(source, semantic);
  nlohmann::json tokens = nlohmann::json::array();
  size_t lexical = 0;
  for (size_t index = 0; index < semantic.size(); ++index) {
    const SemanticToken& token = semantic[index];
    nlohmann::json written{
        {"kind", SemanticKindName(token.kind)},
        {"start", token.start},
        {"end", token.end},
        {"line", token.line},
        {"column", token.column},
    };
    // The lexer's own name for it, beside the meaning: one call then serves a
    // client that colours and a client that has to tokenise.
    while (lexical + 1 < lexed.tokens.size() &&
           lexed.tokens[lexical].end <= token.start) {
      ++lexical;
    }
    if (lexical < lexed.tokens.size()) {
      written["lexical"] = KindName(lexed.tokens[lexical].kind);
    }
    tokens.push_back(std::move(written));
  }
  nlohmann::json diagnostics = nlohmann::json::array();
  for (const Diagnostic& diagnostic : lexed.diagnostics) {
    diagnostics.push_back(DiagnosticToJsonValue(diagnostic));
  }
  return nlohmann::json{
      {"format", kTokensFormat},
      {"source", std::string(source_name)},
      {"tokens", tokens},
      {"diagnostics", diagnostics},
  };
}

std::string TokensToJson(std::string_view source_name,
                         std::string_view source) {
  return absl::StrCat(TokensToJsonValue(source_name, source).dump(2), "\n");
}

nlohmann::json FormatToJsonValue(const FormatResult& result) {
  nlohmann::json edits = nlohmann::json::array();
  for (const Edit& edit : result.edits) {
    edits.push_back(nlohmann::json{
        {"start", edit.start}, {"end", edit.end}, {"text", edit.text}});
  }
  nlohmann::json diagnostics = nlohmann::json::array();
  for (const Diagnostic& diagnostic : result.diagnostics) {
    diagnostics.push_back(DiagnosticToJsonValue(diagnostic));
  }
  return nlohmann::json{
      {"format", kFormatFormat},    {"formatted", result.formatted},
      {"changed", result.changed},  {"edits", edits},
      {"diagnostics", diagnostics},
  };
}

nlohmann::json CompletionsToJsonValue(const CompleteResult& result) {
  nlohmann::json proposals = nlohmann::json::array();
  for (const Proposal& proposal : result.proposals) {
    nlohmann::json written{
        {"name", proposal.name},
        {"kind", ProposalKindName(proposal.kind)},
        {"insert", proposal.insert},
    };
    // Only what there is: a proposal with no tail, no type and a caret at the
    // end of what it writes is the common case, and saying so four times would
    // make the payload longer than the list it describes.
    if (proposal.caret >= 0) {
      written["caret"] = proposal.caret;
    }
    if (proposal.replace_start != std::string::npos) {
      written["replace_start"] = proposal.replace_start;
    }
    if (!proposal.filter.empty()) {
      written["filter"] = proposal.filter;
    }
    if (!proposal.tail.empty()) {
      written["tail"] = proposal.tail;
    }
    if (!proposal.type.empty()) {
      written["type"] = proposal.type;
    }
    if (!proposal.documentation.empty()) {
      written["documentation"] = proposal.documentation;
    }
    proposals.push_back(std::move(written));
  }
  return nlohmann::json{
      {"format", kCompletionsFormat},
      {"prefix", result.prefix},
      {"prefix_start", result.prefix_start},
      {"proposals", proposals},
  };
}

namespace {

nlohmann::json PortToJson(const PortPlan& port) {
  nlohmann::json value{
      {"type", port.declared.empty() ? port.type : port.declared},
      {"unary", port.unary},
      {"required", port.required},
      {"description", port.description},
  };
  if (!port.constraints.empty()) {
    value["constraints"] = port.constraints;
  }
  if (port.analysis_shape != nullptr) {
    value["analysis_shape"] = DtoToJsonValue(*port.analysis_shape);
  }
  return value;
}

/// One resolved shape, as `flow.plan/v1` and `flow.schema/v1` write it.
///
/// The constraints go out only when they were written, so a plain field is a
/// plain object and a reader can see at a glance what the author actually said.
nlohmann::json DtoToJson(const DtoPlan& dto) {
  nlohmann::json fields = nlohmann::json::object();
  nlohmann::json order = nlohmann::json::array();
  for (const FieldPlan& field : dto.fields) {
    nlohmann::json written{
        {"type", field.declared.empty() ? field.type : field.declared},
        {"resolved", field.type},
        {"required", field.required},
        {"description", field.description},
    };
    if (!field.element.empty()) {
      written["element"] = field.element;
    }
    if (!field.dto_name.empty()) {
      written["struct"] = field.dto_name;
    }
    if (!field.element_dto_name.empty()) {
      written["element_struct"] = field.element_dto_name;
    }
    if (field.unique) {
      written["unique"] = true;
    }
    if (!field.range.Empty()) {
      written["range"] = RangeJson(field.range);
    }
    if (field.has_pattern) {
      written["pattern"] = field.pattern;
    }
    if (field.has_enumeration) {
      nlohmann::json allowed = nlohmann::json::array();
      for (const syntax::Constant& one : field.enumeration) {
        allowed.push_back(ConstantToJsonValue(one));
      }
      written["one_of"] = allowed;
    }
    if (field.has_default) {
      written["default"] = ConstantToJsonValue(field.default_value);
    }
    fields[field.name] = std::move(written);
    order.push_back(field.name);
  }
  return nlohmann::json{
      {"struct", dto.name},
      {"description", dto.description},
      {"fields", fields},
      // The order is beside the fields rather than implied by them: a JSON
      // object has no order a reader may rely on, and a shape's fields have
      // one.
      {"order", order},
      {"binary", dto.binary},
  };
}

nlohmann::json StepsToJson(const std::vector<StepPlan>& steps);

nlohmann::json StepToJson(const StepPlan& step) {
  nlohmann::json written{{"step", step.kind}, {"label", step.label}};
  if (step.scope_barrier) {
    written["scope_barrier"] = true;
  }
  if (!step.after.empty()) {
    written["after"] = step.after;
  }
  if (!step.action.empty()) {
    written["action"] = step.action;
    written["mode"] = step.mode;
    if (!step.pipeline_input.empty()) {
      written["pipeline"] = {{"input", step.pipeline_input},
                             {"output", step.pipeline_output},
                             {"activation", "whole-stream"}};
    }
    if (step.tolerant) {
      written["try"] = true;
    }
    if (step.tee) {
      written["tee"] = true;
    }
  }
  if (!step.node_map.empty()) {
    written["via"] = step.node_map;
  }
  if (step.has_deadline) {
    written["deadline"] = true;
  }
  if (step.timeout.has_value()) {
    written["timeout"] = absl::ToDoubleSeconds(*step.timeout);
  }
  // `from`/`to` for a pipe, `of` for the statements that name one thing: the
  // words `a11.flow.plan` describes them with.
  if (!step.source.empty()) {
    written[step.destination.empty() ? "of" : "from"] = step.source;
  }
  if (!step.optional_binding.empty()) {
    written["optional_binding"] = step.optional_binding;
    written["binding_scope"] = "success-branch";
    written["condition_evaluation"] = "once";
  }
  if (!step.serial_sources.empty()) {
    written["serial_sources"] = step.serial_sources;
    written["selected_destinations"] = step.destinations;
    written["grouping"] = {{"segments", "serial"},
                           {"attachments", "interleaved"}};
    if (!step.continuation_parent.empty()) {
      written["continuation_parent"] = step.continuation_parent;
    }
    written["destination_close"] = "all-registered-writers";
    written["result_category"] = "continuation-sink";
    written["attachment_order"] = "interleaved-after-operation";
    written["completion"] = "operation-and-attached-writers";
  }
  if (!step.destination.empty()) {
    written["to"] = step.destination;
  }
  if (!step.bodies.empty()) {
    nlohmann::json bodies = nlohmann::json::array();
    for (const std::vector<StepPlan>& body : step.bodies) {
      bodies.push_back(StepsToJson(body));
    }
    // An `if` has two, in the order the language reads them; everything else
    // that nests has one.
    if (step.kind == "if") {
      written["then"] = bodies[0];
      if (bodies.size() > 1) {
        written["else"] = bodies[1];
      }
    } else {
      written["body"] = bodies[0];
    }
  }
  return written;
}

nlohmann::json StepsToJson(const std::vector<StepPlan>& steps) {
  nlohmann::json list = nlohmann::json::array();
  for (const StepPlan& step : steps) {
    list.push_back(StepToJson(step));
  }
  return list;
}

}  // namespace

nlohmann::json PlanToJsonValue(std::string_view source_name,
                               const Program& program) {
  nlohmann::json flows = nlohmann::json::array();
  for (const FlowPlan& flow : program.flows) {
    nlohmann::json inputs = nlohmann::json::object();
    nlohmann::json outputs = nlohmann::json::object();
    for (const PortPlan& port : flow.ports) {
      nlohmann::json& side =
          port.direction == syntax::PortDirection::kInput ? inputs : outputs;
      side[port.name] = PortToJson(port);
    }
    std::vector<std::string> headers;
    headers.reserve(flow.headers.size());
    for (const HeaderPlan& header : flow.headers) {
      headers.push_back(header.name);
    }
    std::sort(headers.begin(), headers.end());
    nlohmann::json header_schemas = nlohmann::json::object();
    for (const auto& header : flow.headers) {
      auto written = nlohmann::json{{"alias", header.alias},
                                    {"description", header.description}};
      if (header.has_default) {
        written["default"] = ConstantToJsonValue(header.default_value);
      }
      header_schemas[header.name] = std::move(written);
    }
    flows.push_back(nlohmann::json{
        {"flow", flow.name},
        {"entry", flow.entry},
        {"declaration", flow.interface_only},
        {"exported", flow.exported},
        {"description", flow.description},
        {"inputs", inputs},
        {"outputs", outputs},
        {"headers", headers},
        {"node_maps", flow.node_maps},
        {"steps", StepsToJson(flow.steps)},
    });
    if (!header_schemas.empty()) {
      flows.back()["header_schemas"] = std::move(header_schemas);
    }
  }
  nlohmann::json dtos = nlohmann::json::array();
  for (const DtoPlan& dto : program.dtos) {
    dtos.push_back(DtoToJson(dto));
  }
  return nlohmann::json{
      {"format", kPlanFormat},
      {"source", std::string(source_name)},
      {"flows", flows},
      {"structs", dtos},
  };
}

nlohmann::json DtoToJsonValue(const DtoPlan& dto,
                              const Program* absl_nullable program) {
  nlohmann::json out = DtoToJson(dto);
  if (program == nullptr) {
    return out;
  }
  // Breadth-first from this shape, skipping the ones already carried -- so a
  // cycle of shapes travels once and a shape that names itself is not repeated.
  std::vector<const DtoPlan*> found{&dto};
  absl::flat_hash_set<std::string> seen{dto.name};
  nlohmann::json nested = nlohmann::json::array();
  for (size_t index = 0; index < found.size(); ++index) {
    for (const FieldPlan& field : found[index]->fields) {
      for (const std::string& named :
           {field.dto_name, field.element_dto_name}) {
        if (named.empty() || !seen.insert(named).second) {
          continue;
        }
        const DtoPlan* next = program->Dto(named);
        if (next == nullptr) {
          continue;
        }
        found.push_back(next);
        nested.push_back(DtoToJson(*next));
      }
    }
  }
  if (!nested.empty()) {
    out["nested"] = std::move(nested);
  }
  return out;
}

std::string PlanToJson(std::string_view source_name, const Program& program) {
  return absl::StrCat(PlanToJsonValue(source_name, program).dump(2), "\n");
}

std::string DiagnosticToText(std::string_view source,
                             const Diagnostic& diagnostic) {
  std::string prefix;
  if (!diagnostic.source.empty()) {
    source = diagnostic.source;
  }
  if (!source.empty()) {
    absl::StrAppend(&prefix, source, ":");
  }
  return absl::StrCat(prefix, diagnostic.range.start.line, ":",
                      diagnostic.range.start.column, ": ",
                      SeverityName(diagnostic.severity), ": ",
                      diagnostic.message, " [", diagnostic.code, "]");
}

}  // namespace a11::flow
