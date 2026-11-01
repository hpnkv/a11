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

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status_macros.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/strings/str_split.h>

#include "a11/actions/action.h"
#include "a11/actions/reference_mirror.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/uuid.h"

namespace a11::actions {
namespace {
constexpr size_t kMaxStreamReferences = 4096;
}  // namespace

absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> Action::GetStreamSourcesNode(
    bool request_payload) {
  const auto accessible = GetNodeMap()->StorageIds();
  {
    thread::MutexLock lock(&mu_);
    streams_enabled_ = true;
    if (request_payload) {
      stream_reader_requested_ = true;
      std::vector<std::string> supported;
      const auto inherited = headers_.find(std::string(kStreamStorageHeader));
      for (const auto& identity : accessible) {
        if (inherited == headers_.end()) {
          supported.push_back(identity);
        } else {
          for (const auto candidate : absl::StrSplit(inherited->second, ',')) {
            if (candidate == identity) {
              supported.push_back(identity);
              break;
            }
          }
        }
      }
      headers_.insert_or_assign(std::string(kStreamStorageHeader),
                                absl::StrJoin(supported, ","));
    }
    headers_.insert_or_assign(
        std::string(kStreamScopeHeader),
        output_ids_.at(std::string(kActionStreamsOutput)));
  }
  return GetOutput(std::string(kActionStreamsOutput), false);
}

absl::Status Action::ExposeStreamSources(std::string port,
                                         std::string purpose) {
  {
    thread::MutexLock lock(&mu_);
    if (outputs_finished_ || !stream_references_.empty()) {
      return absl::FailedPreconditionError(
          "Expose stream sources before registering producers");
    }
    if (schema_.outputs.find(port) == schema_.outputs.end() ||
        purpose.empty()) {
      return absl::InvalidArgumentError(
          "A declared output and purpose are required");
    }
    stream_source_ports_.insert_or_assign(std::move(port), std::move(purpose));
  }
  return GetStreamSourcesNode(false).status();
}

absl::StatusOr<StreamReference> Action::RegisterStream(
    std::string purpose, data::Chunk source, std::string producer_id,
    std::string parent_producer_id) {
  if (producer_id.empty()) {
    producer_id = a11::NewUuid();
  }
  const std::string stream_name = absl::StrCat("stream_", a11::NewUuid());
  ABSL_ASSIGN_OR_RETURN(std::string node_id, MakeNodeId(GetId(), stream_name));
  StreamReference reference{
      .producer_id = std::move(producer_id),
      .parent_producer_id = std::move(parent_producer_id),
      .node = data::NodeRef{.id = node_id,
                            .offset = 0,
                            .length = std::nullopt,
                            .storage = GetNodeMap()->Storage()},
      .purpose = std::move(purpose),
      .source = std::move(source)};
  ABSL_RETURN_IF_ERROR(reference.Validate());
  {
    thread::MutexLock lock(&mu_);
    if (mode_ != Mode::kRun || outputs_finished_) {
      return absl::FailedPreconditionError(
          "Only a running action may register streams");
    }
    owned_stream_ids_.insert(node_id);
  }
  ABSL_RETURN_IF_ERROR(PublishStreamReference(reference));
  return reference;
}

absl::Status Action::ReceiveStreamReference(const StreamReference& reference) {
  ABSL_RETURN_IF_ERROR(reference.Validate());
  {
    thread::MutexLock lock(&mu_);
    if (mode_ != Mode::kCall || completion_status_.has_value()) {
      return absl::FailedPreconditionError(
          "Stream reference requires an active call");
    }
  }
  bool require_direct = false;
  {
    thread::MutexLock lock(&mu_);
    const auto capabilities = headers_.find(std::string(kStreamStorageHeader));
    if (stream_reader_requested_ && reference.node.storage.has_value() &&
        capabilities != headers_.end()) {
      for (const auto identity : absl::StrSplit(capabilities->second, ',')) {
        if (identity == reference.node.storage->id) {
          require_direct = true;
        }
      }
    }
  }
  if (require_direct) {
    ABSL_RETURN_IF_ERROR(GetNodeMap()->Resolve(reference.node).status());
  }
  return PublishStreamReference(reference);
}

absl::Status Action::PublishStreamReference(const StreamReference& reference) {
  ABSL_ASSIGN_OR_RETURN(data::Chunk chunk, StreamReferenceToChunk(reference));
  const std::string& producer = reference.producer_id;
  const std::string& purpose = reference.purpose;
  const auto source_map = GetNodeMap();
  auto current = shared_from_this();
  while (current != nullptr) {
    std::shared_ptr<Action> parent;
    std::shared_ptr<nodes::NodeMap> node_map;
    std::vector<std::string> ports;
    bool remote_run = false;
    bool direct_storage = false;
    bool duplicate = false;
    {
      thread::MutexLock lock(&current->mu_);
      parent = current->parent_.lock();
      node_map = current->node_map_;
      remote_run = current->mode_ == Mode::kRun &&
                   current->stream_ != nullptr && parent == nullptr;
      if (remote_run && reference.node.storage.has_value()) {
        const auto capabilities =
            current->headers_.find(std::string(kStreamStorageHeader));
        if (capabilities != current->headers_.end()) {
          for (const auto identity :
               absl::StrSplit(capabilities->second, ',')) {
            if (identity == reference.node.storage->id) {
              direct_storage = true;
            }
          }
        }
        if (direct_storage) {
          current->direct_stream_ids_.insert(reference.node.id);
        }
      }
      const auto found = current->stream_references_.find(producer);
      if (found != current->stream_references_.end()) {
        if (found->second != reference) {
          return absl::AlreadyExistsError(
              "Conflicting stream producer identity");
        }
        duplicate = true;
      } else {
        if (current->outputs_finished_) {
          return absl::FailedPreconditionError("The stream scope has finished");
        }
        if (current->stream_references_.size() >= kMaxStreamReferences) {
          return absl::ResourceExhaustedError(
              "Stream scope exceeds 4096 producers");
        }
        current->streams_enabled_ = true;
        current->stream_references_.emplace(producer, reference);
        ports.emplace_back(kActionStreamsOutput);
        for (const auto& [port, wanted] : current->stream_source_ports_) {
          if (wanted == purpose) {
            ports.push_back(port);
          }
        }
      }
    }
    if (!duplicate) {
      if (node_map == nullptr) {
        return absl::FailedPreconditionError("Stream scope has no node map");
      }
      ABSL_RETURN_IF_ERROR(
          node_map->ImportReference(reference.node, source_map).status());
      data::Chunk published_chunk = chunk;
      if (remote_run && !direct_storage) {
        StreamReference published = reference;
        ABSL_ASSIGN_OR_RETURN(
            published.node,
            current->MirrorRegistry()->Rewrite(reference.node, source_map));
        ABSL_ASSIGN_OR_RETURN(published_chunk,
                              StreamReferenceToChunk(published));
      }
      for (const std::string& port : ports) {
        ABSL_ASSIGN_OR_RETURN(auto output,
                              current->GetOutput(port, remote_run));
        ABSL_RETURN_IF_ERROR(
            output->PutChunk(published_chunk).Await().status());
        if (remote_run && port == kActionStreamsOutput) {
          ABSL_RETURN_IF_ERROR(output->WaitForBufferToDrain().Await().status());
        }
      }
    }
    current = std::move(parent);
  }
  return absl::OkStatus();
}

absl::Status Action::WaitForStreamChildren() {
  std::vector<std::shared_ptr<Action>> children;
  {
    thread::MutexLock lock(&mu_);
    if (!streams_enabled_ &&
        headers_.find(std::string(kStreamScopeHeader)) == headers_.end()) {
      return absl::OkStatus();
    }
    children.assign(children_.begin(), children_.end());
  }
  for (const auto& child : children) {
    if (child->HasBeenRun() || child->HasBeenCalled()) {
      child->Wait().Await().status().IgnoreError();
    }
  }
  return absl::OkStatus();
}
}  // namespace a11::actions
