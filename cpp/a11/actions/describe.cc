// Copyright 2026 The A11 Authors.

#include "a11/actions/describe.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_split.h>
#include <nlohmann/json.hpp>

#include "a11/actions/internal/pattern.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/json_codec.h"

namespace a11::actions {
namespace {

/// A string field, or empty where the key is absent or not a string.
std::string Text(const nlohmann::json& value, std::string_view key) {
  const auto found = value.find(key);
  if (found == value.end() || !found->is_string()) return {};
  return found->get<std::string>();
}

/// A boolean field, or @p fallback where the key is absent or not a boolean.
bool Flag(const nlohmann::json& value, std::string_view key, bool fallback) {
  const auto found = value.find(key);
  if (found == value.end() || !found->is_boolean()) return fallback;
  return found->get<bool>();
}

/// The strings in an array field, skipping anything that is not one.
std::vector<std::string> Strings(const nlohmann::json& value,
                                 std::string_view key) {
  std::vector<std::string> result;
  const auto found = value.find(key);
  if (found == value.end()) return result;
  if (found->is_string()) {
    result.push_back(found->get<std::string>());
    return result;
  }
  if (!found->is_array()) return result;
  for (const nlohmann::json& one : *found) {
    if (one.is_string()) result.push_back(one.get<std::string>());
  }
  return result;
}

/// Ports in the order a reader will see them: declaration order is not kept by
/// `flat_hash_map`, and a description that reshuffles itself between calls is
/// one nobody can diff. Sorted by name, which is arbitrary but stable.
std::vector<const ActionPortSchema*> SortedPorts(
    const absl::flat_hash_map<std::string, ActionPortSchema>& ports) {
  std::vector<const ActionPortSchema*> sorted;
  sorted.reserve(ports.size());
  for (const auto& [unused, port] : ports) {
    (void)unused;
    sorted.push_back(&port);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const ActionPortSchema* left, const ActionPortSchema* right) {
              return left->name < right->name;
            });
  return sorted;
}

std::vector<const ActionHeaderSchema*> SortedHeaders(
    const absl::flat_hash_map<std::string, ActionHeaderSchema>& headers) {
  std::vector<const ActionHeaderSchema*> sorted;
  sorted.reserve(headers.size());
  for (const auto& [unused, header] : headers) {
    (void)unused;
    sorted.push_back(&header);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const ActionHeaderSchema* left, const ActionHeaderSchema* right) {
              return left->name < right->name;
            });
  return sorted;
}

/// A port's `json_schema`, parsed if it parses.
///
/// Carried as a JSON value rather than as the string it is stored as, so a
/// consumer building a tool definition can splice it in rather than re-parse it.
/// Text that is not JSON is dropped: a schema nobody can read is worse than an
/// absent one, which at least reads as "no type information".
void AttachJsonSchema(nlohmann::json* target, const std::string& encoded) {
  if (encoded.empty()) return;
  absl::StatusOr<nlohmann::json> parsed =
      a11::ParseJson(encoded, "port JSON Schema");
  if (!parsed.ok()) return;
  (*target)["json_schema"] = *std::move(parsed);
}

nlohmann::json PortToJson(const ActionPortSchema& port, bool autofilled) {
  nlohmann::json value{{"name", port.name}, {"type", port.type}};
  if (!port.description.empty()) value["description"] = port.description;
  if (port.required) value["required"] = true;
  // Always explicit. See the warning on describe.h: the two port structs in
  // this codebase disagree on what an absent `unary` means.
  value["unary"] = port.unary;
  if (autofilled) value["autofilled"] = true;
  AttachJsonSchema(&value, port.json_schema);
  return value;
}

ActionPortSchema PortFromJson(const nlohmann::json& value) {
  ActionPortSchema port;
  port.name = Text(value, "name");
  port.type = Text(value, "type");
  port.description = Text(value, "description");
  port.required = Flag(value, "required", false);
  // No fallback worth having: a document that omits `unary` was not written by
  // this codec, and guessing is the bug the warning is about. False matches
  // ActionPortSchema's own default, so a guess is at least consistent locally.
  port.unary = Flag(value, "unary", false);
  // A `user_facing` flag from an older client is read and dropped. Narration
  // travels on the reserved log port, which no schema declares, so a port
  // saying it was narration has nothing left to mean.
  const auto schema = value.find("json_schema");
  if (schema != value.end() && !schema->is_null()) {
    port.json_schema = a11::DumpJsonLossy(*schema);
  }
  return port;
}

