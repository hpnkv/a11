// Copyright 2026 The A11 Authors.

#include "a11/flow/schema.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/flow/syntax.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow {
namespace {

using syntax::Constant;

/// A Flow constant as JSON. The three the language has that JSON does not go out
/// the way the schema says to read them back.
nlohmann::json ConstantJson(const Constant& value) {
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
      for (const Constant& item : value.items) items.push_back(ConstantJson(item));
      return items;
    }
    case Constant::Kind::kObject: {
      nlohmann::json pairs = nlohmann::json::object();
      for (const auto& [key, held] : value.pairs) pairs[key] = ConstantJson(held);
      return pairs;
    }
  }
  return nullptr;
}

/// The JSON a constant is, as Flow would write it back in source.
std::string ConstantFlow(const Constant& value) {
  switch (value.kind) {
    case Constant::Kind::kNull:
      return "null";
    case Constant::Kind::kBool:
      return value.boolean ? "true" : "false";
    case Constant::Kind::kInteger:
      return absl::StrCat(value.integer);
    case Constant::Kind::kDouble:
      return nlohmann::json(value.number).dump();
    case Constant::Kind::kString:
      return nlohmann::json(value.text).dump();
    case Constant::Kind::kDuration:
      return absl::FormatDuration(value.duration);
    case Constant::Kind::kList: {
      std::vector<std::string> items;
      for (const Constant& item : value.items) items.push_back(ConstantFlow(item));
      return absl::StrCat("[", absl::StrJoin(items, ", "), "]");
    }
    case Constant::Kind::kObject: {
      std::vector<std::string> pairs;
      for (const auto& [key, held] : value.pairs) {
        pairs.push_back(absl::StrCat(nlohmann::json(key).dump(), ": ",
                                     ConstantFlow(held)));
      }
      return absl::StrCat("{", absl::StrJoin(pairs, ", "), "}");
    }
  }
  return "null";
}

/// A constant of whatever kind this JSON value is.
Constant ConstantOf(const nlohmann::json& value) {
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
      list.items.push_back(ConstantOf(item));
    }
    return list;
  }
  if (value.is_object()) {
    Constant object;
    object.kind = Constant::Kind::kObject;
    for (const auto& [key, held] : value.items()) {
      object.pairs.emplace_back(key, ConstantOf(held));
    }
    return object;
  }
  return Constant::Null();
}

/// Whether a range on this type bounds a length rather than a magnitude.
///
/// The same judgement the coercion makes, and it has to be: a schema that turned
/// `1..200` on a string into `minimum` would validate different values from the
/// flow it came from.
bool BoundsLength(std::string_view type) {
  return type == "string" || type == "text" || type == "bytes" ||
         type == "list" || type == "array" || type == "object" ||
         type == "json";
}

/// The `type`, `format` and `x-a11-type` a Flow scalar goes out as.
void WriteScalar(std::string_view type, nlohmann::json& out) {
  if (type == "string" || type == "text") {
    out["type"] = "string";
    return;
  }
  if (type == "number") {
    out["type"] = "number";
    return;
  }
  if (type == "integer" || type == "int") {
    out["type"] = "integer";
    return;
  }
  if (type == "bool" || type == "boolean") {
    out["type"] = "boolean";
    return;
  }
  if (type == "bytes") {
    out["type"] = "string";
    out["contentEncoding"] = "base64";
    out[std::string(kFlowTypeKey)] = "bytes";
    return;
  }
  if (type == "time") {
    out["type"] = "string";
    out["format"] = "date-time";
    out[std::string(kFlowTypeKey)] = "time";
    return;
  }
  if (type == "duration") {
    out["type"] = "string";
    out["format"] = "duration";
    out[std::string(kFlowTypeKey)] = "duration";
    return;
  }
  if (type == "object") {
    out["type"] = "object";
    return;
  }
  // `json` and `any` say nothing, which in JSON Schema is `{}` -- anything
  // validates. A quoted mimetype or a registry tag is a type only the host
  // knows; a schema can say it is a string carrying that, and the extension says
  // which.
  if (type == "json" || type == "any") return;
  out["type"] = "string";
  out[std::string(kFlowTypeKey)] = std::string(type);
}

