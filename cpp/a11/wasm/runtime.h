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

#ifndef A11_WASM_RUNTIME_H_
#define A11_WASM_RUNTIME_H_

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/data/types.h"

namespace a11::wasm {

// Single-threaded runtime. The host drives action callbacks and persistence.
class Runtime {
 public:
  explicit Runtime(std::string storage_id);

  absl::Status RegisterAction(std::string name);
  absl::Status Invoke(data::ActionMessage action);
  absl::StatusOr<std::optional<data::ActionMessage>> NextAction();
  absl::Status FinishAction(std::string_view id, absl::Status status);

  absl::Status Append(data::NodeFragment fragment);
  absl::StatusOr<std::uint32_t> Put(data::NodeFragment fragment);
  absl::StatusOr<std::vector<std::uint32_t>> PutMany(
      std::vector<data::NodeFragment> fragments);
  absl::Status Close(std::string_view id, absl::Status status);
  absl::StatusOr<data::NodeFragment> Read(std::string_view id,
                                          size_t index) const;
  absl::StatusOr<data::NodeFragment> ReadArrival(std::string_view id,
                                                 size_t arrival) const;
  absl::StatusOr<data::NodeFragment> ClearData(std::string_view id,
                                               std::uint32_t seq);
  absl::StatusOr<size_t> Count(std::string_view id) const;
  absl::StatusOr<std::uint32_t> ArrivalSeq(std::string_view id,
                                           size_t arrival) const;
  absl::StatusOr<std::optional<std::uint32_t>> FinalSeq(
      std::string_view id) const;
  absl::StatusOr<bool> Closed(std::string_view id) const;
  absl::StatusOr<std::vector<data::Chunk>> Resolve(
      const data::NodeRef& ref, size_t max_depth = 8,
      size_t max_chunks = 1024) const;
  absl::StatusOr<std::string> ExportSnapshot() const;
  absl::Status ImportSnapshot(std::string_view bytes);

 private:
  struct Node {
    absl::flat_hash_map<std::uint32_t, data::NodeFragment> fragments{};
    std::vector<std::uint32_t> arrival{};
    std::optional<std::uint32_t> final_seq{};
    bool closed = false;
    absl::Status status{};
  };

  struct Invocation {
    data::ActionMessage message{};
    bool running = false;
    bool finished = false;
    absl::Status status{};
  };

  absl::Status ResolveInto(const data::NodeRef& ref, size_t depth,
                           size_t max_chunks,
                           absl::flat_hash_set<std::string>* seen,
                           std::vector<data::Chunk>* output) const;

  std::string storage_id_;
  absl::flat_hash_map<std::string, Node> nodes_;
  absl::flat_hash_set<std::string> actions_;
  absl::flat_hash_map<std::string, Invocation> invocations_;
  std::deque<std::string> pending_;
};

}  // namespace a11::wasm

#endif  // A11_WASM_RUNTIME_H_
