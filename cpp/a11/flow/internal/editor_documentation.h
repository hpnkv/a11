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

#ifndef A11_FLOW_INTERNAL_EDITOR_DOCUMENTATION_H_
#define A11_FLOW_INTERNAL_EDITOR_DOCUMENTATION_H_

#include <string>
#include <string_view>

#include <absl/container/flat_hash_map.h>

#include "a11/flow/module.h"

namespace a11::flow::internal {

/// Comment fallbacks for editor rendering, separate from semantic descriptions.
class EditorDocumentation {
 public:
  EditorDocumentation() = default;
  EditorDocumentation(std::string_view source, const ParseResult& parsed,
                      CompilationLimits limits,
                      CompilationBudget* absl_nullable budget);

  std::string_view Get(std::string_view description,
                       const syntax::Location& location) const;
  FlowPlan Flow(const FlowPlan& flow) const;
  DtoPlan Shape(const DtoPlan& shape) const;
  catalogue::Catalogue CatalogueAt(
      const LinkedModule& linked, size_t offset, bool selection,
      CompilationLimits limits, CompilationBudget* absl_nullable budget) const;

 private:
  absl::flat_hash_map<size_t, std::string> comments_;
  absl::flat_hash_map<std::string, const syntax::FlowDeclaration* absl_nonnull>
      flows_;
};

}  // namespace a11::flow::internal

#endif  // A11_FLOW_INTERNAL_EDITOR_DOCUMENTATION_H_
