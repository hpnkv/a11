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

#ifndef A11_FLOW_INTERNAL_RUNTIME_LAYOUT_H_
#define A11_FLOW_INTERNAL_RUNTIME_LAYOUT_H_

#include "a11/flow/internal/call_handle.h"
#include "a11/flow/internal/frame.h"
#include "a11/flow/internal/runtime_transport.h"

namespace a11::flow {

struct RuntimeLayouts {
  struct WriterGroup {
    int remaining = 0;
    absl::Status failure;
    std::unique_ptr<
        absl::flat_hash_map<std::pair<const void*, graph::StepId>, a11::Task>>
        feeds;
  };

  enum class Phase : uint8_t { kPending, kExecuting, kDone };

  struct StepState {
    Phase phase = Phase::kPending;
    bool started = false;
    absl::Status completion;
    uint64_t order = 0;
    std::optional<absl::Status> outcome;
    std::optional<int64_t> winner;
  };

  struct Body {
    graph::Analysis analysis;
    internal::FrameLayout storage;
    internal::FrameLayout::Slots<StepState> steps;
    internal::FrameLayout::Slots<std::optional<Value>> captures;
    internal::FrameLayout::Slots<std::optional<Value>> folds;
    internal::FrameLayout::Slots<int> value_consumers;
    absl::flat_hash_map<size_t, size_t> capture_slots;
    internal::FrameObjectLayout<internal::CallHandle> calls;
    internal::FrameObjectLayout<internal::Lazy> lazies;
    internal::FrameObjectLayout<internal::Bus> buses;
    internal::FrameObjectLayout<internal::Buffer> buffers;
    internal::FrameObjectLayout<internal::Destination> destinations;
    internal::FrameObjectLayout<WriterGroup> writers;
    internal::FrameObjectLayout<internal::ValueCursor> cursors;
  };

  struct Flow {
    std::vector<std::unique_ptr<Body>> bodies;
    std::vector<size_t> step_slots;
    std::vector<size_t> step_captures;
    std::vector<size_t> joined_captures;
    std::vector<size_t> condition_captures;
    std::vector<size_t> fold_slots;
    size_t carry_capture = graph::kNone;
    size_t result_capture = graph::kNone;
  };

  absl::flat_hash_map<std::string, Flow> flows;
};

struct RuntimeAccess {
  static const RuntimeLayouts::Flow& Layout(const CompiledProgram& program,
                                            std::string_view flow) {
    return program.layouts_->flows.at(std::string(flow));
  }
};

namespace internal {
absl::StatusOr<std::shared_ptr<const RuntimeLayouts>> BuildRuntimeLayouts(
    const CompiledProgram& program);
}  // namespace internal

}  // namespace a11::flow
#endif  // A11_FLOW_INTERNAL_RUNTIME_LAYOUT_H_
