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

#ifndef A11_FLOW_FILE_MODULES_H_
#define A11_FLOW_FILE_MODULES_H_
#include <filesystem>
#include <fstream>
#include <iterator>

#include "a11/flow/module.h"
#include "a11/percent.h"

namespace a11::flow {
/// Canonical identity used by hosts for a local root document.
inline std::string FileModuleIdentity(std::string_view reference) {
  std::string name(reference);
  if (name.empty() || name == "-" || name.front() == '<') {
    return name;
  }
  if (name.starts_with("file://")) {
    auto decoded = a11::percent::DecodeStrict(std::string_view(name).substr(7),
                                              "file URI");
    if (!decoded.ok()) {
      return name;
    }
    name = *decoded;
  }
  std::error_code error;
  auto path = std::filesystem::weakly_canonical(name, error);
  return error ? name : path.generic_string();
}

/// Explicit host adapter for local imports, with canonical filesystem identity.
inline ModuleResolver FileModuleResolver(size_t max_bytes = 8 * 1024 * 1024,
                                         ModuleSources overrides = {}) {
  return [max_bytes, overrides = std::move(overrides)](
             std::string_view reference,
             std::string_view importer) -> absl::StatusOr<ModuleSource> {
    if (reference.find("://") != std::string_view::npos) {
      return absl::InvalidArgumentError(
          "A file import requires a local filesystem path.");
    }
    std::filesystem::path path(reference);
    if (path.is_relative()) {
      if (importer.empty() || importer == "-" || importer.front() == '<') {
        return absl::InvalidArgumentError(
            "A relative file import requires a root document path.");
      }
      path = std::filesystem::path(importer).parent_path() / path;
    }
    std::error_code error;
    path = std::filesystem::weakly_canonical(path, error);
    if (error) {
      return absl::NotFoundError(error.message());
    }
    if (const auto found = overrides.find(path.generic_string());
        found != overrides.end()) {
      if (found->second.size() > max_bytes) {
        return absl::ResourceExhaustedError(
            "Imported source exceeds the byte limit.");
      }
      return ModuleSource{found->first, found->second};
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
      return absl::NotFoundError(error.message());
    }
    if (size > max_bytes) {
      return absl::ResourceExhaustedError(
          "Imported source exceeds the byte limit.");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
      return absl::NotFoundError("Cannot open imported source.");
    }
    std::string source;
    source.reserve(static_cast<size_t>(size));
    char buffer[4096];
    while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
      const auto count = static_cast<size_t>(stream.gcount());
      if (count > max_bytes - source.size()) {
        return absl::ResourceExhaustedError(
            "Imported source exceeds the byte limit.");
      }
      source.append(buffer, count);
    }
    if (!stream.eof()) {
      return absl::DataLossError("Cannot read imported source.");
    }
    return ModuleSource{path.generic_string(), std::move(source)};
  };
}
}  // namespace a11::flow
#endif  // A11_FLOW_FILE_MODULES_H_
