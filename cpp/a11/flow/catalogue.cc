// Copyright 2026 The A11 Authors.

#include "a11/flow/catalogue.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/strings/str_cat.h>
#include <nlohmann/json.hpp>

#include "a11/flow/catalogue_data.h"
#include "a11/flow/syntax.h"

namespace a11::flow::catalogue {
namespace {

using syntax::Constant;

std::string Text(const nlohmann::json& value, std::string_view key) {
  const auto found = value.find(key);
  if (found == value.end() || !found->is_string()) return "";
  return found->get<std::string>();
}

bool Flag(const nlohmann::json& value, std::string_view key, bool fallback) {
  const auto found = value.find(key);
  if (found == value.end() || !found->is_boolean()) return fallback;
  return found->get<bool>();
}

PortInfo PortFromJson(const nlohmann::json& value) {
  PortInfo port;
  port.name = Text(value, "name");
  port.type = Text(value, "type");
  port.description = Text(value, "description");
  port.required = Flag(value, "required", false);
  port.unary = Flag(value, "unary", true);
  return port;
}

std::vector<PortInfo> PortsFromJson(const nlohmann::json& value,
                                    std::string_view key) {
  std::vector<PortInfo> ports;
  const auto found = value.find(key);
  if (found == value.end() || !found->is_array()) return ports;
  for (const nlohmann::json& one : *found) {
    if (!one.is_object()) continue;
    PortInfo port = PortFromJson(one);
    if (port.name.empty()) continue;
    ports.push_back(std::move(port));
  }
  return ports;
}

nlohmann::json PortToJson(const PortInfo& port) {
  nlohmann::json value{{"name", port.name}, {"type", port.type}};
  if (!port.description.empty()) value["description"] = port.description;
  if (port.required) value["required"] = true;
  if (!port.unary) value["unary"] = false;
  return value;
}

/// Where an entry was declared, or `nullopt` where nothing said.
///
/// A file is what makes an origin worth having, so an entry naming a line and no
/// file is read as saying nothing rather than as pointing at line 12 of
/// somewhere.
std::optional<Origin> OriginFromJson(const nlohmann::json& value) {
  const auto found = value.find("origin");
  if (found == value.end() || !found->is_object()) return std::nullopt;
  Origin origin;
  origin.file = Text(*found, "file");
  if (origin.file.empty()) return std::nullopt;
  if (const auto line = found->find("line");
      line != found->end() && line->is_number_integer()) {
    origin.line = line->get<int>();
  }
  if (const auto column = found->find("column");
      column != found->end() && column->is_number_integer()) {
    origin.column = column->get<int>();
  }
  return origin;
}

nlohmann::json OriginToJson(const Origin& origin) {
  return nlohmann::json{
      {"file", origin.file}, {"line", origin.line}, {"column", origin.column}};
}

nlohmann::json PortsToJson(const std::vector<PortInfo>& ports) {
  nlohmann::json list = nlohmann::json::array();
  for (const PortInfo& port : ports) list.push_back(PortToJson(port));
  return list;
}

/// A constant from JSON, for a field's default and its allowed values.
Constant ConstantFromJson(const nlohmann::json& value) {
  if (value.is_boolean()) return Constant::Bool(value.get<bool>());
  if (value.is_number_integer()) {
    return Constant::Integer(value.get<long long>());
  }
  if (value.is_number()) return Constant::Double(value.get<double>());
  if (value.is_string()) return Constant::String(value.get<std::string>());
  if (value.is_array()) {
    Constant list;
    list.kind = Constant::Kind::kList;
    for (const nlohmann::json& item : value) {
      list.items.push_back(ConstantFromJson(item));
    }
    return list;
  }
  if (value.is_object()) {
    Constant object;
    object.kind = Constant::Kind::kObject;
    for (const auto& [key, held] : value.items()) {
      object.pairs.emplace_back(key, ConstantFromJson(held));
    }
    return object;
  }
  return Constant::Null();
}

nlohmann::json ConstantToJson(const Constant& value) {
  switch (value.kind) {
    case Constant::Kind::kNull:
      return nullptr;
    case Constant::Kind::kBool:
      return value.boolean;
    case Constant::Kind::kInteger:
      return value.integer;
    case Constant::Kind::kDouble:
      return value.number;
    case Constant::Kind::kString:
      return value.text;
    case Constant::Kind::kDuration:
      return absl::FormatDuration(value.duration);
    case Constant::Kind::kList: {
      nlohmann::json items = nlohmann::json::array();
      for (const Constant& item : value.items) {
        items.push_back(ConstantToJson(item));
      }
      return items;
    }
    case Constant::Kind::kObject: {
      nlohmann::json pairs = nlohmann::json::object();
      for (const auto& [key, held] : value.pairs) {
        pairs[key] = ConstantToJson(held);
      }
      return pairs;
    }
  }
  return nullptr;
}

/// A type's fields, in the shape a `struct` records them.
///
/// The same [DtoPlan] a declared shape produces, so completion of a registered
/// type's fields and completion of a `struct`'s are one code path.
DtoPlan ShapeFromJson(std::string_view tag, const nlohmann::json& value) {
  DtoPlan shape;
  shape.name = std::string(tag);
  shape.description = Text(value, "description");
  const auto fields = value.find("fields");
  if (fields == value.end() || !fields->is_array()) return shape;
  for (const nlohmann::json& one : *fields) {
    if (!one.is_object()) continue;
    FieldPlan field;
    field.name = Text(one, "name");
    if (field.name.empty()) continue;
    field.type = Text(one, "type");
    field.declared = field.type;
    field.element = Text(one, "element");
    field.description = Text(one, "description");
    field.required = Flag(one, "required", false);
    if (const auto given = one.find("default"); given != one.end()) {
      field.default_value = ConstantFromJson(*given);
      field.has_default = true;
    }
    if (field.type == "bytes" || field.element == "bytes") shape.binary = true;
    shape.fields.push_back(std::move(field));
  }
  return shape;
}

nlohmann::json ShapeToJson(const DtoPlan& shape) {
  nlohmann::json fields = nlohmann::json::array();
  for (const FieldPlan& field : shape.fields) {
    nlohmann::json one{{"name", field.name}, {"type", field.type}};
    if (!field.element.empty()) one["element"] = field.element;
    if (!field.description.empty()) one["description"] = field.description;
    if (field.required) one["required"] = true;
    if (field.has_default) one["default"] = ConstantToJson(field.default_value);
    fields.push_back(std::move(one));
  }
  nlohmann::json value{{"fields", std::move(fields)}};
  if (!shape.description.empty()) value["description"] = shape.description;
  return value;
}

}  // namespace

const PortInfo* absl_nullable ActionInfo::Port(
    std::string_view name, syntax::PortDirection direction) const {
  const std::vector<PortInfo>& side =
      direction == syntax::PortDirection::kInput ? inputs : outputs;
  for (const PortInfo& port : side) {
    if (port.name == name) return &port;
  }
  return nullptr;
}

std::vector<std::string> ActionInfo::PortNames(
    syntax::PortDirection direction) const {
  const std::vector<PortInfo>& side =
      direction == syntax::PortDirection::kInput ? inputs : outputs;
  std::vector<std::string> names;
  names.reserve(side.size());
  for (const PortInfo& port : side) names.push_back(port.name);
  return names;
}

Catalogue Catalogue::Of(std::vector<ActionInfo> actions,
                        std::vector<TypeInfo> types) {
  Catalogue built;
  built.actions_ = std::move(actions);
  built.types_ = std::move(types);
  return built;
}

Catalogue Catalogue::FromJson(const nlohmann::json& value) {
  Catalogue built;
  if (!value.is_object()) return built;

  if (const auto actions = value.find("actions");
      actions != value.end() && actions->is_array()) {
    for (const nlohmann::json& one : *actions) {
      if (!one.is_object()) continue;
      ActionInfo action;
      action.name = Text(one, "name");
      if (action.name.empty()) continue;
      action.description = Text(one, "description");
      action.inputs = PortsFromJson(one, "inputs");
      action.outputs = PortsFromJson(one, "outputs");
      action.headers = PortsFromJson(one, "headers");
      action.origin = OriginFromJson(one);
      built.actions_.push_back(std::move(action));
    }
  }
  if (const auto types = value.find("types");
      types != value.end() && types->is_array()) {
    for (const nlohmann::json& one : *types) {
      if (!one.is_object()) continue;
      TypeInfo type;
      type.tag = Text(one, "tag");
      if (type.tag.empty()) continue;
      type.shape = ShapeFromJson(type.tag, one);
      type.origin = OriginFromJson(one);
      built.types_.push_back(std::move(type));
    }
  }
  return built;
}

nlohmann::json Catalogue::ToJson() const {
  nlohmann::json actions = nlohmann::json::array();
  for (const ActionInfo& action : actions_) {
    nlohmann::json one{{"name", action.name}};
    if (!action.description.empty()) one["description"] = action.description;
    if (!action.inputs.empty()) one["inputs"] = PortsToJson(action.inputs);
    if (!action.outputs.empty()) one["outputs"] = PortsToJson(action.outputs);
    if (!action.headers.empty()) one["headers"] = PortsToJson(action.headers);
    if (action.origin.has_value()) one["origin"] = OriginToJson(*action.origin);
    actions.push_back(std::move(one));
  }
  nlohmann::json types = nlohmann::json::array();
  for (const TypeInfo& type : types_) {
    nlohmann::json one = ShapeToJson(type.shape);
    one["tag"] = type.tag;
    if (type.origin.has_value()) one["origin"] = OriginToJson(*type.origin);
    types.push_back(std::move(one));
  }
  return nlohmann::json{{"format", kCatalogueFormat},
                        {"actions", std::move(actions)},
                        {"types", std::move(types)}};
}

Catalogue Catalogue::MergedWith(const Catalogue& other) const {
  Catalogue merged = *this;
  for (const ActionInfo& action : other.actions_) {
    bool replaced = false;
    for (ActionInfo& mine : merged.actions_) {
      if (mine.name != action.name) continue;
      mine = action;
      replaced = true;
    }
    if (!replaced) merged.actions_.push_back(action);
  }
  for (const TypeInfo& type : other.types_) {
    bool replaced = false;
    for (TypeInfo& mine : merged.types_) {
      if (mine.tag != type.tag) continue;
      mine = type;
      replaced = true;
    }
    if (!replaced) merged.types_.push_back(type);
  }
  return merged;
}

const ActionInfo* absl_nullable Catalogue::Action(std::string_view name) const {
  for (const ActionInfo& action : actions_) {
    if (action.name == name) return &action;
  }
  return nullptr;
}

const TypeInfo* absl_nullable Catalogue::Type(std::string_view tag) const {
  for (const TypeInfo& type : types_) {
    if (type.tag == tag) return &type;
  }
  return nullptr;
}

const Catalogue& Catalogue::Builtin() {
  // Parsed once. A snapshot that will not parse is a generator bug and would be
  // caught by the check that regenerates it, so a bad one here is an empty
  // catalogue rather than a crash in an editor.
  static const Catalogue* const kBuiltin = [] {
    const nlohmann::json value =
        nlohmann::json::parse(kCatalogueSnapshot, nullptr, false);
    return new Catalogue(value.is_discarded() ? Catalogue()
                                              : FromJson(value));
  }();
  return *kBuiltin;
}

}  // namespace a11::flow::catalogue