nlohmann::json HeaderToJson(const ActionHeaderSchema& header) {
  nlohmann::json value{{"name", header.name}};
  if (!header.description.empty()) value["description"] = header.description;
  if (header.default_value.has_value()) {
    value["has_default"] = true;
    // The value itself only where it is text. A header default can be arbitrary
    // bytes, and base64 in a document whose whole purpose is to be read by a
    // person is a worse answer than saying a default exists.
    if (a11::IsValidUtf8(*header.default_value)) {
      value["default"] = *header.default_value;
    }
  }
  return value;
}

ActionHeaderSchema HeaderFromJson(const nlohmann::json& value) {
  ActionHeaderSchema header;
  header.name = Text(value, "name");
  header.description = Text(value, "description");
  const auto found = value.find("default");
  if (found != value.end() && found->is_string()) {
    header.default_value = found->get<std::string>();
  }
  return header;
}

/// One hex digit's value, or -1.
int HexValue(char one) {
  if (one >= '0' && one <= '9') return one - '0';
  if (one >= 'a' && one <= 'f') return one - 'a' + 10;
  if (one >= 'A' && one <= 'F') return one - 'A' + 10;
  return -1;
}

/// A query-string value with `%xx` and `+` undone.
///
/// A pattern is a regex, and a regex is full of characters a client will have
/// escaped. An undecodable `%` is passed through as itself rather than dropped,
/// so a malformed query gives a "no such pattern" answer instead of a silently
/// different one.
std::string PercentDecode(std::string_view encoded) {
  std::string decoded;
  decoded.reserve(encoded.size());
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const char one = encoded[index];
    if (one == '+') {
      decoded.push_back(' ');
      continue;
    }
    if (one != '%' || index + 2 >= encoded.size()) {
      decoded.push_back(one);
      continue;
    }
    const int high = HexValue(encoded[index + 1]);
    const int low = HexValue(encoded[index + 2]);
    if (high < 0 || low < 0) {
      decoded.push_back(one);
      continue;
    }
    decoded.push_back(static_cast<char>(high * 16 + low));
    index += 2;
  }
  return decoded;
}

bool MatchesAnyPattern(const std::vector<std::string>& patterns,
                       std::string_view name) {
  const std::string subject(name);
  for (const std::string& pattern : patterns) {
    // Full match, the same rule `x-a11-allowed-llm-actions` already uses, so a
    // pattern means one thing across this codebase rather than two. A pattern
    // that does not compile is skipped here; the request parser rejected it
    // already, so reaching this is a caller that built a request by hand.
    absl::StatusOr<std::regex> expression =
        internal::CompilePattern(pattern);
    if (!expression.ok()) continue;
    if (std::regex_match(subject, *expression)) return true;
  }
  return false;
}

/// Every pattern compiles, or the first one that does not, named.
absl::Status ValidatePatterns(const std::vector<std::string>& patterns) {
  for (const std::string& pattern : patterns) {
    absl::StatusOr<std::regex> expression =
        internal::CompilePattern(pattern);
    if (!expression.ok()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "'", pattern, "' is not a valid name pattern: ",
          expression.status().message()));
    }
  }
  return absl::OkStatus();
}

/// Whether a name belongs to the reserved, double-underscored set.
bool IsReservedName(std::string_view name) {
  return name.size() > 4 && name.substr(0, 2) == "__";
}

}  // namespace

bool SchemaQueryAccepts(const SchemaQuery& request,
                            std::string_view name) {
  const bool named = std::find(request.exact.begin(), request.exact.end(),
                               name) != request.exact.end();
  if (!request.include_reserved && IsReservedName(name) && !named) {
    return false;
  }
  if (request.names.empty() && request.exact.empty()) return true;
  if (named) return true;
  return MatchesAnyPattern(request.names, name);
}

