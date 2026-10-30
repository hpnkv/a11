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

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "a11/flow/module.h"
#include "a11/percent.h"

namespace a11::flow {
/// Lexical source identity; normalizing it performs no target-path inspection.
inline std::string FileModuleIdentity(std::string_view reference) {
  std::string name(reference);
  if (name.empty() || name == "-" || name.front() == '<') {
    return name;
  }
  if (name.starts_with("file://")) {
    auto decoded =
        percent::DecodeStrict(std::string_view(name).substr(7), "file URI");
    if (!decoded.ok()) {
      return name;
    }
    name = *decoded;
  }
  std::error_code error;
  const auto path = std::filesystem::absolute(name, error);
  return error ? name : path.lexically_normal().generic_string();
}

/// Filesystem imports confined to explicitly approved roots without symlinks.
inline ModuleResolver FileModuleResolver(
    size_t max_bytes = 8 * 1024 * 1024, ModuleSources overrides = {},
    std::vector<std::string> approved_roots = {}) {
  std::vector<std::filesystem::path> roots;
  for (const auto& root : approved_roots) {
    roots.emplace_back(FileModuleIdentity(root));
  }
  return [max_bytes, overrides = std::move(overrides),
          roots = std::move(roots)](
             std::string_view reference,
             std::string_view importer) -> absl::StatusOr<ModuleSource> {
    if (roots.empty() && overrides.empty()) {
      return absl::PermissionDeniedError(
          "Source imports are disallowed in the current runtime: no source "
          "roots are approved.");
    }
    if (reference.find("://") != std::string_view::npos ||
        reference.find('\0') != std::string_view::npos) {
      return absl::PermissionDeniedError(
          "Source imports require a local path.");
    }
    std::filesystem::path path(reference);
    if (path.is_relative()) {
      if (importer.empty() || importer == "-" || importer.front() == '<') {
        return absl::PermissionDeniedError(
            "A relative source import requires a document path.");
      }
      path = std::filesystem::path(importer).parent_path() / path;
    }
    path = std::filesystem::path(FileModuleIdentity(path.generic_string()));
    if (const auto found = overrides.find(path.generic_string());
        found != overrides.end()) {
      if (found->second.size() > max_bytes) {
        return absl::ResourceExhaustedError(
            "Imported source exceeds the byte limit.");
      }
      return ModuleSource{found->first, found->second};
    }
    if (roots.empty()) {
      return absl::PermissionDeniedError(
          "Source imports are disallowed in the current runtime: "
          "no source roots are approved.");
    }
    std::filesystem::path approved;
    std::filesystem::path relative;
    for (const auto& root : roots) {
      const auto candidate = path.lexically_relative(root);
      if (candidate.empty() || candidate.is_absolute()) {
        continue;
      }
      bool contained = true;
      for (const auto& component : candidate) {
        if (component == "..") {
          contained = false;
        }
      }
      if (contained) {
        approved = root;
        relative = candidate;
        break;
      }
    }
    if (approved.empty()) {
      return absl::PermissionDeniedError(
          "Source import is outside the runtime's approved source roots.");
    }
#if defined(__unix__) || defined(__APPLE__)
    class Descriptor {
     public:
      explicit Descriptor(int value) : value_(value) {}
      ~Descriptor() {
        if (value_ >= 0)
          ::close(value_);
      }
      Descriptor(const Descriptor&) = delete;
      Descriptor& operator=(const Descriptor&) = delete;
      int get() const { return value_; }
      void Reset(int value) {
        if (value_ >= 0)
          ::close(value_);
        value_ = value;
      }

     private:
      int value_;
    };
    Descriptor directory(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    for (const auto& component : approved.relative_path()) {
      if (component == ".")
        continue;
      const int next =
          ::openat(directory.get(), component.c_str(),
                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (next < 0) {
        return absl::PermissionDeniedError(
            "Cannot open an approved source root without traversing symlinks.");
      }
      directory.Reset(next);
    }
    std::vector<std::filesystem::path> components;
    for (const auto& component : relative) {
      if (component != ".")
        components.push_back(component);
    }
    if (components.empty()) {
      return absl::InvalidArgumentError("An imported source must be a file.");
    }
    for (size_t index = 0; index + 1 < components.size(); ++index) {
      const int next =
          ::openat(directory.get(), components[index].c_str(),
                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (next < 0) {
        return absl::PermissionDeniedError(
            "Cannot resolve imported source within approved roots; "
            "symlink traversal is disallowed.");
      }
      directory.Reset(next);
    }
    Descriptor file(::openat(directory.get(), components.back().c_str(),
                             O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.get() < 0) {
      return absl::PermissionDeniedError(
          "Cannot open imported source within approved roots; "
          "symlink traversal is disallowed.");
    }
    struct stat metadata{};
    if (::fstat(file.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
      return absl::InvalidArgumentError(
          "An imported source must be a regular file.");
    }
    if (metadata.st_size < 0 ||
        static_cast<uint64_t>(metadata.st_size) > max_bytes) {
      return absl::ResourceExhaustedError(
          "Imported source exceeds the byte limit.");
    }
    std::string source;
    char buffer[4096];
    while (true) {
      const auto count = ::read(file.get(), buffer, sizeof(buffer));
      if (count == 0)
        break;
      if (count < 0) {
        if (errno == EINTR)
          continue;
        return absl::DataLossError("Cannot read imported source.");
      }
      const auto size = static_cast<size_t>(count);
      if (size > max_bytes - source.size()) {
        return absl::ResourceExhaustedError(
            "Imported source exceeds the byte limit.");
      }
      source.append(buffer, size);
    }
    return ModuleSource{path.generic_string(), std::move(source)};
#else
    return absl::UnimplementedError(
        "This runtime has no confined filesystem source resolver.");
#endif
  };
}
}  // namespace a11::flow
#endif  // A11_FLOW_FILE_MODULES_H_
