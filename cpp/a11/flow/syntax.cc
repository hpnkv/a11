// Copyright 2026 The A11 Authors.

#include "a11/flow/syntax.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>

namespace a11::flow::syntax {

Location LocationOf(const Token& token) {
  Location location;
  location.start = token.start;
  location.end = token.end;
  location.line = token.line;
  location.column = token.column;
  return location;
}

Constant Constant::Bool(bool value) {
  Constant constant;
  constant.kind = Kind::kBool;
  constant.boolean = value;
  return constant;
}

Constant Constant::Integer(long long value) {
  Constant constant;
  constant.kind = Kind::kInteger;
  constant.integer = value;
  constant.number = static_cast<double>(value);
  return constant;
}

Constant Constant::Double(double value) {
  Constant constant;
  constant.kind = Kind::kDouble;
  constant.number = value;
  return constant;
}

Constant Constant::String(std::string value) {
  Constant constant;
  constant.kind = Kind::kString;
  constant.text = std::move(value);
  return constant;
}

Constant Constant::Duration(absl::Duration value) {
  Constant constant;
  constant.kind = Kind::kDuration;
  constant.duration = value;
  return constant;
}

double Constant::AsDouble() const {
  switch (kind) {
    case Kind::kInteger:
      return static_cast<double>(integer);
    case Kind::kDouble:
      return number;
    case Kind::kBool:
      return boolean ? 1.0 : 0.0;
    default:
      return 0.0;
  }
}

std::string_view ConstantKindName(Constant::Kind kind) {
  switch (kind) {
    case Constant::Kind::kNull:
      return "null";
    case Constant::Kind::kBool:
      return "bool";
    case Constant::Kind::kInteger:
      return "integer";
    case Constant::Kind::kDouble:
      return "number";
    case Constant::Kind::kString:
      return "string";
    case Constant::Kind::kDuration:
      return "duration";
    case Constant::Kind::kList:
      return "list";
    case Constant::Kind::kObject:
      return "object";
  }
  return "null";
}

std::string TypeExpression::ToString() const {
  if (sugared && parameters.size() == 1) {
    return absl::StrCat(parameters.front().ToString(), "[]");
  }
  if (parameters.empty()) return name;
  std::vector<std::string> inside;
  inside.reserve(parameters.size());
  for (const TypeExpression& parameter : parameters) {
    inside.push_back(parameter.ToString());
  }
  return absl::StrCat(name, "[", absl::StrJoin(inside, ", "), "]");
}

std::string_view NodeKindName(NodeKind kind) {
  switch (kind) {
    case NodeKind::kError:
      return "error";
    case NodeKind::kLiteral:
      return "literal";
    case NodeKind::kListLiteral:
      return "list";
    case NodeKind::kObjectLiteral:
      return "object";
    case NodeKind::kSpread:
      return "spread";
    case NodeKind::kIt:
      return "it";
    case NodeKind::kName:
      return "name";
    case NodeKind::kAttr:
      return "attr";
    case NodeKind::kIndex:
      return "index";
    case NodeKind::kBuiltin:
      return "builtin";
    case NodeKind::kZip:
      return "zip";
    case NodeKind::kTypedValue:
      return "typed-value";
    case NodeKind::kUnary:
      return "unary";
    case NodeKind::kBinary:
      return "binary";
    case NodeKind::kStage:
      return "stage";
    case NodeKind::kPipeline:
      return "pipeline";
    case NodeKind::kOutcome:
      return "outcome";
    case NodeKind::kPipelineValue:
      return "pipeline-value";
    case NodeKind::kCallModifiers:
      return "call-modifiers";
    case NodeKind::kCallExpression:
      return "call";
    case NodeKind::kBind:
      return "bind";
    case NodeKind::kLet:
      return "let";
    case NodeKind::kAdvance:
      return "advance";
    case NodeKind::kBlock:
      return "block";
    case NodeKind::kCallStatement:
      return "call-statement";
    case NodeKind::kPipe:
      return "pipe";
    case NodeKind::kSkip:
      return "skip";
    case NodeKind::kWait:
      return "wait";
    case NodeKind::kDrain:
      return "drain";
    case NodeKind::kCancel:
      return "cancel";
    case NodeKind::kFail:
      return "fail";
    case NodeKind::kForEach:
      return "for-each";
    case NodeKind::kRepeat:
      return "repeat";
    case NodeKind::kCarry:
      return "carry";
    case NodeKind::kUntil:
      return "until";
    case NodeKind::kIf:
      return "if";
    case NodeKind::kNodes:
      return "nodes";
    case NodeKind::kNodeExpression:
      return "node";
    case NodeKind::kPortDeclaration:
      return "port";
    case NodeKind::kHeaderDeclaration:
      return "header";
    case NodeKind::kFlowDeclaration:
      return "flow";
    case NodeKind::kFieldDeclaration:
      return "field";
    case NodeKind::kDtoDeclaration:
      return "struct";
  }
  return "error";
}

bool IsAnyOf(const Node* node, std::initializer_list<NodeKind> kinds) {
  if (node == nullptr) return false;
  return std::find(kinds.begin(), kinds.end(), node->kind) != kinds.end();
}

std::string_view PortDirectionName(PortDirection direction) {
  return direction == PortDirection::kInput ? "inputs" : "outputs";
}

namespace {

/// Put `value` at `key`, replacing a pair already there.
///
/// What makes a later pair win over one a spread brought in, and what keeps the
/// result a mapping rather than a list of pairs with a duplicate in it.
void SetPair(Constant& object, std::string_view key, Constant value) {
  for (auto& [existing, held] : object.pairs) {
    if (existing == key) {
      held = std::move(value);
      return;
    }
  }
  object.pairs.emplace_back(std::string(key), std::move(value));
}

}  // namespace

std::optional<Constant> ConstantValue(const Node* node) {
  if (const Literal* literal = As<Literal>(node); literal != nullptr) {
    return literal->value;
  }
  if (const ListLiteral* list = As<ListLiteral>(node); list != nullptr) {
    Constant constant;
    constant.kind = Constant::Kind::kList;
    for (const NodePtr& item : list->items) {
      // A spread of a constant list is still a constant: the items are known
      // here, so splicing them is folding rather than running anything.
      if (const Spread* spread = As<Spread>(item.get()); spread != nullptr) {
        std::optional<Constant> held = ConstantValue(spread->value.get());
        if (!held.has_value() || held->kind != Constant::Kind::kList) {
          return std::nullopt;
        }
        for (Constant& inner : held->items) {
          constant.items.push_back(std::move(inner));
        }
        continue;
      }
      std::optional<Constant> value = ConstantValue(item.get());
      if (!value.has_value()) return std::nullopt;
      constant.items.push_back(*std::move(value));
    }
    return constant;
  }
  if (const ObjectLiteral* object = As<ObjectLiteral>(node);
      object != nullptr) {
    Constant constant;
    constant.kind = Constant::Kind::kObject;
    for (const auto& [key, item] : object->pairs) {
      if (const Spread* spread = As<Spread>(item.get()); spread != nullptr) {
        std::optional<Constant> held = ConstantValue(spread->value.get());
        if (!held.has_value() || held->kind != Constant::Kind::kObject) {
          return std::nullopt;
        }
        for (auto& [inner_key, inner] : held->pairs) {
          SetPair(constant, inner_key, std::move(inner));
        }
        continue;
      }
      std::optional<Constant> value = ConstantValue(item.get());
      if (!value.has_value()) return std::nullopt;
      SetPair(constant, key, *std::move(value));
    }
    return constant;
  }
  return std::nullopt;
}

void VisitChildren(const Node& node,
                   const std::function<void(const Node&)>& visit) {
  // One child, when there is one. Every field below is optional in practice --
  // the parser leaves a hole where it could not read a value.
  const auto one = [&](const NodePtr& child) {
    if (child != nullptr) visit(*child);
  };
  const auto all = [&](const std::vector<NodePtr>& children) {
    for (const NodePtr& child : children) one(child);
  };

  switch (node.kind) {
    case NodeKind::kError:
    case NodeKind::kLiteral:
    case NodeKind::kIt:
    case NodeKind::kName:
    case NodeKind::kCancel:
      return;
    case NodeKind::kListLiteral:
      all(static_cast<const ListLiteral&>(node).items);
      return;
    case NodeKind::kObjectLiteral:
      for (const auto& [key, value] :
           static_cast<const ObjectLiteral&>(node).pairs) {
        one(value);
      }
      return;
    case NodeKind::kSpread:
      one(static_cast<const Spread&>(node).value);
      return;
    case NodeKind::kAttr:
      one(static_cast<const Attr&>(node).base);
      return;
    case NodeKind::kIndex: {
      const auto& index = static_cast<const Index&>(node);
      one(index.base);
      one(index.index);
      return;
    }
    case NodeKind::kBuiltin:
      all(static_cast<const Builtin&>(node).args);
      return;
    case NodeKind::kZip:
      all(static_cast<const Zip&>(node).sources);
      return;
    case NodeKind::kTypedValue:
      one(static_cast<const TypedValue&>(node).value);
      return;
    case NodeKind::kUnary:
      one(static_cast<const Unary&>(node).operand);
      return;
    case NodeKind::kBinary: {
      const auto& binary = static_cast<const Binary&>(node);
      one(binary.left);
      one(binary.right);
      return;
    }
    case NodeKind::kStage:
      one(static_cast<const Stage&>(node).argument);
      return;
    case NodeKind::kPipeline: {
      const auto& pipeline = static_cast<const Pipeline&>(node);
      one(pipeline.source);
      for (const StagePtr& stage : pipeline.stages) {
        if (stage != nullptr) visit(*stage);
      }
      return;
    }
    case NodeKind::kOutcome:
      one(static_cast<const Outcome&>(node).subject);
      return;
    case NodeKind::kPipelineValue: {
      const PipelinePtr& pipeline =
          static_cast<const PipelineValue&>(node).pipeline;
      if (pipeline != nullptr) visit(*pipeline);
      return;
    }
    case NodeKind::kCallModifiers: {
      const auto& modifiers = static_cast<const CallModifiers&>(node);
      for (const auto& [name, value] : modifiers.headers) one(value);
      one(modifiers.action_id);
      return;
    }
    case NodeKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(node);
      for (const CallExpression::Argument& argument : call.args) {
        if (argument.pipeline != nullptr) visit(*argument.pipeline);
      }
      if (call.modifiers != nullptr) visit(*call.modifiers);
      return;
    }
    case NodeKind::kBind:
      one(static_cast<const Bind&>(node).value);
      return;
    case NodeKind::kLet: {
      const PipelinePtr& pipeline = static_cast<const Let&>(node).pipeline;
      if (pipeline != nullptr) visit(*pipeline);
      return;
    }
    case NodeKind::kCallStatement: {
      const CallExpressionPtr& call =
          static_cast<const CallStatement&>(node).call;
      if (call != nullptr) visit(*call);
      return;
    }
    case NodeKind::kPipe: {
      const auto& pipe = static_cast<const Pipe&>(node);
      if (pipe.pipeline != nullptr) visit(*pipe.pipeline);
      all(pipe.targets);
      return;
    }
    case NodeKind::kSkip: {
      for (const SkipTarget& target : static_cast<const Skip&>(node).targets) {
        if (target.pipeline != nullptr) visit(*target.pipeline);
      }
      return;
    }
    case NodeKind::kWait:
      one(static_cast<const Wait&>(node).subject);
      return;
    case NodeKind::kDrain:
      one(static_cast<const Drain&>(node).target);
      return;
    case NodeKind::kFail: {
      const auto& fail = static_cast<const Fail&>(node);
      one(fail.code);
      one(fail.message);
      return;
    }
    case NodeKind::kForEach: {
      const auto& loop = static_cast<const ForEach&>(node);
      if (loop.pipeline != nullptr) visit(*loop.pipeline);
      all(loop.body);
      return;
    }
    case NodeKind::kRepeat: {
      const auto& repeat = static_cast<const Repeat&>(node);
      one(repeat.start);
      all(repeat.body);
      return;
    }
    case NodeKind::kCarry: {
      const PipelinePtr& pipeline = static_cast<const Carry&>(node).pipeline;
      if (pipeline != nullptr) visit(*pipeline);
      return;
    }
    case NodeKind::kUntil:
      one(static_cast<const Until&>(node).condition);
      return;
    case NodeKind::kIf: {
      const auto& branch = static_cast<const If&>(node);
      one(branch.condition);
      all(branch.then_body);
      all(branch.else_body);
      return;
    }
    case NodeKind::kNodes:
      all(static_cast<const Nodes&>(node).body);
      return;
    case NodeKind::kNodeExpression:
      one(static_cast<const NodeExpression&>(node).id);
      return;
    case NodeKind::kPortDeclaration:
    case NodeKind::kHeaderDeclaration:
    case NodeKind::kFieldDeclaration:
      // What these hold is a type and a constant, neither of which is a node.
      return;
    case NodeKind::kFlowDeclaration: {
      const auto& flow = static_cast<const FlowDeclaration&>(node);
      for (const PortDeclarationPtr& port : flow.ports) {
        if (port != nullptr) visit(*port);
      }
      for (const HeaderDeclarationPtr& header : flow.headers) {
        if (header != nullptr) visit(*header);
      }
      all(flow.body);
      return;
    }
    case NodeKind::kDtoDeclaration:
      for (const FieldDeclarationPtr& field :
           static_cast<const DtoDeclaration&>(node).fields) {
        if (field != nullptr) visit(*field);
      }
      return;
  }
}

std::optional<std::string> DottedName(const Node* node) {
  std::vector<std::string> parts;
  while (const Attr* attr = As<Attr>(node)) {
    parts.push_back(attr->name);
    node = attr->base.get();
  }
  const Name* name = As<Name>(node);
  if (name == nullptr) return std::nullopt;
  parts.push_back(name->name);
  std::reverse(parts.begin(), parts.end());
  return absl::StrJoin(parts, ".");
}

}  // namespace a11::flow::syntax