/// One field as a schema, following a shape it names as a `$ref`.
nlohmann::json FieldSchema(const FieldPlan& field) {
  nlohmann::json out = nlohmann::json::object();
  if (!field.dto_name.empty()) {
    // A `$ref` carries no siblings a validator has to merge, so a described
    // reference is the reference and the description beside it.
    out["$ref"] = absl::StrCat("#/$defs/", field.dto_name);
    if (!field.description.empty()) out["description"] = field.description;
    return out;
  }
  if (field.type == "list" || field.type == "array") {
    out["type"] = "array";
    if (!field.element.empty()) {
      nlohmann::json items = nlohmann::json::object();
      if (!field.element_dto_name.empty()) {
        items["$ref"] = absl::StrCat("#/$defs/", field.element_dto_name);
      } else {
        WriteScalar(field.element, items);
      }
      out["items"] = std::move(items);
    }
    if (field.unique) out["uniqueItems"] = true;
    if (field.range.has_minimum) {
      out["minItems"] = field.range.minimum.integer;
    }
    if (field.range.has_maximum) {
      out["maxItems"] = field.range.maximum.integer;
    }
  } else {
    WriteScalar(field.type, out);
    if (!field.range.Empty()) {
      const bool length = BoundsLength(field.type);
      if (field.range.has_minimum) {
        out[length ? "minLength" : "minimum"] =
            length ? nlohmann::json(field.range.minimum.integer)
                   : ConstantJson(field.range.minimum);
      }
      if (field.range.has_maximum) {
        out[length ? "maxLength" : "maximum"] =
            length ? nlohmann::json(field.range.maximum.integer)
                   : ConstantJson(field.range.maximum);
      }
    }
  }
  if (field.has_pattern) out["pattern"] = field.pattern;
  if (field.has_enumeration) {
    nlohmann::json allowed = nlohmann::json::array();
    for (const Constant& one : field.enumeration) {
      allowed.push_back(ConstantJson(one));
    }
    out["enum"] = std::move(allowed);
  }
  if (field.has_default) out["default"] = ConstantJson(field.default_value);
  if (!field.description.empty()) out["description"] = field.description;
  return out;
}

/// The body of one shape, without the `$defs` its fields refer to.
nlohmann::json ShapeSchema(const DtoPlan& dto) {
  nlohmann::json properties = nlohmann::json::object();
  nlohmann::json required = nlohmann::json::array();
  nlohmann::json order = nlohmann::json::array();
  for (const FieldPlan& field : dto.fields) {
    properties[field.name] = FieldSchema(field);
    order.push_back(field.name);
    if (field.required) required.push_back(field.name);
  }
  nlohmann::json out{{"type", "object"},
                     {"title", dto.name},
                     {"properties", std::move(properties)},
                     {std::string(kFlowOrderKey), std::move(order)}};
  if (!dto.description.empty()) out["description"] = dto.description;
  if (!required.empty()) out["required"] = std::move(required);
  // A key the shape does not declare is dropped rather than refused, which is
  // what the coercion does -- so the schema says the same thing rather than
  // describing a stricter type than the one that will actually be built.
  out["additionalProperties"] = false;
  return out;
}

/// Every shape `struct` reaches, itself included, in the order they are found.
std::vector<const DtoPlan*> Reachable(const DtoPlan& dto,
                                      const Program& program) {
  std::vector<const DtoPlan*> found{&dto};
  absl::flat_hash_set<std::string> seen{dto.name};
  for (size_t index = 0; index < found.size(); ++index) {
    for (const FieldPlan& field : found[index]->fields) {
      for (const std::string& named :
           {field.dto_name, field.element_dto_name}) {
        if (named.empty() || !seen.insert(named).second) continue;
        if (const DtoPlan* next = program.Dto(named); next != nullptr) {
          found.push_back(next);
        }
      }
    }
  }
  return found;
}

