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

std::optional<Constant> ConstantValue(const Node* node) {
  if (const Literal* literal = As<Literal>(node); literal != nullptr) {
    return literal->value;
  }
  if (const ListLiteral* list = As<ListLiteral>(node); list != nullptr) {
    Constant constant;
    constant.kind = Constant::Kind::kList;
    for (const NodePtr& item : list->items) {
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
      std::optional<Constant> value = ConstantValue(item.get());
      if (!value.has_value()) return std::nullopt;
      constant.pairs.emplace_back(key, *std::move(value));
    }
    return constant;
  }
  return std::nullopt;
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
