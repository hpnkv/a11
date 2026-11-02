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

#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/internal/regex.h"
#include "a11/json_codec.h"
#include "a11/percent.h"

namespace a11::actions {
namespace {

/// A string field, or empty where the key is absent or not a string.
std::string Text(const nlohmann::json& value, std::string_view key) {
  const auto found = value.find(key);
  if (found == value.end() || !found->is_string()) {
    return {};
  }
  return found->get<std::string>();
}

/// A boolean field, or @p fallback where the key is absent or not a boolean.
bool Flag(const nlohmann::json& value, std::string_view key, bool fallback) {
  const auto found = value.find(key);
  if (found == value.end() || !found->is_boolean()) {
    return fallback;
  }
  return found->get<bool>();
}

/// The strings in an array field, skipping anything that is not one.
std::vector<std::string> Strings(const nlohmann::json& value,
                                 std::string_view key) {
  std::vector<std::string> result;
  const auto found = value.find(key);
  if (found == value.end()) {
    return result;
  }
  if (found->is_string()) {
    result.push_back(found->get<std::string>());
    return result;
  }
  if (!found->is_array()) {
    return result;
  }
  for (const nlohmann::json& one : *found) {
    if (one.is_string()) {
      result.push_back(one.get<std::string>());
    }
  }
  return result;
}

/// Ports in the order a reader will see them: declaration order is not kept by
/// `flat_hash_map`, and a description that reshuffles itself between calls is
/// one nobody can diff. Sorted by name, which is arbitrary but stable.
bool MatchesAnyPattern(const std::vector<std::string>& patterns,
                       std::string_view name) {
  for (const std::string& pattern : patterns) {
    auto expression = a11::internal::CompileRegex(pattern);
    if (!expression.ok()) {
      continue;
    }
    if (expression->FullMatch(name)) {
      return true;
    }
  }
  return false;
}

/// Every pattern compiles, or the first one that does not, named.
absl::Status ValidatePatterns(const std::vector<std::string>& patterns) {
  for (const std::string& pattern : patterns) {
    auto expression = a11::internal::CompileRegex(pattern);
    if (!expression.ok()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "'", pattern,
          "' is not a valid name pattern: ", expression.status().message()));
    }
  }
  return absl::OkStatus();
}

/// Whether a name belongs to the reserved, double-underscored set.
bool IsReservedName(std::string_view name) {
  return name.size() > 4 && name.substr(0, 2) == "__";
}

}  // namespace

bool SchemaQueryAccepts(const SchemaQuery& request, std::string_view name) {
  const bool named = std::find(request.exact.begin(), request.exact.end(),
                               name) != request.exact.end();
  if (!request.include_reserved && IsReservedName(name) && !named) {
    return false;
  }
  if (request.names.empty() && request.exact.empty()) {
    return true;
  }
  if (named) {
    return true;
  }
  return MatchesAnyPattern(request.names, name);
}

absl::StatusOr<SchemaQuery> ParseSchemaQuery(std::string_view encoded) {
  SchemaQuery request;
  const std::string trimmed(absl::StripAsciiWhitespace(encoded));
  // An empty request uses the default query.
  if (trimmed.empty() || trimmed == "null") {
    return request;
  }
  ABSL_ASSIGN_OR_RETURN(const nlohmann::json value,
                        a11::ParseJson(trimmed, "__list_actions__ request"));
  if (value.is_null()) {
    return request;
  }
  if (value.is_array()) {
    // A bare array is read as name patterns.
    for (const nlohmann::json& one : value) {
      if (one.is_string()) {
        request.names.push_back(one.get<std::string>());
      }
    }
    return request;
  }
  if (!value.is_object()) {
    return absl::InvalidArgumentError(
        "A __list_actions__ request must be an object, an array of patterns, "
        "or "
        "absent");
  }
  request.names = Strings(value, "names");
  request.exact = Strings(value, "exact");
  request.include_reserved = Flag(value, "include_reserved", false);
  request.runnable_only = Flag(value, "runnable_only", false);
  if (Text(value, "ports") == "all") {
    request.ports = PortView::kAll;
  }
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
      // A query string is form-encoded, so `+` is a space. A pattern is a
      // regex, and a regex is full of characters a client will have escaped.
      value = percent::Decode(pair.substr(split + 1), /*plus_is_space=*/true);
    }
    if (key == "name") {
      if (!value.empty()) {
        request.names.push_back(std::move(value));
      }
    } else if (key == "exact") {
      if (!value.empty()) {
        request.exact.push_back(std::move(value));
      }
    } else if (key == "ports") {
      if (value == "all") {
        request.ports = PortView::kAll;
      }
    } else if (key == "reserved") {
      request.include_reserved = value != "0" && value != "false";
    } else if (key == "runnable") {
      request.runnable_only = value != "0" && value != "false";
    }
  }
  ABSL_RETURN_IF_ERROR(ValidatePatterns(request.names));
  return request;
}

nlohmann::json RegistryToJson(const ActionRegistry& registry,
                              const SchemaQuery& request) {
  std::vector<std::string> names = registry.ListRegisteredActions();
  std::sort(names.begin(), names.end());
  nlohmann::json actions = nlohmann::json::array();
  for (const std::string& name : names) {
    if (!SchemaQueryAccepts(request, name)) {
      continue;
    }
    absl::StatusOr<ActionSchema> schema = registry.GetSchema(name);
    // A name that vanished between the listing and the lookup is a registry
    // that changed under us, not an error worth failing the whole listing over.
    if (!schema.ok()) {
      continue;
    }
    const bool runnable = registry.GetHandler(name).ok();
    if (request.runnable_only && !runnable) {
      continue;
    }
    actions.push_back(SchemaToJson(*schema, runnable, request.ports));
  }
  return nlohmann::json{{"format", std::string(kSchemaDocumentFormat)},
                        {"actions", std::move(actions)}};
}

absl::StatusOr<std::string> RegistryToJsonText(const ActionRegistry& registry,
                                               const SchemaQuery& request) {
  return a11::DumpJson(RegistryToJson(registry, request), "action descriptors");
}

}  // namespace a11::actions