// --- Reading a schema --------------------------------------------------------

void Report(SchemaImport& into, std::string_view code, std::string message) {
  Diagnostic diagnostic;
  diagnostic.code = std::string(code);
  diagnostic.severity = Severity::kWarning;
  diagnostic.family = Family::kForm;
  diagnostic.message = std::move(message);
  into.diagnostics.push_back(std::move(diagnostic));
}

/// The Flow name for a shape, from a `$ref` or a `$defs` key.
///
/// A name that would not lex as one is repaired rather than refused: a schema
/// written elsewhere is under no obligation to name things the way this language
/// does, and an import that failed on a hyphen would be useless.
std::string FlowName(std::string_view given) {
  std::string name;
  bool capitalise = true;
  for (const char letter : given) {
    const bool ok = (letter >= 'a' && letter <= 'z') ||
                    (letter >= 'A' && letter <= 'Z') ||
                    (letter >= '0' && letter <= '9') || letter == '_';
    if (!ok) {
      capitalise = true;
      continue;
    }
    if (name.empty() && letter >= '0' && letter <= '9') name.push_back('_');
    name.push_back(capitalise && letter >= 'a' && letter <= 'z'
                       ? static_cast<char>(letter - 32)
                       : letter);
    capitalise = false;
  }
  if (name.empty()) return "Shape";
  // A shape may not be named after a built-in type, so one that would be gets a
  // suffix rather than a diagnostic nobody can act on.
  if (vocabulary::TypeNames().contains(vocabulary::Canonical(name))) {
    return absl::StrCat(name, "Value");
  }
  return name;
}

/// The shape a `$ref` names, or empty when it is not one this can follow.
std::string RefName(const nlohmann::json& schema) {
  const auto ref = schema.find("$ref");
  if (ref == schema.end() || !ref->is_string()) return "";
  const std::string target = ref->get<std::string>();
  constexpr std::string_view kPrefix = "#/$defs/";
  if (!absl::StartsWith(target, kPrefix)) return "";
  return FlowName(target.substr(kPrefix.size()));
}

/// The keywords this reads. Anything else in a property is reported once.
const absl::flat_hash_set<std::string>& KnownKeywords() {
  static const auto* words = new absl::flat_hash_set<std::string>{
      "type",        "format",      "contentEncoding", "items",
      "enum",        "const",       "default",         "description",
      "title",       "pattern",     "minimum",         "maximum",
      "minLength",   "maxLength",   "minItems",        "maxItems",
      "uniqueItems", "$ref",        "properties",      "required",
      "additionalProperties",       std::string(kFlowTypeKey),
      std::string(kFlowOrderKey),
  };
  return *words;
}

/// The Flow scalar type a property's JSON type says it is.
std::string ScalarType(const nlohmann::json& property) {
  const auto marked = property.find(std::string(kFlowTypeKey));
  if (marked != property.end() && marked->is_string()) {
    return marked->get<std::string>();
  }
  const auto type = property.find("type");
  std::string spelled;
  if (type != property.end() && type->is_string()) {
    spelled = type->get<std::string>();
  } else if (type != property.end() && type->is_array()) {
    // A union of a type and `null` is how a schema spells "may be absent", which
    // Flow spells by the field not being required. The type is the other one.
    for (const nlohmann::json& one : *type) {
      if (one.is_string() && one.get<std::string>() != "null") {
        spelled = one.get<std::string>();
      }
    }
  }
  if (spelled == "integer") return "integer";
  if (spelled == "number") return "number";
  if (spelled == "boolean") return "bool";
  if (spelled == "object") return "object";
  if (spelled == "array") return "list";
  if (spelled != "string") return "json";
  // A string with something that says how to read it: the three types JSON has
  // no word for.
  const auto encoding = property.find("contentEncoding");
  if (encoding != property.end() && encoding->is_string() &&
      encoding->get<std::string>() == "base64") {
    return "bytes";
  }
  const auto format = property.find("format");
  if (format != property.end() && format->is_string()) {
    const std::string named = format->get<std::string>();
    if (named == "date-time") return "time";
    if (named == "duration") return "duration";
  }
  return "string";
}

