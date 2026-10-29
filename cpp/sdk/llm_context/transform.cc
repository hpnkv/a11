/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sdk/llm_context/transform.h"

#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <nlohmann/json.hpp>

#include "a11/json_codec.h"

namespace a11::sdk::llm_context {
namespace {
bool Space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

absl::Status Check(std::string_view text, size_t max_bytes) {
  if (text.size() > max_bytes) {
    return absl::ResourceExhaustedError("Context exceeds byte limit");
  }
  if (!IsValidUtf8(text)) {
    return absl::InvalidArgumentError("Context is not UTF-8");
  }
  return absl::OkStatus();
}
}  // namespace

absl::StatusOr<std::string> CompactJson(std::string_view text, size_t max_bytes,
                                        size_t max_depth) {
  auto status = Check(text, max_bytes);
  if (!status.ok()) {
    return status;
  }
  std::string out;
  out.reserve(text.size());
  bool quoted = false;
  bool escape = false;
  size_t depth = 0;
  for (char c : text) {
    if (quoted) {
      out.push_back(c);
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        quoted = false;
      }
    } else {
      if (c == '"') {
        quoted = true;
      }
      if (c == '{' || c == '[') {
        if (++depth > max_depth) {
          return absl::ResourceExhaustedError("JSON exceeds depth limit");
        }
      } else if (c == '}' || c == ']') {
        if (depth == 0) {
          return absl::InvalidArgumentError("Unbalanced JSON");
        }
        --depth;
      }
      if (!Space(c)) {
        out.push_back(c);
      }
    }
  }
  if (quoted || depth != 0 || !nlohmann::json::accept(text)) {
    return absl::InvalidArgumentError("Invalid JSON context");
  }
  return out;
}

absl::StatusOr<std::string> Encode(std::string_view text,
                                   std::string_view format, size_t max_bytes) {
  auto status = Check(text, max_bytes);
  if (!status.ok()) {
    return status;
  }
  nlohmann::json parts = nlohmann::json::array();
  nlohmann::json envelope = {{"format", std::string(format)}};
  if (format == "a11.lines/v1") {
    size_t start = 0;
    std::string last;
    size_t count = 0;
    while (start < text.size()) {
      size_t end = text.find('\n', start);
      end = end == std::string_view::npos ? text.size() : end + 1;
      std::string line(text.substr(start, end - start));
      if (count != 0 && line != last) {
        parts.push_back({count, last});
        count = 0;
      }
      last = std::move(line);
      ++count;
      start = end;
    }
    if (count != 0) {
      parts.push_back({count, last});
    }
  } else if (format == "a11.json-keys/v1") {
    auto valid = CompactJson(text, max_bytes);
    if (!valid.ok()) {
      return valid.status();
    }
    absl::flat_hash_map<std::string, size_t> ids;
    nlohmann::json keys = nlohmann::json::array();
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] != '"') {
        continue;
      }
      const size_t begin = i++;
      bool escape = false;
      for (; i < text.size(); ++i) {
        if (escape) {
          escape = false;
        } else if (text[i] == '\\') {
          escape = true;
        } else if (text[i] == '"') {
          break;
        }
      }
      size_t next = i + 1;
      while (next < text.size() && Space(text[next])) {
        ++next;
      }
      if (next >= text.size() || text[next] != ':') {
        continue;
      }
      std::string key(text.substr(begin, i - begin + 1));
      auto [it, added] = ids.try_emplace(key, keys.size());
      if (added) {
        keys.push_back(key);
      }
      if (begin != start) {
        parts.push_back(std::string(text.substr(start, begin - start)));
      }
      parts.push_back(it->second);
      start = i + 1;
    }
    if (start < text.size()) {
      parts.push_back(std::string(text.substr(start)));
    }
    envelope["keys"] = std::move(keys);
  } else {
    return absl::InvalidArgumentError("Unknown context encoding");
  }
  envelope["parts"] = std::move(parts);
  return DumpJson(envelope, "context encoding");
}

absl::StatusOr<std::string> Expand(std::string_view encoded, size_t max_bytes) {
  auto status = Check(encoded, max_bytes);
  if (!status.ok()) {
    return status;
  }
  // Bound depth before handing the document to the JSON decoder.
  auto compact = CompactJson(encoded, max_bytes);
  if (!compact.ok()) {
    return compact.status();
  }
  auto parsed = ParseJson(*compact, "context encoding");
  if (!parsed.ok()) {
    return parsed.status();
  }
  const auto& value = *parsed;
  auto f = value.find("format");
  auto p = value.find("parts");
  if (!value.is_object() || f == value.end() || !f->is_string() ||
      p == value.end() || !p->is_array()) {
    return absl::InvalidArgumentError("Invalid context encoding envelope");
  }
  if (*f != "a11.lines/v1" && *f != "a11.json-keys/v1") {
    return absl::InvalidArgumentError("Unknown context encoding");
  }
  std::string out;
  auto append = [&](std::string_view part, size_t count) -> absl::Status {
    if (!part.empty() && count > (max_bytes - out.size()) / part.size()) {
      return absl::ResourceExhaustedError(
          "Expanded context exceeds byte limit");
    }
    if (part.empty()) {
      return absl::OkStatus();
    }
    for (size_t i = 0; i < count; ++i) {
      out.append(part);
    }
    return absl::OkStatus();
  };
  for (const auto& part : *p) {
    if (*f == "a11.lines/v1") {
      if (!part.is_array() || part.size() != 2 ||
          !part[0].is_number_unsigned() || !part[1].is_string()) {
        return absl::InvalidArgumentError("Invalid line run");
      }
      status =
          append(part[1].get_ref<const std::string&>(), part[0].get<size_t>());
    } else if (*f == "a11.json-keys/v1") {
      auto keys = value.find("keys");
      if (keys == value.end() || !keys->is_array()) {
        return absl::InvalidArgumentError("Missing key dictionary");
      }
      if (part.is_string()) {
        status = append(part.get_ref<const std::string&>(), 1);
      } else if (part.is_number_unsigned() &&
                 part.get<size_t>() < keys->size() &&
                 (*keys)[part.get<size_t>()].is_string()) {
        status = append(
            (*keys)[part.get<size_t>()].get_ref<const std::string&>(), 1);
      } else {
        return absl::InvalidArgumentError("Invalid dictionary reference");
      }
    } else {
      return absl::InvalidArgumentError("Unknown context encoding");
    }
    if (!status.ok()) {
      return status;
    }
  }
  return out;
}

size_t EstimateTokens(std::string_view text) {
  return (text.size() + 2) / 3;
}
}  // namespace a11::sdk::llm_context
