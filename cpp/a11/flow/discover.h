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

#ifndef A11_FLOW_DISCOVER_H_
#define A11_FLOW_DISCOVER_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/types/span.h>

#include "a11/flow/catalogue.h"

namespace a11::flow {

/// Source scanning for project action declarations.
/// Reads literal `ActionSchema` fields in Python, C++, and TypeScript.
/// Partial entries retain known fields; schemas without a literal name are
/// omitted. Origins support navigation to declarations.
namespace discover {

/// Catalogue origin of a source declaration.
using Origin = catalogue::Origin;

/// A language an `ActionSchema` may be declared in.
enum class Language {
  kPython,
  kCpp,
  kTypeScript,
};

/// The language a path's extension says it is, or `nullopt` for a file this
/// does not read.
std::optional<Language> LanguageOf(std::string_view path);

/// What to read and what to leave alone.
struct Options {
  /// Directory names never descended into. Defaults to the ones that hold
  /// generated code and caches: `node_modules`, `.venv*`, `build*`,
  /// `cmake-build*`, `dist`, `.git`, `__pycache__`, `.next`, `.vscode-test`.
  std::vector<std::string> skip_directories;

  /// Maximum file size read during a scan.
  size_t max_file_bytes = 1u << 20;

  /// The most files read in one scan.
  size_t max_files = 20000;

  /// The most directory entries visited, including non-source files.
  size_t max_entries = 50000;

  /// Options with the defaults filled in.
  static Options Default();
};

/// Discovered actions and limits reached during the scan.
struct Result {
  catalogue::Catalogue found;
  /// Files opened and read to the end.
  size_t files_read = 0;
  /// Files skipped for being larger than `max_file_bytes`.
  std::vector<std::string> too_large;
  /// Whether `max_files` stopped the walk before it ran out of files.
  bool reached_file_limit = false;
  /// Directory entries visited, including directories and non-source files.
  size_t entries_visited = 0;
  /// Whether `max_entries` stopped traversal.
  bool reached_entry_limit = false;
};

/// Scans directories or files under `roots` for action declarations.
/// Unreadable paths are skipped; the result records scan limits.
Result Discover(absl::Span<const std::string> roots,
                const Options& options = Options::Default());

/// Scans supplied source and records `path` as its origin.
catalogue::Catalogue DiscoverInSource(std::string_view source,
                                      std::string_view path, Language language);

/// Scan results use the catalogue envelope format.
inline constexpr std::string_view kScanFormat = catalogue::kCatalogueFormat;

}  // namespace discover
}  // namespace a11::flow

#endif  // A11_FLOW_DISCOVER_H_