void ReadShape(const nlohmann::json& schema, std::string_view name,
               SchemaImport& into, absl::flat_hash_set<std::string>& seen);

/// One property as a field.
FieldPlan ReadField(const nlohmann::json& property, std::string_view name,
                    bool required, SchemaImport& into) {
  FieldPlan field;
  field.name = std::string(name);
  field.required = required;

  const std::string referenced = RefName(property);
  if (!referenced.empty()) {
    field.type = referenced;
    field.declared = referenced;
    field.dto_name = referenced;
  } else {
    field.type = ScalarType(property);
    field.declared = field.type;
  }

  if (field.type == "list") {
    const auto items = property.find("items");
    if (items != property.end() && items->is_object()) {
      const std::string element = RefName(*items);
      if (!element.empty()) {
        field.element = element;
        field.element_dto_name = element;
      } else {
        field.element = ScalarType(*items);
      }
      field.declared = absl::StrCat(field.element, "[]");
    }
    if (property.value("uniqueItems", false)) field.unique = true;
    if (property.contains("minItems")) {
      field.range.has_minimum = true;
      field.range.minimum = Constant::Integer(property["minItems"].get<long long>());
    }
    if (property.contains("maxItems")) {
      field.range.has_maximum = true;
      field.range.maximum = Constant::Integer(property["maxItems"].get<long long>());
    }
  } else {
    // A length and a magnitude are one range in Flow, and which of the two a
    // range means is decided by the field's type -- so `minLength` on a string
    // and `minimum` on a number both arrive here as the same thing. A schema
    // that wrote both of a pair would be describing a value that is a string and
    // a number at once; the one that matches the type is the one read.
    const bool length = BoundsLength(field.type);
    const char* low = length ? "minLength" : "minimum";
    const char* high = length ? "maxLength" : "maximum";
    if (property.contains(low)) {
      field.range.has_minimum = true;
      field.range.minimum = ConstantOf(property[low]);
    }
    if (property.contains(high)) {
      field.range.has_maximum = true;
      field.range.maximum = ConstantOf(property[high]);
    }
  }

  if (const auto pattern = property.find("pattern");
      pattern != property.end() && pattern->is_string()) {
    field.pattern = pattern->get<std::string>();
    field.has_pattern = true;
  }
  if (const auto allowed = property.find("enum");
      allowed != property.end() && allowed->is_array()) {
    for (const nlohmann::json& one : *allowed) {
      // A `null` among the allowed values is a schema saying the field may be
      // absent, which Flow says by the field not being required.
      if (one.is_null()) continue;
      field.enumeration.push_back(ConstantOf(one));
    }
    field.has_enumeration = !field.enumeration.empty();
  } else if (const auto only = property.find("const"); only != property.end()) {
    field.enumeration.push_back(ConstantOf(*only));
    field.has_enumeration = true;
  }
  if (const auto given = property.find("default"); given != property.end()) {
    field.default_value = ConstantOf(*given);
    field.has_default = true;
    // A duration went out as the text the language writes one as, so it comes
    // back as the duration it was rather than as a string that happens to read
    // like one -- which is what makes `default 30s` survive the trip.
    if (field.type == "duration" &&
        field.default_value.kind == Constant::Kind::kString) {
      absl::Duration parsed;
      if (absl::ParseDuration(field.default_value.text, &parsed)) {
        field.default_value = Constant::Duration(parsed);
      }
    }
  }
  if (const auto said = property.find("description");
      said != property.end() && said->is_string()) {
    field.description = said->get<std::string>();
  }

  for (const auto& [keyword, unused] : property.items()) {
    if (KnownKeywords().contains(keyword)) continue;
    Report(into, "flow.schema.unsupported-keyword",
           absl::StrCat("'", keyword, "' on '", name,
                        "' has no spelling in a shape, so it was dropped."));
  }
  return field;
}

