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

#ifndef A11_FLOW_MODULE_H_
#define A11_FLOW_MODULE_H_

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/status/statusor.h>

#include "a11/flow/catalogue.h"
#include "a11/flow/limits.h"
#include "a11/flow/parser.h"

namespace a11::flow {
/// Source supplied by a host, with its canonical identity.
struct ModuleSource {
  std::string identity;
  std::string source;
};

using ModuleSources = absl::flat_hash_map<std::string, std::string>;
using ModuleResolver = std::function<absl::StatusOr<ModuleSource>(
    std::string_view reference, std::string_view importer)>;

/// Limits applied before imported source is parsed or linked.
using ModuleLimits = CompilationLimits;

/// One original document's interval in linked analysis source.
struct ModuleRange {
  std::string identity;
  size_t start = 0;
  size_t end = 0;
};

/// One visible namespace member and its linked identity.
struct ModuleMember {
  std::string name;
  std::string target;
  std::string source;
  syntax::Location location;
  size_t scope_start = 0;
  size_t scope_end = 0;
  bool scoped = false;
  bool type = false;
  bool selection = false;
  std::string intrinsic = {};
  bool module = false;
};

struct ImportSelection {
  size_t start = 0;
  size_t end = 0;
};

/// Owning syntax and source snapshot for a linked module graph.
struct LinkedModule {
  std::string source;
  ParseResult parsed;
  std::vector<ModuleRange> documents;
  std::vector<ModuleMember> members;
  std::vector<ImportSelection> selections;

  bool SelectingImport(size_t offset) const;

  /// Public namespace members visible at an offset in the root document.
  catalogue::Catalogue CatalogueAt(
      size_t offset, bool import_selection = true,
      CompilationLimits limits = {},
      CompilationBudget* absl_nullable budget = nullptr) const;

  /// Rebase linked diagnostics to their original source documents.
  void Locate(std::vector<Diagnostic>& diagnostics) const;
};

/// Link lexical imports and private dependencies using host-supplied sources.
LinkedModule LinkModules(std::string source, std::string identity,
                         const ModuleSources& sources = {},
                         ModuleResolver resolver = {}, ModuleLimits limits = {},
                         CompilationBudget* absl_nullable budget = nullptr);
}  // namespace a11::flow
#endif  // A11_FLOW_MODULE_H_
