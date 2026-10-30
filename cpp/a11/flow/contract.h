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

#ifndef A11_FLOW_CONTRACT_H_
#define A11_FLOW_CONTRACT_H_

#include <string>
#include <string_view>

#include <absl/status/status.h>

#include "a11/flow/plan.h"

namespace a11::flow {
/// Whether available metadata proves payload refinement.
enum class Refinement { kYes, kNo, kUnknown };
/// Canonical payload spelling for alias and generic equality.
std::string CanonicalPayloadType(std::string_view type);
/// Compare payload types using native aliases and document shapes.
Refinement RefinesType(std::string_view actual, std::string_view expected,
                       const Program& program);
/// Conjoin compatible interfaces without changing executable identity.
absl::Status MergeContract(FlowPlan& effective, const FlowPlan& contribution,
                           const Program& program);
/// Validate the actual implementation against every declared obligation.
absl::Status CheckImplementation(const FlowPlan& actual,
                                 const FlowPlan& declared,
                                 const Program& program);
/// Encode a constant using the native action-header textual convention.
std::string HeaderBytes(const syntax::Constant& constant);
}  // namespace a11::flow
#endif  // A11_FLOW_CONTRACT_H_
