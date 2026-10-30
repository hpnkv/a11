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

#ifndef A11_FLOW_SINGLE_ITEM_H_
#define A11_FLOW_SINGLE_ITEM_H_

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "a11/flow/graph.h"
#include "a11/flow/limits.h"

namespace a11::flow {
/// Evidence for transport selection, independent of declared cardinality.
struct SingleItemFact {
  std::optional<int> upper;
  bool exactly_one_on_success = false;
  bool literal = false;
  bool eligible = false;
  bool pump_free = false;
  bool direct_transfer = false;
  std::vector<graph::StepId> writers;
  std::string reason = "unknown-producer";
  std::string pump_reason = "existing-producer-scheduling";
};

struct SingleItemAnalysis {
  std::vector<SingleItemFact> refs;
  bool complete = true;
};

/// Infer and verify prefix emission bounds, with a bounded optional work quota.
SingleItemAnalysis AnalyseSingleItems(const graph::FlowGraph& flow,
                                      CompilationBudget* budget = nullptr,
                                      size_t max_work = 200000);

/// Validate graph structure independently of transport candidate discovery.
bool CheckSingleItemStructure(const graph::FlowGraph& flow);

/// Explain transport decisions without changing the public source plan.
nlohmann::json SingleItemsToJson(const graph::FlowGraph& flow,
                                 const SingleItemAnalysis& analysis,
                                 bool enabled);
}  // namespace a11::flow
#endif  // A11_FLOW_SINGLE_ITEM_H_
