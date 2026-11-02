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

#include "runtime.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <absl/status/status_macros.h>

namespace a11::wasm {

Runtime::Runtime(std::string storage_id) : storage_id_(std::move(storage_id)) {}

absl::Status Runtime::RegisterAction(std::string name) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(name));
  if (!actions_.insert(std::move(name)).second) {
    return absl::AlreadyExistsError("action is already registered");
  }
  return absl::OkStatus();
}

absl::Status Runtime::Invoke(data::ActionMessage action) {
  ABSL_RETURN_IF_ERROR(action.Validate());
  if (actions_.find(action.name) == actions_.end()) {
    return absl::NotFoundError("action is not registered");
  }
  const std::string id = action.id;
  if (invocations_.find(id) != invocations_.end()) {
    return absl::AlreadyExistsError("action id is already in use");
  }
  for (const auto& port : action.inputs) {
    nodes_.try_emplace(port.id);
  }
  for (const auto& port : action.outputs) {
    nodes_.try_emplace(port.id);
  }
  invocations_.emplace(id, Invocation{std::move(action)});
  pending_.push_back(id);
  return absl::OkStatus();
}

absl::StatusOr<std::optional<data::ActionMessage>> Runtime::NextAction() {
  if (pending_.empty()) {
    return std::nullopt;
  }
  auto& invocation = invocations_.at(pending_.front());
  invocation.running = true;
  pending_.pop_front();
  return invocation.message;
}

absl::Status Runtime::FinishAction(std::string_view id, absl::Status status) {
  auto it = invocations_.find(id);
  if (it == invocations_.end()) {
    return absl::NotFoundError("action id not found");
  }
  if (!it->second.running || it->second.finished) {
    return absl::FailedPreconditionError("action is not running");
  }
  it->second.finished = true;
  it->second.status = status;
  for (const auto& port : it->second.message.outputs) {
    auto& node = nodes_.at(port.id);
    if (!node.closed) {
      node.closed = true;
      node.status = status;
    }
  }
  return absl::OkStatus();
}

absl::Status Runtime::Append(data::NodeFragment fragment) {
  fragment.continued = true;
  return Put(std::move(fragment)).status();
}

absl::StatusOr<std::uint32_t> Runtime::Put(data::NodeFragment fragment) {
  ABSL_RETURN_IF_ERROR(fragment.Validate());
  auto& node = nodes_[fragment.id];
  if (node.closed) {
    return absl::FailedPreconditionError("node is closed");
  }
  if (node.arrival.size() > std::numeric_limits<std::uint32_t>::max()) {
    return absl::OutOfRangeError("node sequence is exhausted");
  }
  std::uint64_t candidate = node.arrival.size();
  if (!fragment.seq.has_value()) {
    while (candidate <= std::numeric_limits<std::uint32_t>::max() &&
           node.fragments.find(static_cast<std::uint32_t>(candidate)) !=
               node.fragments.end()) {
      ++candidate;
    }
    if (candidate > std::numeric_limits<std::uint32_t>::max()) {
      return absl::OutOfRangeError("node sequence is exhausted");
    }
    fragment.seq = static_cast<std::uint32_t>(candidate);
  }
  const auto seq = *fragment.seq;
  if (node.fragments.find(seq) != node.fragments.end()) {
    return absl::AlreadyExistsError("fragment sequence already exists");
  }
  if (node.final_seq.has_value() && seq > *node.final_seq) {
    return absl::OutOfRangeError("fragment exceeds final sequence");
  }
  if (!fragment.continued) {
    if (node.final_seq.has_value() && *node.final_seq != seq) {
      return absl::FailedPreconditionError("node has another final sequence");
    }
    for (const auto& [existing, unused] : node.fragments) {
      (void)unused;
      if (existing > seq) {
        return absl::OutOfRangeError(
            "existing fragment exceeds final sequence");
      }
    }
    node.final_seq = seq;
  }
  node.fragments.emplace(seq, std::move(fragment));
  node.arrival.push_back(seq);
  return seq;
}

absl::StatusOr<std::vector<std::uint32_t>> Runtime::PutMany(
    std::vector<data::NodeFragment> fragments) {
  if (fragments.empty()) {
    return std::vector<std::uint32_t>{};
  }
  const std::string id = fragments.front().id;
  const bool explicit_seq = fragments.front().seq.has_value();
  for (const auto& fragment : fragments) {
    ABSL_RETURN_IF_ERROR(fragment.Validate());
    if (fragment.id != id) {
      return absl::InvalidArgumentError("batch fragments use different nodes");
    }
    if (fragment.seq.has_value() != explicit_seq) {
      return absl::InvalidArgumentError("batch mixes explicit sequences");
    }
  }
  Runtime trial(storage_id_);
  if (auto found = nodes_.find(id); found != nodes_.end()) {
    trial.nodes_.emplace(id, found->second);
  }
  std::vector<std::uint32_t> assigned;
  assigned.reserve(fragments.size());
  for (auto& fragment : fragments) {
    ABSL_ASSIGN_OR_RETURN(auto seq, trial.Put(std::move(fragment)));
    assigned.push_back(seq);
  }
  nodes_.insert_or_assign(id, std::move(trial.nodes_.at(id)));
  return assigned;
}