absl::StatusOr<SchemaQuery> ParseSchemaQuery(std::string_view encoded) {
  SchemaQuery request;
  const std::string trimmed(absl::StripAsciiWhitespace(encoded));
  // No request is the default request. Asking a peer what it serves, with
  // nothing further to say, is the common case and must not need a document.
  if (trimmed.empty() || trimmed == "null") return request;
  ABSL_ASSIGN_OR_RETURN(const nlohmann::json value,
                        a11::ParseJson(trimmed, "__list_actions__ request"));
  if (value.is_null()) return request;
  if (value.is_array()) {
    // A bare array is read as patterns, because that is what a caller who wrote
    // one meant, and refusing it teaches nothing.
    for (const nlohmann::json& one : value) {
      if (one.is_string()) request.names.push_back(one.get<std::string>());
    }
    return request;
  }
  if (!value.is_object()) {
    return absl::InvalidArgumentError(
        "A __list_actions__ request must be an object, an array of patterns, or "
        "absent");
  }
  request.names = Strings(value, "names");
  request.exact = Strings(value, "exact");
  request.include_reserved = Flag(value, "include_reserved", false);
  request.runnable_only = Flag(value, "runnable_only", false);
  if (Text(value, "ports") == "all") request.ports = PortView::kAll;
  ABSL_RETURN_IF_ERROR(ValidatePatterns(request.names));
  return request;
}

absl::StatusOr<SchemaQuery> ParseSchemaQueryString(std::string_view query) {
  SchemaQuery request;
  for (std::string_view pair : absl::StrSplit(query, '&', absl::SkipEmpty())) {
    const std::size_t split = pair.find('=');
    const std::string_view key = pair.substr(0, split);
    std::string value;
    if (split != std::string_view::npos) {
      value = PercentDecode(pair.substr(split + 1));
    }
    if (key == "name") {
      if (!value.empty()) request.names.push_back(std::move(value));
    } else if (key == "exact") {
      if (!value.empty()) request.exact.push_back(std::move(value));
    } else if (key == "ports") {
      if (value == "all") request.ports = PortView::kAll;
    } else if (key == "reserved") {
      request.include_reserved = value != "0" && value != "false";
    } else if (key == "runnable") {
      request.runnable_only = value != "0" && value != "false";
    }
  }
  ABSL_RETURN_IF_ERROR(ValidatePatterns(request.names));
  return request;
}

nlohmann::json SchemaToJson(const ActionSchema& schema, bool runnable,
                              PortView ports) {
  nlohmann::json entry{{"name", schema.name}};
  if (!schema.description.empty()) entry["description"] = schema.description;
  entry["runnable"] = runnable;

  nlohmann::json inputs = nlohmann::json::array();
  for (const ActionPortSchema* port : SortedPorts(schema.inputs)) {
    const bool autofilled = !port->autofills.empty();
    // A caller cannot write an autofilled input: the runtime requires it empty
    // before applying the receiver's default, so offering it invites a failure.
    if (autofilled && ports == PortView::kCallable) continue;
    inputs.push_back(PortToJson(*port, autofilled));
  }
  nlohmann::json outputs = nlohmann::json::array();
  for (const ActionPortSchema* port : SortedPorts(schema.outputs)) {
    outputs.push_back(PortToJson(*port, false));
  }
  if (!inputs.empty()) entry["inputs"] = std::move(inputs);
  if (!outputs.empty()) entry["outputs"] = std::move(outputs);

  nlohmann::json headers = nlohmann::json::array();
  for (const ActionHeaderSchema* header : SortedHeaders(schema.headers)) {
    headers.push_back(HeaderToJson(*header));
  }
  if (!headers.empty()) entry["headers"] = std::move(headers);

  if (!schema.output_to_json_field.empty()) {
    nlohmann::json mapping = nlohmann::json::object();
    // Sorted for the same reason the ports are.
    std::vector<std::pair<std::string, std::string>> pairs(
        schema.output_to_json_field.begin(), schema.output_to_json_field.end());
    std::sort(pairs.begin(), pairs.end());
    for (const auto& [output, field] : pairs) mapping[output] = field;
    entry["output_to_json_field"] = std::move(mapping);
  }
  return entry;
}

