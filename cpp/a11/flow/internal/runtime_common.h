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

#ifndef A11_FLOW_INTERNAL_RUNTIME_COMMON_H_
#define A11_FLOW_INTERNAL_RUNTIME_COMMON_H_

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/base/thread_annotations.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/container/inlined_vector.h>
#include <absl/functional/any_invocable.h>
#include <absl/functional/function_ref.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/strings/str_replace.h>
#include <absl/strings/str_split.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <absl/types/span.h>
#include <nlohmann/json.hpp>

#include "a11/actions/registry.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/concurrency/parallel.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/flow/contract.h"
#include "a11/flow/emit_json.h"
#include "a11/flow/runtime.h"
#include "a11/flow/schema.h"
#include "a11/flow/vocabulary.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/status.h"
#include "a11/stores/chunk_store_writer.h"
#include "a11/utf8.h"
#include "thread/boost_primitives.h"

namespace a11::flow {
struct ExecutionSnapshot {
  absl::flat_hash_map<std::string,
                      std::pair<actions::ActionSchema, actions::ActionHandler>>
      external;
  bool sealed = false;
};

}  // namespace a11::flow

namespace a11::flow::internal {

using graph::BodyId;
using graph::ExprId;
using graph::FlowGraph;
using graph::kNone;
using graph::RefId;
using graph::RefKind;
using graph::StepId;
using graph::StepKind;

class Item;
using ItemPtr = std::shared_ptr<Item>;
class Reader;
using ReaderPtr = std::unique_ptr<Reader>;
using NodePtr = std::shared_ptr<nodes::AsyncNode>;

inline constexpr size_t kStackSize = 256 * 1024;

inline thread::TreeOptions StackOptions() {
  return thread::TreeOptions{.stack_size = kStackSize};
}

inline absl::Status Fail(
    std::string_view message,
    absl::StatusCode code = absl::StatusCode::kInvalidArgument) {
  return {code, message};
}

std::string SubjectOf(const graph::Step& step);
absl::StatusOr<Value> Divide(const Value& total, const Value& count);
void PrimeBatch(const std::vector<ItemPtr>& items,
                HostBridge* absl_nonnull bridge);
std::string BaseMimetype(std::string_view mimetype);
bool Matches(std::string_view name, std::string_view pattern);
bool MatchesFolded(std::string_view name, std::string_view pattern);
absl::Status CloseNode(const NodePtr& node);

}  // namespace a11::flow::internal
#endif  // A11_FLOW_INTERNAL_RUNTIME_COMMON_H_