absl::Status Runtime::Close(std::string_view id, absl::Status status) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(id));
  auto& node = nodes_[std::string(id)];
  if (node.closed) {
    return absl::FailedPreconditionError("node is closed");
  }
  node.closed = true;
  node.status = std::move(status);
  return absl::OkStatus();
}

absl::StatusOr<data::NodeFragment> Runtime::Read(std::string_view id,
                                                 size_t index) const {
  const auto it = nodes_.find(id);
  if (it == nodes_.end()) {
    return absl::NotFoundError("node not found");
  }
  if (index > std::numeric_limits<std::uint32_t>::max()) {
    return absl::OutOfRangeError("sequence exceeds node range");
  }
  const auto found =
      it->second.fragments.find(static_cast<std::uint32_t>(index));
  if (found == it->second.fragments.end()) {
    if (it->second.closed && !it->second.status.ok()) {
      return it->second.status;
    }
    return it->second.closed
               ? absl::OutOfRangeError("end of node")
               : absl::UnavailableError("node has no fragment yet");
  }
  data::NodeFragment fragment = found->second;
  fragment.continued =
      !it->second.final_seq.has_value() || index < *it->second.final_seq;
  return fragment;
}

absl::StatusOr<data::NodeFragment> Runtime::ReadArrival(std::string_view id,
                                                        size_t arrival) const {
  const auto it = nodes_.find(id);
  if (it == nodes_.end()) {
    return absl::NotFoundError("node not found");
  }
  if (arrival >= it->second.arrival.size()) {
    if (it->second.closed && !it->second.status.ok()) {
      return it->second.status;
    }
    return it->second.closed
               ? absl::OutOfRangeError("end of node")
               : absl::UnavailableError("node has no fragment yet");
  }
  return Read(id, it->second.arrival[arrival]);
}

absl::StatusOr<data::NodeFragment> Runtime::ClearData(std::string_view id,
                                                      std::uint32_t seq) {
  auto it = nodes_.find(id);
  if (it == nodes_.end()) {
    return absl::NotFoundError("node not found");
  }
  auto found = it->second.fragments.find(seq);
  if (found == it->second.fragments.end()) {
    return absl::NotFoundError("fragment not found");
  }
  auto* chunk = std::get_if<data::Chunk>(&found->second.data);
  if (chunk == nullptr) {
    return absl::UnimplementedError("clearing a reference is unsupported");
  }
  data::NodeFragment original = found->second;
  data::Chunk tombstone;
  tombstone.metadata = chunk->metadata;
  tombstone.ref = "__tombstone__";
  found->second.data = std::move(tombstone);
  return original;
}

absl::StatusOr<size_t> Runtime::Count(std::string_view id) const {
  const auto it = nodes_.find(id);
  if (it == nodes_.end()) {
    return absl::NotFoundError("node not found");
  }
  return it->second.arrival.size();
}

absl::StatusOr<std::uint32_t> Runtime::ArrivalSeq(std::string_view id,
                                                  size_t arrival) const {
  const auto it = nodes_.find(id);
  if (it == nodes_.end()) {
    return absl::NotFoundError("node not found");
  }
  if (arrival >= it->second.arrival.size()) {
    return absl::NotFoundError("arrival order not found");
  }
  return it->second.arrival[arrival];
}

absl::StatusOr<std::optional<std::uint32_t>> Runtime::FinalSeq(
    std::string_view id) const {
  const auto it = nodes_.find(id);
  if (it == nodes_.end()) {
    return absl::NotFoundError("node not found");
  }
  return it->second.final_seq;
}

absl::StatusOr<bool> Runtime::Closed(std::string_view id) const {
  const auto it = nodes_.find(id);
  if (it == nodes_.end()) {
    return absl::NotFoundError("node not found");
  }
  return it->second.closed;
}