nlohmann::json RegistryToJson(const ActionRegistry& registry,
                                const SchemaQuery& request) {
  std::vector<std::string> names = registry.ListRegisteredActions();
  std::sort(names.begin(), names.end());
  nlohmann::json actions = nlohmann::json::array();
  for (const std::string& name : names) {
    if (!SchemaQueryAccepts(request, name)) continue;
    absl::StatusOr<ActionSchema> schema = registry.GetSchema(name);
    // A name that vanished between the listing and the lookup is a registry
    // that changed under us, not an error worth failing the whole listing over.
    if (!schema.ok()) continue;
    const bool runnable = registry.GetHandler(name).ok();
    if (request.runnable_only && !runnable) continue;
    actions.push_back(SchemaToJson(*schema, runnable, request.ports));
  }
  return nlohmann::json{{"format", std::string(kSchemaDocumentFormat)},
                        {"actions", std::move(actions)}};
}

absl::StatusOr<std::string> RegistryToJsonText(
    const ActionRegistry& registry, const SchemaQuery& request) {
  return a11::DumpJson(RegistryToJson(registry, request),
                       "action descriptors");
}

absl::StatusOr<std::string> SchemaToJsonText(const ActionSchema& schema,
                                               bool runnable, PortView ports) {
  nlohmann::json actions = nlohmann::json::array();
  actions.push_back(SchemaToJson(schema, runnable, ports));
  return a11::DumpJson(nlohmann::json{{"format", std::string(kSchemaDocumentFormat)},
                                      {"actions", std::move(actions)}},
                       "action descriptor");
}

absl::StatusOr<ActionSchema> SchemaFromJson(
    const nlohmann::json& entry) {
  if (!entry.is_object()) {
    return absl::InvalidArgumentError("A entry action must be an object");
  }
  ActionSchema schema;
  schema.name = Text(entry, "name");
  if (schema.name.empty()) {
    return absl::InvalidArgumentError("A entry action must have a name");
  }
  schema.description = Text(entry, "description");

  const auto add_ports =
      [&entry](std::string_view key,
                   absl::flat_hash_map<std::string, ActionPortSchema>* into) {
        const auto found = entry.find(key);
        if (found == entry.end() || !found->is_array()) return;
        for (const nlohmann::json& one : *found) {
          if (!one.is_object()) continue;
          ActionPortSchema port = PortFromJson(one);
          if (port.name.empty()) continue;
          into->insert_or_assign(port.name, std::move(port));
        }
      };
  add_ports("inputs", &schema.inputs);
  add_ports("outputs", &schema.outputs);

  const auto headers = entry.find("headers");
  if (headers != entry.end() && headers->is_array()) {
    for (const nlohmann::json& one : *headers) {
      if (!one.is_object()) continue;
      ActionHeaderSchema header = HeaderFromJson(one);
      if (header.name.empty()) continue;
      schema.headers.insert_or_assign(header.name, std::move(header));
    }
  }

  const auto mapping = entry.find("output_to_json_field");
  if (mapping != entry.end() && mapping->is_object()) {
    for (const auto& [output, field] : mapping->items()) {
      if (!field.is_string()) continue;
      // Only a mapping onto a port that came with it: an output named here and
      // absent above would fail validation with a message about the wrong thing.
      if (!schema.outputs.contains(output)) continue;
      ABSL_RETURN_IF_ERROR(
          schema.MapOutputToJson(output, field.get<std::string>()));
    }
  }

  ABSL_RETURN_IF_ERROR(schema.Validate());
  return schema;
}

absl::StatusOr<ActionSchema> SchemaFromJsonText(
    std::string_view encoded) {
  ABSL_ASSIGN_OR_RETURN(const nlohmann::json value,
                        a11::ParseJson(encoded, "action descriptor"));
  return SchemaFromJson(value);
}

absl::StatusOr<std::vector<nlohmann::json>> SchemasInDocument(
    const nlohmann::json& envelope) {
  std::vector<nlohmann::json> entry;
  if (envelope.is_array()) {
    for (const nlohmann::json& one : envelope) entry.push_back(one);
    return entry;
  }
  if (!envelope.is_object()) {
    return absl::InvalidArgumentError(
        "Action descriptors must be an object or an array");
  }
  const auto actions = envelope.find("actions");
  if (actions == envelope.end() || !actions->is_array()) {
    return absl::InvalidArgumentError(
        "Action descriptors are missing their 'actions' array");
  }
  for (const nlohmann::json& one : *actions) entry.push_back(one);
  return entry;
}

}  // namespace a11::actions