void ReadShape(const nlohmann::json& schema, std::string_view name,
               SchemaImport& into, absl::flat_hash_set<std::string>& seen) {
  if (!seen.insert(std::string(name)).second) return;

  DtoPlan dto;
  dto.name = std::string(name);
  if (const auto said = schema.find("description");
      said != schema.end() && said->is_string()) {
    dto.description = said->get<std::string>();
  }

  absl::flat_hash_set<std::string> required;
  if (const auto listed = schema.find("required");
      listed != schema.end() && listed->is_array()) {
    for (const nlohmann::json& one : *listed) {
      if (one.is_string()) required.insert(one.get<std::string>());
    }
  }

  const auto properties = schema.find("properties");
  if (properties == schema.end() || !properties->is_object()) {
    Report(into, "flow.schema.not-a-record",
           absl::StrCat("'", name,
                        "' has no properties, so the shape it describes has no "
                        "fields."));
  } else {
    // Declaration order where the schema kept it, and the object's own order
    // otherwise -- which for a schema written elsewhere is the best there is.
    std::vector<std::string> keys;
    if (const auto ordered = schema.find(std::string(kFlowOrderKey));
        ordered != schema.end() && ordered->is_array()) {
      for (const nlohmann::json& one : *ordered) {
        if (one.is_string() && properties->contains(one.get<std::string>())) {
          keys.push_back(one.get<std::string>());
        }
      }
    }
    for (const auto& [key, unused] : properties->items()) {
      if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
        keys.push_back(key);
      }
    }
    for (const std::string& key : keys) {
      const nlohmann::json& property = properties->at(key);
      if (!property.is_object()) {
        Report(into, "flow.schema.not-a-record",
               absl::StrCat("'", key, "' of '", name,
                            "' is not an object, so it describes no field."));
        continue;
      }
      dto.fields.push_back(
          ReadField(property, key, required.contains(key), into));
    }
  }
  // Recorded before the shapes it names, so the one that was asked about is
  // first.
  into.dtos.push_back(std::move(dto));

  // Only the definitions something actually referred to: a `$defs` full of
  // shapes nothing uses would arrive as a file of unused declarations.
  const auto defs = schema.find("$defs");
  if (defs == schema.end() || !defs->is_object()) return;
  const DtoPlan& written = into.dtos[into.dtos.size() - 1];
  std::vector<std::string> wanted;
  for (const FieldPlan& field : written.fields) {
    for (const std::string& named : {field.dto_name, field.element_dto_name}) {
      if (!named.empty()) wanted.push_back(named);
    }
  }
  for (const std::string& named : wanted) {
    for (const auto& [key, definition] : defs->items()) {
      if (FlowName(key) != named || !definition.is_object()) continue;
      // The `$defs` travel with every nested read, so a shape that names one two
      // levels down is still found.
      nlohmann::json carried = definition;
      if (!carried.contains("$defs")) carried["$defs"] = *defs;
      ReadShape(carried, named, into, seen);
    }
  }
}

/// Whether the shapes hold bytes anywhere, the way the resolver works it out.
void MarkBinary(std::vector<DtoPlan>& dtos) {
  for (DtoPlan& dto : dtos) {
    for (const FieldPlan& field : dto.fields) {
      if (field.type == "bytes" || field.element == "bytes") dto.binary = true;
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (DtoPlan& dto : dtos) {
      if (dto.binary) continue;
      for (const FieldPlan& field : dto.fields) {
        for (const std::string& named :
             {field.dto_name, field.element_dto_name}) {
          if (named.empty()) continue;
          for (const DtoPlan& other : dtos) {
            if (other.name == named && other.binary) {
              dto.binary = true;
              changed = true;
            }
          }
        }
      }
    }
  }
}

}  // namespace