absl::Status Runtime::ResolveInto(const data::NodeRef& ref, size_t depth,
                                  size_t max_chunks,
                                  absl::flat_hash_set<std::string>* seen,
                                  std::vector<data::Chunk>* output) const {
  if (depth == 0) {
    return absl::ResourceExhaustedError("reference depth exceeded");
  }
  if (ref.storage.has_value() && ref.storage->id != storage_id_) {
    return absl::UnavailableError("reference storage needs a host resolver");
  }
  if (!seen->insert(ref.id).second) {
    return absl::FailedPreconditionError("reference cycle");
  }
  const auto it = nodes_.find(ref.id);
  if (it == nodes_.end()) {
    return absl::NotFoundError("referenced node not found");
  }
  const auto& fragments = it->second.fragments;
  const std::uint64_t start = ref.offset;
  const std::uint64_t end =
      ref.length.has_value() ? start + *ref.length
      : it->second.final_seq.has_value()
          ? static_cast<std::uint64_t>(*it->second.final_seq) + 1
          : it->second.arrival.size();
  if (end >
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) +
          1) {
    return absl::OutOfRangeError("reference range exceeds node");
  }
  for (std::uint64_t i = start; i < end; ++i) {
    auto found = fragments.find(static_cast<std::uint32_t>(i));
    if (found == fragments.end()) {
      return absl::UnavailableError("reference fragment is unavailable");
    }
    if (std::holds_alternative<data::Chunk>(found->second.data)) {
      if (output->size() >= max_chunks) {
        return absl::ResourceExhaustedError("reference chunk limit exceeded");
      }
      output->push_back(std::get<data::Chunk>(found->second.data));
    } else {
      ABSL_RETURN_IF_ERROR(
          ResolveInto(std::get<data::NodeRef>(found->second.data), depth - 1,
                      max_chunks, seen, output));
    }
  }
  seen->erase(ref.id);
  return absl::OkStatus();
}

absl::StatusOr<std::vector<data::Chunk>> Runtime::Resolve(
    const data::NodeRef& ref, size_t max_depth, size_t max_chunks) const {
  ABSL_RETURN_IF_ERROR(ref.Validate());
  absl::flat_hash_set<std::string> seen;
  std::vector<data::Chunk> output;
  ABSL_RETURN_IF_ERROR(ResolveInto(ref, max_depth, max_chunks, &seen, &output));
  return output;
}

absl::StatusOr<std::string> Runtime::ExportSnapshot() const {
  data::WireMessage snapshot;
  snapshot.headers.emplace("wasm_snapshot", "1");
  snapshot.headers.emplace("wasm_storage", storage_id_);
  for (const auto& [id, node] : nodes_) {
    for (const auto seq : node.arrival) {
      data::NodeFragment fragment = node.fragments.at(seq);
      fragment.continued = !node.final_seq.has_value() || seq < *node.final_seq;
      snapshot.node_fragments.push_back(std::move(fragment));
    }
    if (node.closed) {
      ABSL_ASSIGN_OR_RETURN(data::Chunk closure,
                            data::MakeStatusChunk(node.status, true));
      data::NodeFragment marker;
      marker.id = id;
      marker.data = std::move(closure);
      marker.seq = static_cast<std::uint32_t>(node.arrival.size());
      snapshot.node_fragments.push_back(std::move(marker));
    }
  }
  return snapshot.ToMsgpack();
}

absl::Status Runtime::ImportSnapshot(std::string_view bytes) {
  if (!nodes_.empty() || !invocations_.empty()) {
    return absl::FailedPreconditionError("runtime already has state");
  }
  ABSL_ASSIGN_OR_RETURN(data::WireMessage snapshot,
                        data::WireMessage::FromMsgpack(bytes));
  if (!snapshot.actions.empty()) {
    return absl::InvalidArgumentError("snapshot contains actions");
  }
  const auto version = snapshot.headers.find("wasm_snapshot");
  const auto storage = snapshot.headers.find("wasm_storage");
  if (version == snapshot.headers.end() || version->second != "1" ||
      storage == snapshot.headers.end() || storage->second != storage_id_) {
    return absl::InvalidArgumentError("snapshot version or storage differs");
  }
  Runtime restored(storage_id_);
  for (auto& fragment : snapshot.node_fragments) {
    if (auto* chunk = std::get_if<data::Chunk>(&fragment.data);
        chunk != nullptr && data::IsCloseStatusChunk(*chunk)) {
      absl::StatusOr<absl::Status> outcome =
          data::StatusFromStatusChunk(*chunk);
      if (!outcome.ok()) {
        return outcome.status();
      }
      ABSL_RETURN_IF_ERROR(restored.Close(fragment.id, *outcome));
    } else {
      ABSL_RETURN_IF_ERROR(restored.Put(std::move(fragment)).status());
    }
  }
  nodes_ = std::move(restored.nodes_);
  return absl::OkStatus();
}

}  // namespace a11::wasm