nlohmann::json DtoToJsonSchema(const DtoPlan& dto, const Program& program) {
  nlohmann::json out = ShapeSchema(dto);
  out["$schema"] = "https://json-schema.org/draft/2020-12/schema";

  const std::vector<const DtoPlan*> reachable = Reachable(dto, program);
  nlohmann::json defs = nlohmann::json::object();
  for (const DtoPlan* one : reachable) {
    // The shape itself goes into `$defs` too, so a field of its own type is a
    // reference that resolves rather than one that dangles.
    defs[one->name] = ShapeSchema(*one);
  }
  if (!defs.empty()) out["$defs"] = std::move(defs);
  return out;
}

SchemaImport JsonSchemaToDtos(const nlohmann::json& schema,
                              std::string_view name) {
  SchemaImport into;
  if (!schema.is_object()) {
    Report(into, "flow.schema.not-a-record",
           "A JSON Schema describing a shape is an object.");
    return into;
  }
  std::string chosen(name);
  if (const auto title = schema.find("title");
      chosen.empty() && title != schema.end() && title->is_string()) {
    chosen = title->get<std::string>();
  }
  absl::flat_hash_set<std::string> seen;
  ReadShape(schema, FlowName(chosen.empty() ? "Shape" : chosen), into, seen);
  MarkBinary(into.dtos);
  return into;
}

std::string DtoToFlow(const DtoPlan& dto) {
  // The columns the formatter would give it, worked out the same way: the widest
  // name, and the widest type among the lines that carry a description.
  size_t name_width = 0;
  size_t type_width = 0;
  std::vector<std::string> types;
  types.reserve(dto.fields.size());
  for (const FieldPlan& field : dto.fields) {
    name_width = std::max(name_width, field.name.size() + 1);
    std::string written =
        field.declared.empty() ? field.type : field.declared;
    if (field.required) absl::StrAppend(&written, " required");
    if (field.unique) absl::StrAppend(&written, " unique");
    if (!field.range.Empty()) {
      absl::StrAppend(&written, " ",
                      field.range.has_minimum
                          ? ConstantFlow(field.range.minimum)
                          : "",
                      "..",
                      field.range.has_maximum
                          ? ConstantFlow(field.range.maximum)
                          : "");
    }
    if (field.has_pattern) {
      absl::StrAppend(&written, " matching ", nlohmann::json(field.pattern).dump());
    }
    if (field.has_enumeration) {
      std::vector<std::string> allowed;
      for (const Constant& one : field.enumeration) {
        allowed.push_back(ConstantFlow(one));
      }
      absl::StrAppend(&written, " one of [", absl::StrJoin(allowed, ", "), "]");
    }
    if (field.has_default) {
      absl::StrAppend(&written, " default ", ConstantFlow(field.default_value));
    }
    if (!field.description.empty()) {
      type_width = std::max(type_width, written.size());
    }
    types.push_back(std::move(written));
  }

  std::string out = absl::StrCat("struct ", dto.name, " {\n");
  if (!dto.description.empty()) {
    absl::StrAppend(&out, "  describe ", nlohmann::json(dto.description).dump(),
                    "\n\n");
  }
  for (size_t index = 0; index < dto.fields.size(); ++index) {
    const FieldPlan& field = dto.fields[index];
    std::string line = absl::StrCat("  ", field.name, ":");
    line.append(name_width - field.name.size() - 1, ' ');
    absl::StrAppend(&line, " ", types[index]);
    if (!field.description.empty()) {
      line.append(type_width > types[index].size()
                      ? type_width - types[index].size()
                      : 0,
                  ' ');
      absl::StrAppend(&line, " ", nlohmann::json(field.description).dump());
    }
    absl::StrAppend(&out, line, "\n");
  }
  absl::StrAppend(&out, "}\n");
  return out;
}

}  // namespace a11::flow
