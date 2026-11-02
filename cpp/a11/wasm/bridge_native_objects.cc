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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/escaping.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/actions/describe.h"
#include "a11/actions/registry.h"
#include "a11/concurrency/executor.h"
#include "a11/data/json.h"
#include "a11/json_codec.h"
#include "a11/net/wire_stream.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/service/session.h"
#include "a11/status.h"
#include "a11/stores/chunk_store.h"
#include "async_task.h"
#include "bridge_core.h"
#include "native_objects.h"
#include "sdk/flow/actions/time_actions.h"
#include "thread/wasm_system_actions.h"
#include "thread/introspect.h"

namespace {

absl::Status EnsureInitialized(
    const std::shared_ptr<a11::wasm::NativeGraph>& graph) {
  thread::MutexLock lock(&graph->mu);
  if (graph->initialized) {
    return graph->startup;
  }
  graph->initialized = true;
  auto stores = std::make_shared<a11::wasm::StoreRegistry>();
  auto nodes = a11::wasm::MakeTrackedNodeMap(stores, graph->storage_id);
  if (!nodes.ok()) {
    return graph->startup = nodes.status();
  }
  auto registry = std::make_shared<a11::actions::ActionRegistry>();
  graph->startup = a11::sdk::flow::RegisterTimeActions(*registry);
  if (graph->startup.ok()) {
    graph->startup = a11::wasm::RegisterSystemActions(
        *registry, graph->host_settings.environment,
        graph->host_settings.register_environment);
  }
  if (graph->startup.ok()) {
    for (const auto& [name, enabled] :
         {std::pair{"make_http_request",
                    graph->host_settings.register_make_http_request},
          std::pair{"web-fetch", graph->host_settings.register_web_fetch}}) {
      if (!enabled) continue;
      auto schema = a11::wasm::SchemaFromCatalogue(name);
      if (!schema.ok()) return graph->startup = schema.status();
      graph->startup = a11::wasm::RegisterHostAction(
          *registry, std::move(*schema), graph);
      if (!graph->startup.ok()) break;
    }
  }
  if (!graph->startup.ok()) {
    return graph->startup;
  }
  graph->nodes = std::move(*nodes);
  graph->stores = std::move(stores);
  graph->registry = std::move(registry);
  return absl::OkStatus();
}

absl::StatusOr<std::string> RequiredString(const nlohmann::json& request,
                                           const char* name) {
  const auto found = request.find(name);
  if (found == request.end() || !found->is_string()) {
    return absl::InvalidArgumentError(std::string(name) + " must be a string");
  }
  return found->get<std::string>();
}

absl::Status ReadChannelConfig(
    const nlohmann::json& request, a11::net::WireStreamOptions* options,
    a11::net::ChannelFramingOptions* framing,
    a11::net::ChannelEndpointRole* role) {
  const auto read_size = [](const nlohmann::json& object, const char* name,
                            size_t* target) -> absl::Status {
    const auto value = object.find(name);
    if (value == object.end()) return absl::OkStatus();
    if (!value->is_number_unsigned()) {
      return absl::InvalidArgumentError(std::string(name) + " must be unsigned");
    }
    *target = value->get<size_t>();
    return absl::OkStatus();
  };
  if (const auto configured = request.find("options");
      configured != request.end()) {
    if (!configured->is_object()) {
      return absl::InvalidArgumentError("channel options must be an object");
    }
    ABSL_RETURN_IF_ERROR(read_size(*configured, "maxBufferedIncomingMessages",
                                   &options->max_buffered_incoming_messages));
    ABSL_RETURN_IF_ERROR(read_size(*configured, "maxSingleMessageSize",
                                   &options->max_single_message_size));
    ABSL_RETURN_IF_ERROR(read_size(*configured, "maxBufferedIncomingBytes",
                                   &options->max_buffered_incoming_bytes));
    if (const auto timeout = configured->find("messageTimeoutMs");
        timeout != configured->end()) {
      if (timeout->is_null()) {
        options->message_timeout = absl::InfiniteDuration();
      } else if (timeout->is_number_unsigned()) {
        options->message_timeout =
            absl::Milliseconds(timeout->get<std::uint64_t>());
      } else {
        return absl::InvalidArgumentError("messageTimeoutMs is invalid");
      }
    }
    if (const auto deadline = configured->find("deadline");
        deadline != configured->end()) {
      if (deadline->is_null()) {
        options->deadline = absl::InfiniteFuture();
      } else if (deadline->is_number_integer()) {
        options->deadline =
            absl::FromUnixMillis(deadline->get<std::int64_t>());
      } else {
        return absl::InvalidArgumentError("channel deadline is invalid");
      }
    }
  }
  if (const auto configured = request.find("framing");
      configured != request.end()) {
    if (!configured->is_object()) {
      return absl::InvalidArgumentError("channel framing must be an object");
    }
    ABSL_RETURN_IF_ERROR(
        read_size(*configured, "splitSize", &framing->split_size));
    ABSL_RETURN_IF_ERROR(read_size(*configured, "maxPendingMessages",
                                   &framing->max_pending_messages));
    ABSL_RETURN_IF_ERROR(read_size(*configured, "maxPendingBytes",
                                   &framing->max_pending_bytes));
  }
  if (const auto value = request.find("role"); value != request.end()) {
    if (!value->is_string()) {
      return absl::InvalidArgumentError("channel role must be a string");
    }
    const auto name = value->get<std::string>();
    if (name == "client") *role = a11::net::ChannelEndpointRole::kClient;
    else if (name == "server") *role = a11::net::ChannelEndpointRole::kServer;
    else if (name == "either") *role = a11::net::ChannelEndpointRole::kEither;
    else return absl::InvalidArgumentError("unknown channel role");
  }
  ABSL_RETURN_IF_ERROR(options->Validate());
  return framing->Validate();
}

absl::StatusOr<a11::actions::ActionSettings> ReadActionSettings(
    const nlohmann::json& value, a11::actions::ActionSettings settings) {
  if (!value.is_object()) {
    return absl::InvalidArgumentError("settings must be an object");
  }
  for (const auto& [name, target] :
       {std::pair{"bindStreamsOnInputsByDefault",
                  &settings.bind_streams_on_inputs_by_default},
        std::pair{"bindStreamsOnOutputsByDefault",
                  &settings.bind_streams_on_outputs_by_default}}) {
    if (auto found = value.find(name); found != value.end()) {
      if (found->is_null()) {
        *target = std::nullopt;
      } else if (found->is_boolean()) {
        *target = found->get<bool>();
      } else {
        return absl::InvalidArgumentError(std::string(name) +
                                          " must be boolean or null");
      }
    }
  }
  for (const auto& [name, target] :
       {std::pair{"clearInputsAfterRun", &settings.clear_inputs_after_run},
        std::pair{"clearOutputsAfterRun", &settings.clear_outputs_after_run}}) {
    if (auto found = value.find(name); found != value.end()) {
      if (!found->is_boolean()) {
        return absl::InvalidArgumentError(std::string(name) +
                                          " must be boolean");
      }
      *target = found->get<bool>();
    }
  }
  return settings;
}

absl::Status ReadBoolField(const nlohmann::json& value, const char* name,
                           bool* output) {
  if (auto found = value.find(name); found != value.end()) {
    if (!found->is_boolean()) {
      return absl::InvalidArgumentError(std::string(name) +
                                        " must be boolean");
    }
    *output = found->get<bool>();
  }
  return absl::OkStatus();
}

template <typename T>
absl::Status ReadUnsignedField(const nlohmann::json& value, const char* name,
                               T* output) {
  if (auto found = value.find(name); found != value.end()) {
    if (!found->is_number_unsigned() ||
        found->get<std::uint64_t>() > std::numeric_limits<T>::max()) {
      return absl::InvalidArgumentError(std::string(name) +
                                        " must be an unsigned integer");
    }
    *output = found->get<T>();
  }
  return absl::OkStatus();
}

absl::Status ReadOptionalUnsignedField(
    const nlohmann::json& value, const char* name,
    std::optional<std::uint64_t>* output) {
  if (auto found = value.find(name); found != value.end()) {
    if (found->is_null()) {
      *output = std::nullopt;
    } else if (found->is_number_unsigned()) {
      *output = found->get<std::uint64_t>();
    } else {
      return absl::InvalidArgumentError(std::string(name) +
                                        " must be unsigned or null");
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<a11::stores::ChunkStoreReaderOptions> ReadReaderOptions(
    const nlohmann::json& value,
    a11::stores::ChunkStoreReaderOptions options) {
  if (!value.is_object()) {
    return absl::InvalidArgumentError("reader options must be an object");
  }
  ABSL_RETURN_IF_ERROR(ReadBoolField(value, "ordered", &options.ordered));
  ABSL_RETURN_IF_ERROR(ReadBoolField(value, "popChunks", &options.pop_chunks));
  ABSL_RETURN_IF_ERROR(ReadUnsignedField(value, "numChunksToBuffer",
                                         &options.num_chunks_to_buffer));
  ABSL_RETURN_IF_ERROR(ReadUnsignedField(value, "offset", &options.offset));
  ABSL_RETURN_IF_ERROR(ReadOptionalUnsignedField(
      value, "maxChunksToRead", &options.max_chunks_to_read));
  ABSL_RETURN_IF_ERROR(ReadBoolField(value, "stickyMimetype",
                                     &options.sticky_mimetype));
  ABSL_RETURN_IF_ERROR(ReadBoolField(value, "followNodeRefs",
                                     &options.follow_node_refs));
  ABSL_RETURN_IF_ERROR(ReadUnsignedField(value, "maxRefDepth",
                                         &options.max_ref_depth));
  ABSL_RETURN_IF_ERROR(ReadUnsignedField(value, "maxRefBreadth",
                                         &options.max_ref_breadth));
  ABSL_RETURN_IF_ERROR(options.Validate());
  return options;
}

absl::StatusOr<a11::stores::ChunkStoreWriterOptions> ReadWriterOptions(
    const nlohmann::json& value,
    a11::stores::ChunkStoreWriterOptions options) {
  if (!value.is_object()) {
    return absl::InvalidArgumentError("writer options must be an object");
  }
  ABSL_RETURN_IF_ERROR(ReadUnsignedField(value, "offset", &options.offset));
  ABSL_RETURN_IF_ERROR(ReadUnsignedField(value, "maxChunksToWriteAtOnce",
                                         &options.max_chunks_to_write_at_once));
  ABSL_RETURN_IF_ERROR(ReadOptionalUnsignedField(
      value, "numChunksToBuffer", &options.num_chunks_to_buffer));
  ABSL_RETURN_IF_ERROR(ReadBoolField(value, "stickyMimetype",
                                     &options.sticky_mimetype));
  ABSL_RETURN_IF_ERROR(options.Validate());
  return options;
}

nlohmann::json WriteReaderOptions(
    const a11::stores::ChunkStoreReaderOptions& options) {
  return nlohmann::json{
      {"ordered", options.ordered},
      {"popChunks", options.pop_chunks},
      {"numChunksToBuffer", options.num_chunks_to_buffer},
      {"offset", options.offset},
      {"maxChunksToRead", options.max_chunks_to_read},
      {"stickyMimetype", options.sticky_mimetype},
      {"followNodeRefs", options.follow_node_refs},
      {"maxRefDepth", options.max_ref_depth},
      {"maxRefBreadth", options.max_ref_breadth}};
}

nlohmann::json WriteWriterOptions(
    const a11::stores::ChunkStoreWriterOptions& options) {
  return nlohmann::json{
      {"offset", options.offset},
      {"maxChunksToWriteAtOnce", options.max_chunks_to_write_at_once},
      {"numChunksToBuffer", options.num_chunks_to_buffer},
      {"stickyMimetype", options.sticky_mimetype}};
}

absl::StatusOr<a11::actions::ActionSchema> ReadActionSchema(
    const nlohmann::json& value) {
  ABSL_ASSIGN_OR_RETURN(auto schema, a11::actions::SchemaFromJson(value));
  if (auto headers = value.find("headers");
      headers != value.end() && headers->is_array()) {
    for (const auto& entry : *headers) {
      if (!entry.is_object()) continue;
      const auto name = entry.find("name");
      const auto encoded = entry.find("default_base64");
      if (name == entry.end() || !name->is_string() ||
          encoded == entry.end()) {
        continue;
      }
      if (!encoded->is_string()) {
        return absl::InvalidArgumentError(
            "header default_base64 must be a string");
      }
      auto found = schema.headers.find(name->get<std::string>());
      if (found == schema.headers.end()) continue;
      std::string decoded;
      if (!absl::Base64Unescape(encoded->get<std::string>(), &decoded)) {
        return absl::InvalidArgumentError(
            "header default_base64 is invalid");
      }
      found->second.default_value = std::move(decoded);
    }
  }
  if (auto inputs = value.find("inputs");
      inputs != value.end() && inputs->is_array()) {
    for (const auto& entry : *inputs) {
      if (!entry.is_object()) continue;
      const auto name = entry.find("name");
      const auto autofills = entry.find("autofills");
      if (name == entry.end() || !name->is_string() ||
          autofills == entry.end()) {
        continue;
      }
      if (!autofills->is_array()) {
        return absl::InvalidArgumentError("input autofills must be an array");
      }
      auto found = schema.inputs.find(name->get<std::string>());
      if (found == schema.inputs.end()) continue;
      for (const auto& fragment : *autofills) {
        if (fragment.is_null()) {
          found->second.autofills.emplace_back(std::nullopt);
        } else {
          ABSL_ASSIGN_OR_RETURN(
              auto decoded,
              a11::data::NodeFragmentFromJsonValue(fragment));
          found->second.autofills.emplace_back(std::move(decoded));
        }
      }
    }
  }
  ABSL_RETURN_IF_ERROR(schema.Validate());
  return schema;
}

absl::StatusOr<nlohmann::json> WriteActionSchema(
    const a11::actions::ActionSchema& schema, bool runnable) {
  nlohmann::json value = a11::actions::SchemaToJson(
      schema, runnable, a11::actions::PortView::kAll);
  if (auto inputs = value.find("inputs");
      inputs != value.end() && inputs->is_array()) {
    for (auto& entry : *inputs) {
      const auto name = entry.find("name");
      if (name == entry.end() || !name->is_string()) continue;
      auto found = schema.inputs.find(name->get<std::string>());
      if (found == schema.inputs.end() ||
          found->second.autofills.empty()) {
        continue;
      }
      nlohmann::json autofills = nlohmann::json::array();
      for (const auto& fragment : found->second.autofills) {
        if (!fragment.has_value()) {
          autofills.push_back(nullptr);
        } else {
          ABSL_ASSIGN_OR_RETURN(auto encoded,
                                a11::data::NodeFragmentToJsonValue(*fragment));
          autofills.push_back(std::move(encoded));
        }
      }
      entry["autofills"] = std::move(autofills);
    }
  }
  if (auto headers = value.find("headers");
      headers != value.end() && headers->is_array()) {
    for (auto& entry : *headers) {
      const auto name = entry.find("name");
      if (name == entry.end() || !name->is_string()) continue;
      auto found = schema.headers.find(name->get<std::string>());
      if (found == schema.headers.end() ||
          !found->second.default_value.has_value()) {
        continue;
      }
      entry["default_base64"] =
          absl::Base64Escape(*found->second.default_value);
    }
  }
  return value;
}

absl::StatusOr<a11::service::SessionOptions> ReadSessionOptions(
    const nlohmann::json& request) {
  a11::service::SessionOptions options;
  options.max_buffered_messages_total = 64;
  options.max_buffered_messages_per_stream = 16;
  options.max_concurrent_root_actions = 4;
  options.max_concurrent_nested_actions = 16;
  options.max_buffered_bytes_total = 8 * 1024 * 1024;
  options.max_buffered_bytes_per_stream = 2 * 1024 * 1024;
  const auto configured = request.find("options");
  if (configured == request.end()) {
    return options;
  }
  if (!configured->is_object()) {
    return absl::InvalidArgumentError("session options must be an object");
  }
  auto read_size = [&](const char* field, size_t* output) -> absl::Status {
    const auto value = configured->find(field);
    if (value == configured->end()) {
      return absl::OkStatus();
    }
    if (!value->is_number_unsigned() ||
        value->get<std::uint64_t>() > std::numeric_limits<size_t>::max()) {
      return absl::InvalidArgumentError(std::string(field) +
                                        " must be a nonnegative integer");
    }
    *output = value->get<size_t>();
    return absl::OkStatus();
  };
  ABSL_RETURN_IF_ERROR(read_size("maxBufferedMessagesTotal",
                                 &options.max_buffered_messages_total));
  ABSL_RETURN_IF_ERROR(read_size("maxBufferedMessagesPerStream",
                                 &options.max_buffered_messages_per_stream));
  ABSL_RETURN_IF_ERROR(read_size("maxConcurrentRootActions",
                                 &options.max_concurrent_root_actions));
  ABSL_RETURN_IF_ERROR(read_size("maxConcurrentNestedActions",
                                 &options.max_concurrent_nested_actions));
  ABSL_RETURN_IF_ERROR(
      read_size("maxSingleMessageSize", &options.max_single_message_size));
  ABSL_RETURN_IF_ERROR(
      read_size("maxBufferedBytesTotal", &options.max_buffered_bytes_total));
  ABSL_RETURN_IF_ERROR(read_size("maxBufferedBytesPerStream",
                                 &options.max_buffered_bytes_per_stream));
  if (const auto timeout = configured->find("noStreamTimeoutMs");
      timeout != configured->end()) {
    if (timeout->is_null()) {
      options.no_stream_timeout = absl::InfiniteDuration();
    } else if (timeout->is_number_unsigned() &&
               timeout->get<std::uint64_t>() <= 86'400'000) {
      options.no_stream_timeout =
          absl::Milliseconds(timeout->get<std::uint64_t>());
    } else {
      return absl::InvalidArgumentError(
          "noStreamTimeoutMs must be null or milliseconds in one day");
    }
  }
  if (const auto deadline = configured->find("deadlineMs");
      deadline != configured->end()) {
    if (deadline->is_null()) {
      options.deadline = absl::InfiniteFuture();
    } else if (deadline->is_number_integer()) {
      options.deadline = absl::FromUnixMillis(deadline->get<std::int64_t>());
    } else {
      return absl::InvalidArgumentError(
          "deadlineMs must be null or a Unix millisecond timestamp");
    }
  }
  ABSL_RETURN_IF_ERROR(options.Validate());
  return options;
}

absl::StatusOr<std::uint32_t> RequiredSequence(const nlohmann::json& request,
                                               const char* name) {
  const auto found = request.find(name);
  if (found == request.end() || !found->is_number_unsigned() ||
      found->get<std::uint64_t>() > UINT32_MAX) {
    return absl::InvalidArgumentError(std::string(name) + " must be uint32");
  }
  return found->get<std::uint32_t>();
}

absl::StatusOr<absl::Time> ReadDeadline(const nlohmann::json& request) {
  if (auto epoch = request.find("deadlineMs"); epoch != request.end()) {
    if (!epoch->is_number_integer() ||
        (epoch->is_number_unsigned() &&
         epoch->get<std::uint64_t>() >
             static_cast<std::uint64_t>(
                 std::numeric_limits<std::int64_t>::max()))) {
      return absl::InvalidArgumentError("deadlineMs must be an integer");
    }
    return absl::FromUnixMillis(epoch->get<std::int64_t>());
  }
  const auto found = request.find("timeoutMs");
  if (found == request.end()) {
    return absl::InfiniteFuture();
  }
  if (!found->is_number_unsigned() ||
      found->get<std::uint64_t>() > 86'400'000) {
    return absl::InvalidArgumentError("timeoutMs must be nonnegative");
  }
  return absl::Now() + absl::Milliseconds(found->get<std::uint64_t>());
}

absl::StatusOr<nlohmann::json> Command(
    const std::shared_ptr<a11::wasm::NativeObjects>& state,
    const nlohmann::json& request) {
  if (!request.is_object()) {
    return absl::InvalidArgumentError(
        "native object request must be an object");
  }
  ABSL_ASSIGN_OR_RETURN(const std::string operation,
                        RequiredString(request, "operation"));
  if (operation == "fiber_report") {
    return nlohmann::json{{"report", thread::FormatFiberReport()}};
  }
  if (operation == "configure") {
    const auto host = request.find("host");
    if (host == request.end()) {
      return absl::InvalidArgumentError("configure requires host settings");
    }
    ABSL_ASSIGN_OR_RETURN(auto settings,
                          a11::wasm::ParseHostSettings(*host));
    ABSL_ASSIGN_OR_RETURN(auto storage_id,
                          RequiredString(request, "storageId"));
    ABSL_RETURN_IF_ERROR(a11::data::ValidateName(storage_id));
    thread::MutexLock lock(&state->graph->mu);
    if (state->graph->initialized) {
      return absl::FailedPreconditionError(
          "native graph is already initialized");
    }
    state->graph->host_settings = std::move(settings);
    state->graph->storage_id = std::move(storage_id);
    return nlohmann::json::object();
  }
  ABSL_RETURN_IF_ERROR(EnsureInitialized(state->graph));
  if (operation == "snapshot_export") {
    ABSL_ASSIGN_OR_RETURN(auto storage_id,
                          RequiredString(request, "storageId"));
    if (storage_id.empty()) {
      return absl::InvalidArgumentError("storageId must not be empty");
    }
    {
      thread::MutexLock lock(&state->graph->mu);
      if (storage_id != state->graph->storage_id) {
        return absl::InvalidArgumentError(
            "snapshot storageId differs from runtime storage");
      }
    }
    std::shared_ptr<a11::wasm::StoreRegistry> stores;
    std::vector<std::shared_ptr<a11::actions::Action>> actions;
    std::vector<std::shared_ptr<a11::service::Session>> sessions;
    {
      thread::MutexLock lock(&state->graph->mu);
      if (state->graph->importing_snapshot ||
          !state->graph->flow_nodes.empty()) {
        return absl::FailedPreconditionError(
            "snapshot requires an idle native graph");
      }
      for (const auto& [unused, action] : state->graph->actions) {
        (void)unused;
        actions.push_back(action);
      }
      for (const auto& [unused, session] : state->graph->sessions) {
        (void)unused;
        sessions.push_back(session);
      }
      stores = state->graph->stores;
    }
    for (const auto& action : actions) {
      if (!action->IsDone()) {
        return absl::FailedPreconditionError(
            "snapshot requires completed actions");
      }
    }
    for (const auto& session : sessions) {
      if (!session->IsDone()) {
        return absl::FailedPreconditionError(
            "snapshot requires completed sessions");
      }
    }
    ABSL_ASSIGN_OR_RETURN(auto bytes,
                          a11::wasm::ExportGraphSnapshot(*stores, storage_id));
    return nlohmann::json{{"data", absl::Base64Escape(bytes)}};
  }
  if (operation == "snapshot_import") {
    ABSL_ASSIGN_OR_RETURN(auto storage_id,
                          RequiredString(request, "storageId"));
    ABSL_ASSIGN_OR_RETURN(auto encoded, RequiredString(request, "data"));
    if (storage_id.empty() || encoded.size() > 90 * 1024 * 1024) {
      return absl::InvalidArgumentError("invalid snapshot identity or size");
    }
    {
      thread::MutexLock lock(&state->graph->mu);
      if (storage_id != state->graph->storage_id) {
        return absl::InvalidArgumentError(
            "snapshot storageId differs from runtime storage");
      }
    }
    std::string bytes;
    if (!absl::Base64Unescape(encoded, &bytes)) {
      return absl::InvalidArgumentError("snapshot data must be base64");
    }
    {
      thread::MutexLock lock(&state->graph->mu);
      if (state->graph->importing_snapshot || !state->graph->actions.empty() ||
          !state->graph->sessions.empty() ||
          !state->graph->flow_nodes.empty() ||
          !state->graph->retained_flows.empty()) {
        return absl::FailedPreconditionError(
            "snapshot import requires an unused native graph");
      }
      thread::MutexLock stores_lock(&state->graph->stores->mu);
      if (!state->graph->stores->stores.empty()) {
        return absl::FailedPreconditionError(
            "snapshot import requires an empty node map");
      }
      state->graph->importing_snapshot = true;
    }
    auto restored = a11::wasm::RestoreGraphSnapshot(bytes, storage_id);
    {
      thread::MutexLock lock(&state->graph->mu);
      state->graph->importing_snapshot = false;
      if (!restored.ok()) {
        return restored.status();
      }
      state->graph->nodes = std::move(restored->nodes);
      state->graph->stores = std::move(restored->registry);
    }
    return nlohmann::json::object();
  }
  {
    thread::MutexLock lock(&state->graph->mu);
    if (state->graph->importing_snapshot) {
      return absl::FailedPreconditionError("snapshot import is in progress");
    }
  }
  if (operation == "action_registry_names") {
    return state->graph->registry->ListRegisteredActions();
  }
  ABSL_ASSIGN_OR_RETURN(const std::string id, RequiredString(request, "id"));
  if (operation == "flow_release") {
    thread::MutexLock lock(&state->graph->mu);
    if (state->graph->retained_flows.erase(id) == 0) {
      return absl::NotFoundError("Flow scope id is unknown");
    }
    return nlohmann::json::object();
  }
  if (operation == "host_stream_prepare") {
    {
      thread::MutexLock lock(&state->graph->mu);
      if (state->graph->host_streams.find(id) !=
              state->graph->host_streams.end() ||
          !state->graph->pending_host_streams.insert(id).second) {
        return absl::AlreadyExistsError("host stream id already exists");
      }
    }
    auto pair = a11::wasm::HostStreamPair::Create(id);
    thread::MutexLock lock(&state->graph->mu);
    state->graph->pending_host_streams.erase(id);
    if (!pair.ok()) {
      return pair.status();
    }
    state->graph->host_streams.emplace(id, std::move(*pair));
    return nlohmann::json{{"id", id}};
  }
  if (operation.rfind("host_stream_", 0) == 0) {
    std::shared_ptr<a11::wasm::HostStreamPair> pair;
    {
      thread::MutexLock lock(&state->graph->mu);
      auto found = state->graph->host_streams.find(id);
      if (found == state->graph->host_streams.end()) {
        return absl::NotFoundError("host stream id is unknown");
      }
      pair = found->second;
    }
    if (operation == "host_stream_send" ||
        operation == "host_stream_half_close") {
      const auto value = request.find("message");
      if (value == request.end()) {
        return absl::InvalidArgumentError("host stream message is required");
      }
      ABSL_ASSIGN_OR_RETURN(auto message,
                            a11::data::WireMessageFromJsonValue(*value));
      const absl::Status status =
          operation == "host_stream_send"
              ? pair->host_side->Send(std::move(message))
              : pair->host_side->HalfClose(std::move(message.headers));
      ABSL_RETURN_IF_ERROR(status);
      return nlohmann::json::object();
    }
    if (operation == "host_stream_abort") {
      const auto value = request.find("status");
      if (value == request.end()) {
        return absl::InvalidArgumentError("host stream status is required");
      }
      ABSL_ASSIGN_OR_RETURN(auto status, a11::StatusFromJson(*value));
      ABSL_RETURN_IF_ERROR(pair->host_side->Abort(std::move(status)));
      return nlohmann::json::object();
    }
    if (operation == "host_stream_release") {
      thread::MutexLock lock(&state->graph->mu);
      state->graph->host_streams.erase(id);
      return nlohmann::json::object();
    }
    return absl::UnimplementedError("unknown host stream operation");
  }
  if (operation == "channel_prepare") {
    ABSL_ASSIGN_OR_RETURN(auto wire_id, RequiredString(request, "wireId"));
    a11::net::WireStreamOptions options;
    a11::net::ChannelFramingOptions framing;
    auto role = a11::net::ChannelEndpointRole::kEither;
    ABSL_RETURN_IF_ERROR(ReadChannelConfig(request, &options, &framing, &role));
    auto channel = std::make_shared<a11::wasm::HostBinaryChannel>(id);
    ABSL_ASSIGN_OR_RETURN(auto stream,
                          a11::wasm::HostChannelWireStream::Create(
                              channel, std::move(wire_id), role, options,
                              framing));
    thread::MutexLock lock(&state->graph->mu);
    if (state->graph->channel_streams.find(id) !=
            state->graph->channel_streams.end() ||
        state->graph->host_streams.find(id) !=
            state->graph->host_streams.end()) {
      return absl::AlreadyExistsError("channel stream id already exists");
    }
    state->graph->binary_channels.emplace(id, std::move(channel));
    state->graph->channel_streams.emplace(id, std::move(stream));
    return nlohmann::json{{"id", id}};
  }
  if (operation == "channel_event") {
    ABSL_ASSIGN_OR_RETURN(auto kind, RequiredString(request, "kind"));
    std::shared_ptr<a11::wasm::HostBinaryChannel> channel;
    {
      thread::MutexLock lock(&state->graph->mu);
      const auto found = state->graph->binary_channels.find(id);
      if (found == state->graph->binary_channels.end()) {
        return absl::NotFoundError("channel stream id is unknown");
      }
      channel = found->second;
    }
    const auto payload = request.find("payload");
    ABSL_RETURN_IF_ERROR(channel->Emit(
        kind, payload == request.end() ? nlohmann::json() : *payload));
    return nlohmann::json::object();
  }
  if (operation.rfind("channel_", 0) == 0 ||
      operation == "session_add_channel_stream") {
    std::shared_ptr<a11::wasm::HostChannelWireStream> stream;
    {
      thread::MutexLock lock(&state->graph->mu);
      const auto found = state->graph->channel_streams.find(id);
      if (found == state->graph->channel_streams.end()) {
        return absl::NotFoundError("channel stream id is unknown");
      }
      stream = found->second;
    }
    if (operation == "channel_start" || operation == "channel_accept") {
      const std::weak_ptr<a11::wasm::HostChannelWireStream> weak = stream;
      auto on_message = [weak, id](
                            std::optional<a11::data::WireMessage> message) {
        return a11::SubmitTask(
            [weak, id, message = std::move(message)]() mutable -> absl::Status {
              auto locked = weak.lock();
              if (locked == nullptr) {
                return absl::CancelledError("channel stream was released");
              }
              if (!message.has_value()) {
                a11::data::WireMessage marker;
                marker.headers = locked->GetTrailers().value_or(
                    a11::data::ByteMap{});
                ABSL_ASSIGN_OR_RETURN(
                    auto value, a11::data::WireMessageToJsonValue(marker));
                ABSL_ASSIGN_OR_RETURN(
                    auto encoded, a11::DumpJson(value, "channel close"));
                return a11::wasm::HostStreamEvent(id, "half_close", encoded);
              }
              ABSL_ASSIGN_OR_RETURN(
                  auto value, a11::data::WireMessageToJsonValue(*message));
              ABSL_ASSIGN_OR_RETURN(
                  auto encoded, a11::DumpJson(value, "channel message"));
              return a11::wasm::HostStreamEvent(id, "message", encoded);
            });
      };
      auto on_done = [weak, id]() {
        return a11::SubmitTask([weak, id]() -> absl::Status {
          const auto active = weak.lock();
          if (active == nullptr) return absl::OkStatus();
          ABSL_ASSIGN_OR_RETURN(
              auto encoded,
              a11::DumpJson(a11::StatusToJsonOrEmptyDetails(
                                active->GetStatus()),
                            "channel completion"));
          return a11::wasm::HostStreamEvent(id, "done", encoded);
        });
      };
      auto started = operation == "channel_start"
                         ? stream->Start(std::move(on_message),
                                         std::move(on_done))
                         : stream->Accept(std::move(on_message),
                                          std::move(on_done));
      ABSL_RETURN_IF_ERROR(started.Await().status());
      return nlohmann::json::object();
    }
    if (operation == "channel_recv_start" ||
        operation == "channel_recv_accept") {
      ABSL_ASSIGN_OR_RETURN(
          auto receiver,
          a11::net::WireStreamWithRecv::Create(stream));
      {
        thread::MutexLock lock(&state->graph->mu);
        if (state->graph->channel_receivers.find(id) !=
            state->graph->channel_receivers.end()) {
          return absl::AlreadyExistsError("channel receiver already started");
        }
        state->graph->channel_receivers.emplace(id, receiver);
      }
      const std::weak_ptr<a11::wasm::HostChannelWireStream> weak = stream;
      auto on_message = [weak, id](
                            std::optional<a11::data::WireMessage> message) {
        return a11::SubmitTask(
            [weak, id, message = std::move(message)]() mutable
                -> absl::Status {
              const auto active = weak.lock();
              if (active == nullptr) {
                return absl::CancelledError("channel stream was released");
              }
              if (!message.has_value()) {
                a11::data::WireMessage marker;
                marker.headers = active->GetTrailers().value_or(
                    a11::data::ByteMap{});
                ABSL_ASSIGN_OR_RETURN(
                    auto value, a11::data::WireMessageToJsonValue(marker));
                ABSL_ASSIGN_OR_RETURN(
                    auto encoded, a11::DumpJson(value, "channel close"));
                return a11::wasm::HostStreamEvent(id, "half_close", encoded);
              }
              ABSL_ASSIGN_OR_RETURN(
                  auto value, a11::data::WireMessageToJsonValue(*message));
              ABSL_ASSIGN_OR_RETURN(
                  auto encoded, a11::DumpJson(value, "channel message"));
              return a11::wasm::HostStreamEvent(id, "message", encoded);
            });
      };
      auto on_done = [weak, id]() {
        return a11::SubmitTask([weak, id]() -> absl::Status {
          const auto active = weak.lock();
          if (active == nullptr) return absl::OkStatus();
          ABSL_ASSIGN_OR_RETURN(
              auto encoded,
              a11::DumpJson(a11::StatusToJsonOrEmptyDetails(
                                active->GetStatus()),
                            "channel completion"));
          return a11::wasm::HostStreamEvent(id, "done", encoded);
        });
      };
      const absl::Status started =
          (operation == "channel_recv_start"
               ? receiver->Start(std::move(on_message), std::move(on_done))
               : receiver->Accept(std::move(on_message), std::move(on_done)))
              .Await()
              .status();
      if (!started.ok()) {
        thread::MutexLock lock(&state->graph->mu);
        state->graph->channel_receivers.erase(id);
      }
      ABSL_RETURN_IF_ERROR(started);
      return nlohmann::json::object();
    }
    if (operation == "channel_recv_receive") {
      std::shared_ptr<a11::net::WireStreamWithRecv> receiver;
      {
        thread::MutexLock lock(&state->graph->mu);
        const auto found = state->graph->channel_receivers.find(id);
        if (found == state->graph->channel_receivers.end()) {
          return absl::FailedPreconditionError("channel receiver is not started");
        }
        receiver = found->second;
      }
      ABSL_ASSIGN_OR_RETURN(auto deadline, ReadDeadline(request));
      const absl::Duration timeout = deadline == absl::InfiniteFuture()
                                         ? absl::InfiniteDuration()
                                         : deadline - absl::Now();
      ABSL_ASSIGN_OR_RETURN(auto message,
                            receiver->Receive(timeout).Await(deadline));
      if (!message.has_value()) return nlohmann::json(nullptr);
      return a11::data::WireMessageToJsonValue(*message);
    }
    if (operation == "channel_send" || operation == "channel_half_close") {
      const auto value = request.find("message");
      if (value == request.end()) {
        return absl::InvalidArgumentError("channel message is required");
      }
      ABSL_ASSIGN_OR_RETURN(auto message,
                            a11::data::WireMessageFromJsonValue(*value));
      const absl::Status status =
          operation == "channel_send"
              ? stream->Send(std::move(message))
              : stream->HalfClose(std::move(message.headers));
      ABSL_RETURN_IF_ERROR(status);
      return nlohmann::json::object();
    }
    if (operation == "channel_abort") {
      const auto value = request.find("status");
      if (value == request.end()) {
        return absl::InvalidArgumentError("channel status is required");
      }
      ABSL_ASSIGN_OR_RETURN(auto status, a11::StatusFromJson(*value));
      ABSL_RETURN_IF_ERROR(stream->Abort(std::move(status)));
      return nlohmann::json::object();
    }
    if (operation == "channel_drain") {
      ABSL_RETURN_IF_ERROR(stream->DrainOutgoingMessages().Await().status());
      return nlohmann::json::object();
    }
    if (operation == "channel_deadline") {
      absl::Time deadline = absl::InfiniteFuture();
      const auto value = request.find("deadlineMs");
      if (value != request.end() && !value->is_null()) {
        ABSL_ASSIGN_OR_RETURN(deadline, ReadDeadline(request));
      }
      ABSL_RETURN_IF_ERROR(stream->SetDeadline(deadline));
      return nlohmann::json::object();
    }
    if (operation == "channel_status") {
      nlohmann::json result{{"status", a11::StatusToJsonOrEmptyDetails(
                                             stream->GetStatus())}};
      const auto deadline = stream->deadline();
      result["deadlineMs"] = deadline == absl::InfiniteFuture()
                                  ? nlohmann::json()
                                  : nlohmann::json(absl::ToUnixMillis(deadline));
      if (const auto trailers = stream->GetTrailers(); trailers.has_value()) {
        a11::data::WireMessage marker;
        marker.headers = *trailers;
        ABSL_ASSIGN_OR_RETURN(auto encoded_trailers,
                              a11::data::WireMessageToJsonValue(marker));
        result["trailers"] = std::move(encoded_trailers);
      }
      return result;
    }
    if (operation == "channel_release") {
      thread::MutexLock lock(&state->graph->mu);
      state->graph->channel_receivers.erase(id);
      state->graph->channel_streams.erase(id);
      state->graph->binary_channels.erase(id);
      return nlohmann::json::object();
    }
    if (operation == "session_add_channel_stream") {
      ABSL_ASSIGN_OR_RETURN(auto session_id,
                            RequiredString(request, "sessionId"));
      ABSL_ASSIGN_OR_RETURN(auto mode, RequiredString(request, "mode"));
      std::shared_ptr<a11::service::Session> session;
      {
        thread::MutexLock lock(&state->graph->mu);
        const auto found = state->graph->sessions.find(session_id);
        if (found == state->graph->sessions.end()) {
          return absl::NotFoundError("session id is unknown");
        }
        session = found->second;
      }
      if (mode != "start" && mode != "accept") {
        return absl::InvalidArgumentError("mode must be start or accept");
      }
      ABSL_ASSIGN_OR_RETURN(
          auto started, session->AddStream(
                            stream, mode == "start"
                                        ? a11::service::StreamMode::kStart
                                        : a11::service::StreamMode::kAccept));
      ABSL_RETURN_IF_ERROR(started.Await().status());
      return nlohmann::json{{"id", stream->GetId()}};
    }
    return absl::UnimplementedError("unknown channel operation");
  }
  if (operation == "session_add_host_stream") {
    ABSL_ASSIGN_OR_RETURN(auto session_id,
                          RequiredString(request, "sessionId"));
    ABSL_ASSIGN_OR_RETURN(auto mode, RequiredString(request, "mode"));
    std::shared_ptr<a11::service::Session> session;
    std::shared_ptr<a11::wasm::HostStreamPair> pair;
    {
      thread::MutexLock lock(&state->graph->mu);
      auto found_session = state->graph->sessions.find(session_id);
      auto found_stream = state->graph->host_streams.find(id);
      if (found_session == state->graph->sessions.end() ||
          found_stream == state->graph->host_streams.end()) {
        return absl::NotFoundError("session or host stream is unknown");
      }
      session = found_session->second;
      pair = found_stream->second;
    }
    if (mode != "start" && mode != "accept") {
      return absl::InvalidArgumentError("mode must be start or accept");
    }
    ABSL_ASSIGN_OR_RETURN(
        auto started, session->AddStream(
                          pair->session_side,
                          mode == "start" ? a11::service::StreamMode::kStart
                                          : a11::service::StreamMode::kAccept));
    ABSL_RETURN_IF_ERROR(started.Await().status());
    return nlohmann::json{{"id", id}};
  }
  if (operation == "action_register" || operation == "action_register_schema") {
    const auto description = request.find("schema");
    if (description == request.end()) {
      return absl::InvalidArgumentError("action_register requires schema");
    }
    ABSL_ASSIGN_OR_RETURN(auto schema, ReadActionSchema(*description));
    if (schema.name != id) {
      return absl::InvalidArgumentError("action schema name differs from id");
    }
    if (operation == "action_register") {
      ABSL_RETURN_IF_ERROR(a11::wasm::RegisterHostAction(
          *state->graph->registry, std::move(schema), state->graph));
    } else {
      const std::string registered_name = schema.name;
      ABSL_RETURN_IF_ERROR(
          state->graph->registry->Register(registered_name, std::move(schema)));
    }
    return nlohmann::json::object();
  }
  if (operation == "action_unregister") {
    ABSL_RETURN_IF_ERROR(state->graph->registry->Unregister(id));
    return nlohmann::json::object();
  }
  if (operation == "action_registry_has") {
    return state->graph->registry->IsRegistered(id);
  }
  if (operation == "action_registry_schema") {
    ABSL_ASSIGN_OR_RETURN(auto schema,
                          state->graph->registry->GetSchema(id));
    return WriteActionSchema(
        schema, state->graph->registry->GetHandler(id).ok());
  }
  if (operation == "action_registry_message") {
    std::string action_id;
    if (auto found = request.find("actionId"); found != request.end()) {
      if (!found->is_string()) {
        return absl::InvalidArgumentError("actionId must be a string");
      }
      action_id = found->get<std::string>();
    }
    ABSL_ASSIGN_OR_RETURN(auto message,
                          state->graph->registry->MakeActionMessage(
                              id, std::move(action_id)));
    return a11::data::ActionMessageToJsonValue(message);
  }
  if (operation == "session_create") {
    ABSL_ASSIGN_OR_RETURN(auto options, ReadSessionOptions(request));
    a11::data::ByteMap headers;
    if (auto found = request.find("headers"); found != request.end()) {
      ABSL_ASSIGN_OR_RETURN(auto decoded,
                            a11::data::WireMessageFromJsonValue(
                                nlohmann::json{{"headers", *found}}));
      headers = std::move(decoded.headers);
    }
    bool with_recv = false;
    if (auto found = request.find("withRecv"); found != request.end()) {
      if (!found->is_boolean()) {
        return absl::InvalidArgumentError("withRecv must be boolean");
      }
      with_recv = found->get<bool>();
    }
    bool with_callbacks = false;
    if (auto found = request.find("withCallbacks");
        found != request.end()) {
      if (!found->is_boolean()) {
        return absl::InvalidArgumentError(
            "withCallbacks must be boolean");
      }
      with_callbacks = found->get<bool>();
    }
    if (with_recv && with_callbacks) {
      return absl::InvalidArgumentError(
          "receiving sessions cannot install stream callbacks");
    }
    std::shared_ptr<a11::service::Session> session;
    if (with_recv) {
      ABSL_ASSIGN_OR_RETURN(auto receiver,
                            a11::service::SessionWithRecv::Create(
                                id, std::move(headers), std::move(options),
                                state->graph->nodes, state->graph->registry));
      session = std::move(receiver);
    } else {
      a11::service::OnSessionStreamMessage on_message;
      a11::service::OnSessionStreamDone on_done;
      if (with_callbacks) {
        on_message = [id](std::optional<a11::data::WireMessage> message,
                          std::shared_ptr<a11::net::WireStream> stream,
                          std::shared_ptr<a11::service::Session>) {
          return a11::SubmitTask(
              [id, message = std::move(message),
               stream = std::move(stream)]() -> absl::Status {
                nlohmann::json event = {
                    {"kind", "message"},
                    {"streamId", stream->GetId()},
                    {"message", nullptr},
                };
                if (message.has_value()) {
                  ABSL_ASSIGN_OR_RETURN(
                      event["message"],
                      a11::data::WireMessageToJsonValue(*message));
                }
                ABSL_ASSIGN_OR_RETURN(
                    auto payload,
                    a11::DumpJson(event, "session message callback"));
                return a11::wasm::HostInvokeSessionCallback(id, payload);
              });
        };
        on_done = [id](std::shared_ptr<a11::net::WireStream> stream,
                       std::shared_ptr<a11::service::Session>) {
          return a11::SubmitTask(
              [id, stream = std::move(stream)]() -> absl::Status {
                ABSL_ASSIGN_OR_RETURN(
                    auto payload,
                    a11::DumpJson(nlohmann::json{{"kind", "done"},
                                                 {"streamId", stream->GetId()}},
                                  "session completion callback"));
                return a11::wasm::HostInvokeSessionCallback(id, payload);
              });
        };
      }
      ABSL_ASSIGN_OR_RETURN(session,
                            a11::service::Session::Create(
                                id, std::move(on_message), std::move(on_done),
                                std::move(headers),
                                std::move(options),
                                state->graph->nodes, state->graph->registry));
    }
    thread::MutexLock lock(&state->graph->mu);
    if (state->graph->sessions.find(id) != state->graph->sessions.end()) {
      return absl::AlreadyExistsError("session id already exists");
    }
    state->graph->sessions.emplace(id, std::move(session));
    return nlohmann::json{{"id", id}};
  }
  if (operation.rfind("session_", 0) == 0) {
    std::shared_ptr<a11::service::Session> session;
    {
      thread::MutexLock lock(&state->graph->mu);
      auto found = state->graph->sessions.find(id);
      if (found == state->graph->sessions.end()) {
        return absl::NotFoundError("session id is unknown");
      }
      session = found->second;
    }
    if (operation == "session_state") {
      const auto status = session->GetStatus();
      const absl::Time deadline = session->deadline();
      return nlohmann::json{{"id", id},
                            {"closed", session->IsClosed()},
                            {"done", session->IsDone()},
                            {"deadlineMs",
                             deadline == absl::InfiniteFuture()
                                 ? nlohmann::json(nullptr)
                                 : nlohmann::json(absl::ToUnixMillis(deadline))},
                            {"status", a11::StatusToJsonOrEmptyDetails(status)}};
    }
    if (operation == "session_set_deadline") {
      auto found = request.find("deadlineMs");
      if (found == request.end()) {
        return absl::InvalidArgumentError("deadlineMs is required");
      }
      absl::Time deadline = absl::InfiniteFuture();
      if (!found->is_null()) {
        if (!found->is_number_integer() ||
            (found->is_number_unsigned() &&
             found->get<std::uint64_t>() >
                 static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max()))) {
          return absl::InvalidArgumentError(
              "deadlineMs must be null or a Unix millisecond timestamp");
        }
        deadline = absl::FromUnixMillis(found->get<std::int64_t>());
      }
      ABSL_RETURN_IF_ERROR(session->SetDeadline(deadline));
      return nlohmann::json::object();
    }
    if (operation == "session_receive") {
      auto receiver =
          std::dynamic_pointer_cast<a11::service::SessionWithRecv>(session);
      if (receiver == nullptr) {
        return absl::FailedPreconditionError(
            "session was not created for pull-style receive");
      }
      ABSL_ASSIGN_OR_RETURN(auto deadline, ReadDeadline(request));
      ABSL_ASSIGN_OR_RETURN(auto received,
                            receiver->ReceiveWithStreamId(deadline).Await(
                                deadline));
      if (!received.has_value()) return nlohmann::json(nullptr);
      ABSL_ASSIGN_OR_RETURN(auto message,
                            a11::data::WireMessageToJsonValue(
                                received->message));
      return nlohmann::json{{"message", std::move(message)},
                            {"streamId", received->stream_id}};
    }
    if (operation == "session_cancel_actions") {
      ABSL_RETURN_IF_ERROR(session->CancelAllActions());
      return nlohmann::json::object();
    }
    if (operation == "session_cancel_action") {
      ABSL_ASSIGN_OR_RETURN(auto action_id,
                            RequiredString(request, "actionId"));
      ABSL_RETURN_IF_ERROR(session->CancelAction(action_id));
      return nlohmann::json::object();
    }
    if (operation == "session_actions") {
      nlohmann::json actions = nlohmann::json::array();
      for (const auto& [action_id, running] : session->Actions()) {
        actions.push_back(nlohmann::json{
            {"id", action_id}, {"name", running->GetSchema().name}});
      }
      return actions;
    }
    if (operation == "session_action") {
      ABSL_ASSIGN_OR_RETURN(auto action_id,
                            RequiredString(request, "actionId"));
      ABSL_ASSIGN_OR_RETURN(auto running, session->GetAction(action_id));
      return nlohmann::json{{"id", action_id},
                            {"name", running->GetSchema().name}};
    }
    if (operation == "session_await_actions") {
      ABSL_ASSIGN_OR_RETURN(auto deadline, ReadDeadline(request));
      ABSL_RETURN_IF_ERROR(session->AwaitAllActions(
          deadline - absl::Now()).Await(deadline).status());
      return nlohmann::json::object();
    }
    if (operation == "session_half_close") {
      ABSL_RETURN_IF_ERROR(session->HalfClose());
      return nlohmann::json::object();
    }
    if (operation == "session_done") {
      ABSL_ASSIGN_OR_RETURN(auto deadline, ReadDeadline(request));
      ABSL_RETURN_IF_ERROR(session->Done().Await(deadline).status());
      return a11::StatusToJsonOrEmptyDetails(session->GetStatus());
    }
    if (operation == "session_send") {
      const auto value = request.find("message");
      if (value == request.end()) {
        return absl::InvalidArgumentError("session_send requires message");
      }
      ABSL_ASSIGN_OR_RETURN(auto message,
                            a11::data::WireMessageFromJsonValue(*value));
      std::string stream_id;
      if (const auto selected = request.find("streamId");
          selected != request.end()) {
        if (!selected->is_string()) {
          return absl::InvalidArgumentError("streamId must be a string");
        }
        stream_id = selected->get<std::string>();
      }
      ABSL_RETURN_IF_ERROR(session->Send(std::move(message), stream_id));
      return nlohmann::json::object();
    }
    if (operation == "session_dispatch_wire_message") {
      const auto value = request.find("message");
      if (value == request.end()) {
        return absl::InvalidArgumentError(
            "session_dispatch_wire_message requires message");
      }
      ABSL_ASSIGN_OR_RETURN(auto message,
                            a11::data::WireMessageFromJsonValue(*value));
      std::shared_ptr<a11::net::WireStream> origin;
      if (auto found = request.find("streamId");
          found != request.end()) {
        if (!found->is_string()) {
          return absl::InvalidArgumentError("streamId must be a string");
        }
        ABSL_ASSIGN_OR_RETURN(origin,
                              session->GetStream(found->get<std::string>()));
      }
      ABSL_RETURN_IF_ERROR(
          session->DispatchWireMessage(std::move(message),
                                       std::move(origin)).Await().status());
      return nlohmann::json::object();
    }
    if (operation == "session_abort") {
      absl::Status reason = absl::CancelledError("session aborted by host");
      if (const auto status = request.find("status"); status != request.end()) {
        ABSL_ASSIGN_OR_RETURN(reason, a11::StatusFromJson(*status));
      }
      ABSL_RETURN_IF_ERROR(session->Abort(std::move(reason)));
      return nlohmann::json::object();
    }
    return absl::UnimplementedError("unknown native session operation");
  }
  if (operation == "action_create") {
    ABSL_ASSIGN_OR_RETURN(auto name, RequiredString(request, "name"));
    std::shared_ptr<a11::service::Session> session;
    std::shared_ptr<a11::net::WireStream> stream;
    if (auto found = request.find("sessionId"); found != request.end()) {
      if (!found->is_string()) {
        return absl::InvalidArgumentError("sessionId must be a string");
      }
      thread::MutexLock lock(&state->graph->mu);
      auto existing = state->graph->sessions.find(found->get<std::string>());
      if (existing == state->graph->sessions.end()) {
        return absl::NotFoundError("session id is unknown");
      }
      session = existing->second;
    }
    if (auto found = request.find("streamId"); found != request.end()) {
      if (!found->is_string() || session == nullptr) {
        return absl::InvalidArgumentError(
            "streamId requires a session and must be a string");
      }
      ABSL_ASSIGN_OR_RETURN(stream,
                            session->GetStream(found->get<std::string>()));
    } else if (session != nullptr) {
      ABSL_ASSIGN_OR_RETURN(auto streams, session->Streams());
      if (streams.size() == 1) {
        stream = std::move(streams.front().second);
      }
    }
    std::shared_ptr<a11::actions::Action> action;
    if (auto description = request.find("schema");
        description != request.end() &&
        !state->graph->registry->IsRegistered(name)) {
      ABSL_ASSIGN_OR_RETURN(auto schema, ReadActionSchema(*description));
      if (schema.name != name) {
        return absl::InvalidArgumentError("action schema name differs from name");
      }
      ABSL_ASSIGN_OR_RETURN(action,
                            a11::actions::Action::Create(
                                std::move(schema), id, {}, state->graph->nodes,
                                stream, session, state->graph->registry));
    } else {
      ABSL_ASSIGN_OR_RETURN(action,
                            state->graph->registry->MakeAction(
                                name, id, state->graph->nodes, stream,
                                session));
    }
    if (auto found = request.find("settings"); found != request.end()) {
      ABSL_ASSIGN_OR_RETURN(auto settings,
                            ReadActionSettings(*found, action->GetSettings()));
      ABSL_RETURN_IF_ERROR(action->SetSettings(std::move(settings)));
    }
    thread::MutexLock lock(&state->graph->mu);
    if (state->graph->actions.find(id) != state->graph->actions.end()) {
      return absl::AlreadyExistsError("action id already exists");
    }
    state->graph->actions.emplace(id, std::move(action));
    return nlohmann::json{{"id", id}};
  }
  if (operation.rfind("action_", 0) == 0) {
    std::shared_ptr<a11::actions::Action> action;
    {
      thread::MutexLock lock(&state->graph->mu);
      auto found = state->graph->actions.find(id);
      if (found == state->graph->actions.end()) {
        return absl::NotFoundError("action id is unknown");
      }
      action = found->second;
    }
    if (operation == "action_run") {
      ABSL_RETURN_IF_ERROR(action->Run().status());
      return nlohmann::json::object();
    }
    if (operation == "action_call") {
      if (action->GetSession() != nullptr && action->GetStream() == nullptr &&
          !action->GetSchema().inputs.empty()) {
        return absl::FailedPreconditionError(
            "a streamed action call requires an attached streamId");
      }
      ABSL_RETURN_IF_ERROR(action->Call().Await().status());
      return nlohmann::json::object();
    }
    if (operation == "action_state") {
      nlohmann::json result = {
          {"id", id},
          {"status", a11::StatusToJsonOrEmptyDetails(action->GetStatus())},
          {"run", action->HasBeenRun()},
          {"called", action->HasBeenCalled()},
          {"done", action->IsDone()},
      };
      if (const auto dispatched = action->GetDispatchStatus();
          dispatched.has_value()) {
        result["dispatchStatus"] = a11::StatusToJsonOrEmptyDetails(*dispatched);
      }
      return result;
    }
    if (operation == "action_session_id") {
      const auto session = action->GetSession();
      return session == nullptr ? nlohmann::json(nullptr)
                                : nlohmann::json(session->GetId());
    }
    if (operation == "action_headers") {
      a11::data::WireMessage message;
      message.headers = action->Headers();
      ABSL_ASSIGN_OR_RETURN(auto encoded,
                            a11::data::WireMessageToJsonValue(message));
      return encoded.value("headers", nlohmann::json::object());
    }
    if (operation == "action_schema") {
      return WriteActionSchema(action->GetSchema(), action->HasHandler());
    }
    if (operation == "action_settings") {
      const a11::actions::ActionSettings settings = action->GetSettings();
      return nlohmann::json{
          {"bindStreamsOnInputsByDefault",
           settings.bind_streams_on_inputs_by_default},
          {"bindStreamsOnOutputsByDefault",
           settings.bind_streams_on_outputs_by_default},
          {"clearInputsAfterRun", settings.clear_inputs_after_run},
          {"clearOutputsAfterRun", settings.clear_outputs_after_run}};
    }
    if (operation == "action_set_settings") {
      const auto found = request.find("settings");
      if (found == request.end()) {
        return absl::InvalidArgumentError("settings are required");
      }
      ABSL_ASSIGN_OR_RETURN(auto settings,
                            ReadActionSettings(*found, action->GetSettings()));
      ABSL_RETURN_IF_ERROR(action->SetSettings(std::move(settings)));
      return nlohmann::json::object();
    }
    if (operation == "action_make_nested") {
      ABSL_ASSIGN_OR_RETURN(auto name, RequiredString(request, "name"));
      bool propagate_io = true;
      bool forward_headers = true;
      for (const auto& [field, target] :
           {std::pair{"propagateIo", &propagate_io},
            std::pair{"forwardHeaders", &forward_headers}}) {
        if (auto found = request.find(field); found != request.end()) {
          if (!found->is_boolean()) {
            return absl::InvalidArgumentError(std::string(field) +
                                              " must be boolean");
          }
          *target = found->get<bool>();
        }
      }
      std::shared_ptr<a11::actions::Action> child;
      if (auto description = request.find("schema");
          description != request.end()) {
        ABSL_ASSIGN_OR_RETURN(auto schema, ReadActionSchema(*description));
        if (schema.name != name) {
          return absl::InvalidArgumentError(
              "nested schema name differs from name");
        }
        ABSL_ASSIGN_OR_RETURN(child,
                              action->MakeNested(schema, propagate_io,
                                                 forward_headers));
      } else {
        ABSL_ASSIGN_OR_RETURN(child,
                              action->MakeNested(name, propagate_io,
                                                 forward_headers));
      }
      if (auto requested_id = request.find("childId");
          requested_id != request.end()) {
        if (!requested_id->is_string()) {
          return absl::InvalidArgumentError("childId must be a string");
        }
        ABSL_RETURN_IF_ERROR(
            child->SetId(requested_id->get<std::string>()));
      }
      const std::string child_id = child->GetId();
      {
        thread::MutexLock lock(&state->graph->mu);
        if (!state->graph->actions.emplace(child_id, std::move(child)).second) {
          return absl::AlreadyExistsError("nested action id already exists");
        }
      }
      return nlohmann::json{{"id", child_id}, {"name", name}};
    }
    if (operation == "action_get_header" ||
        operation == "action_has_header") {
      ABSL_ASSIGN_OR_RETURN(auto name, RequiredString(request, "name"));
      if (operation == "action_has_header") {
        return action->HasHeader(name);
      }
      ABSL_ASSIGN_OR_RETURN(auto value, action->GetHeader(name));
      return value.has_value() ? nlohmann::json(absl::Base64Escape(*value))
                               : nlohmann::json(nullptr);
    }
    if (operation == "action_set_header") {
      ABSL_ASSIGN_OR_RETURN(auto name, RequiredString(request, "name"));
      ABSL_ASSIGN_OR_RETURN(auto value, RequiredString(request, "value"));
      nlohmann::json encoded = {
          {"headers", {{name, value}}},
      };
      ABSL_ASSIGN_OR_RETURN(auto decoded,
                            a11::data::WireMessageFromJsonValue(encoded));
      const auto found = decoded.headers.find(name);
      if (found == decoded.headers.end()) {
        return absl::InvalidArgumentError("header could not be decoded");
      }
      ABSL_RETURN_IF_ERROR(action->SetHeader(name, found->second));
      return nlohmann::json::object();
    }
    if (operation == "action_remove_header") {
      ABSL_ASSIGN_OR_RETURN(auto name, RequiredString(request, "name"));
      ABSL_RETURN_IF_ERROR(action->RemoveHeader(name));
      return nlohmann::json::object();
    }
    if (operation == "action_wait" ||
        operation == "action_wait_dispatch") {
      absl::Duration timeout = absl::Seconds(30);
      if (auto found = request.find("timeoutMs"); found != request.end()) {
        if (!found->is_number_unsigned() ||
            found->get<std::uint64_t>() > 86'400'000) {
          return absl::InvalidArgumentError("timeoutMs must be nonnegative");
        }
        timeout = absl::Milliseconds(found->get<std::uint64_t>());
      }
      if (operation == "action_wait_dispatch") {
        ABSL_ASSIGN_OR_RETURN(auto dispatch,
                              action->WaitForDispatch(timeout).Await(
                                  absl::Now() + timeout));
        return a11::StatusToJsonOrEmptyDetails(dispatch);
      }
      ABSL_RETURN_IF_ERROR(
          action->Wait(timeout).Await(absl::Now() + timeout).status());
      return nlohmann::json::object();
    }
    if (operation == "action_cancel") {
      ABSL_RETURN_IF_ERROR(action->Cancel());
      return nlohmann::json::object();
    }
    if (operation == "action_release") {
      if (!action->IsDone()) {
        return absl::FailedPreconditionError(
            "action must finish before release");
      }
      thread::MutexLock lock(&state->graph->mu);
      state->graph->actions.erase(id);
      return nlohmann::json::object();
    }
    if (operation == "action_port") {
      ABSL_ASSIGN_OR_RETURN(auto name, RequiredString(request, "name"));
      ABSL_ASSIGN_OR_RETURN(auto direction,
                            RequiredString(request, "direction"));
      std::optional<bool> bind_stream;
      if (auto found = request.find("bindStream");
          found != request.end() && !found->is_null()) {
        if (!found->is_boolean()) {
          return absl::InvalidArgumentError("bindStream must be boolean");
        }
        bind_stream = found->get<bool>();
      }
      if (direction == "either" && bind_stream.has_value()) {
        return absl::InvalidArgumentError(
            "bindStream requires input or output direction");
      }
      absl::StatusOr<std::shared_ptr<a11::nodes::AsyncNode>> port =
          direction == "input" ? action->GetInput(name, bind_stream)
          : direction == "output"
              ? action->GetOutput(name, bind_stream)
              : direction == "either"
                  ? action->GetPort(name)
              : absl::StatusOr<std::shared_ptr<a11::nodes::AsyncNode>>(
                    absl::InvalidArgumentError(
                        "direction must be input, output, or either"));
      ABSL_ASSIGN_OR_RETURN(auto node, std::move(port));
      ABSL_ASSIGN_OR_RETURN(auto node_id, node->GetId());
      return nlohmann::json{{"id", node_id}};
    }
    if (operation == "action_contains_port") {
      ABSL_ASSIGN_OR_RETURN(auto name, RequiredString(request, "name"));
      return action->ContainsPort(name);
    }
    if (operation == "action_log_node") {
      ABSL_ASSIGN_OR_RETURN(auto node, action->GetLogNode());
      ABSL_ASSIGN_OR_RETURN(auto node_id, node->GetId());
      return nlohmann::json{{"id", node_id}};
    }
    if (operation == "action_log") {
      const auto found = request.find("chunk");
      if (found == request.end()) {
        return absl::InvalidArgumentError("chunk is required");
      }
      ABSL_ASSIGN_OR_RETURN(auto chunk,
                            a11::data::ChunkFromJsonValue(*found));
      ABSL_RETURN_IF_ERROR(action->Log(std::move(chunk)));
      return nlohmann::json::object();
    }
    if (operation == "action_stream_sources_node") {
      bool request_payload = true;
      if (auto found = request.find("requestPayload");
          found != request.end()) {
        if (!found->is_boolean()) {
          return absl::InvalidArgumentError(
              "requestPayload must be boolean");
        }
        request_payload = found->get<bool>();
      }
      ABSL_ASSIGN_OR_RETURN(auto node,
                            action->GetStreamSourcesNode(request_payload));
      ABSL_ASSIGN_OR_RETURN(auto node_id, node->GetId());
      return nlohmann::json{{"id", node_id}};
    }
    if (operation == "action_expose_stream_sources") {
      ABSL_ASSIGN_OR_RETURN(auto port, RequiredString(request, "port"));
      ABSL_ASSIGN_OR_RETURN(auto purpose,
                            RequiredString(request, "purpose"));
      ABSL_RETURN_IF_ERROR(
          action->ExposeStreamSources(std::move(port), std::move(purpose)));
      return nlohmann::json::object();
    }
    if (operation == "action_register_stream") {
      ABSL_ASSIGN_OR_RETURN(auto purpose,
                            RequiredString(request, "purpose"));
      const auto source = request.find("source");
      if (source == request.end()) {
        return absl::InvalidArgumentError("source chunk is required");
      }
      ABSL_ASSIGN_OR_RETURN(auto decoded_source,
                            a11::data::ChunkFromJsonValue(*source));
      std::string producer_id;
      std::string parent_producer_id;
      if (auto found = request.find("producerId");
          found != request.end()) {
        if (!found->is_string()) {
          return absl::InvalidArgumentError("producerId must be a string");
        }
        producer_id = found->get<std::string>();
      }
      if (auto found = request.find("parentProducerId");
          found != request.end()) {
        if (!found->is_string()) {
          return absl::InvalidArgumentError(
              "parentProducerId must be a string");
        }
        parent_producer_id = found->get<std::string>();
      }
      ABSL_ASSIGN_OR_RETURN(
          auto reference,
          action->RegisterStream(std::move(purpose),
                                 std::move(decoded_source),
                                 std::move(producer_id),
                                 std::move(parent_producer_id)));
      ABSL_ASSIGN_OR_RETURN(
          auto chunk, a11::actions::StreamReferenceToChunk(reference));
      return a11::data::ChunkToJsonValue(chunk);
    }
    if (operation == "action_node") {
      ABSL_ASSIGN_OR_RETURN(auto node_id, RequiredString(request, "nodeId"));
      ABSL_RETURN_IF_ERROR(action->GetNodeMap()->Get(node_id).status());
      return nlohmann::json{{"id", node_id}};
    }
    if (operation == "action_resolve_node") {
      const auto reference = request.find("reference");
      if (reference == request.end()) {
        return absl::InvalidArgumentError("reference is required");
      }
      ABSL_ASSIGN_OR_RETURN(auto decoded,
                            a11::data::NodeRefFromJsonValue(*reference));
      ABSL_ASSIGN_OR_RETURN(auto node,
                            action->GetNodeMap()->Resolve(decoded));
      ABSL_ASSIGN_OR_RETURN(auto node_id, node->GetId());
      return nlohmann::json{{"id", node_id}};
    }
    return absl::UnimplementedError("unknown native action operation");
  }
  std::shared_ptr<a11::nodes::NodeMap> node_map = state->graph->nodes;
  {
    thread::MutexLock lock(&state->graph->mu);
    if (auto scope = request.find("scopeId"); scope != request.end()) {
      if (!scope->is_string()) {
        return absl::InvalidArgumentError("scopeId must be a string");
      }
      auto found = state->graph->retained_flows.find(scope->get<std::string>());
      if (found == state->graph->retained_flows.end()) {
        return absl::NotFoundError("Flow scope id is unknown");
      }
      node_map = found->second;
    } else {
      for (const auto& candidate : state->graph->flow_nodes) {
        if (candidate->Contains(id)) {
          node_map = candidate;
          break;
        }
      }
    }
  }
  if (operation == "node_exists") {
    ABSL_ASSIGN_OR_RETURN(auto existing, node_map->GetIfExists(id));
    return existing != nullptr;
  }
  if (operation == "node_map_size") {
    return nlohmann::json(node_map->Size());
  }
  if (operation == "node_map_ids") {
    return nlohmann::json(node_map->Ids());
  }
  if (operation == "node_map_discard") {
    ABSL_ASSIGN_OR_RETURN(auto removed, node_map->Discard(id));
    return removed != nullptr;
  }
  if (operation == "node_map_resolve") {
    const auto reference = request.find("reference");
    if (reference == request.end()) {
      return absl::InvalidArgumentError("reference is required");
    }
    ABSL_ASSIGN_OR_RETURN(auto decoded,
                          a11::data::NodeRefFromJsonValue(*reference));
    ABSL_ASSIGN_OR_RETURN(auto resolved, node_map->Resolve(decoded));
    ABSL_ASSIGN_OR_RETURN(auto resolved_id, resolved->GetId());
    return nlohmann::json{{"id", resolved_id}};
  }
  ABSL_ASSIGN_OR_RETURN(auto node, node_map->Get(id));
  if (operation.rfind("store_", 0) == 0) {
    const auto store = node->GetChunkStore();
    if (operation == "store_get") {
      ABSL_ASSIGN_OR_RETURN(auto sequence,
                            RequiredSequence(request, "sequence"));
      ABSL_ASSIGN_OR_RETURN(auto deadline, ReadDeadline(request));
      ABSL_ASSIGN_OR_RETURN(auto fragment,
                            store->Get(sequence, deadline).Await(deadline));
      return a11::data::NodeFragmentToJsonValue(fragment);
    }
    if (operation == "store_get_by_arrival") {
      ABSL_ASSIGN_OR_RETURN(auto order, RequiredSequence(request, "order"));
      ABSL_ASSIGN_OR_RETURN(auto deadline, ReadDeadline(request));
      ABSL_ASSIGN_OR_RETURN(
          auto fragment,
          store->GetByArrivalOrder(order, deadline).Await(deadline));
      return a11::data::NodeFragmentToJsonValue(fragment);
    }
    if (operation == "store_next") {
      ABSL_ASSIGN_OR_RETURN(auto deadline, ReadDeadline(request));
      ABSL_ASSIGN_OR_RETURN(auto limit, RequiredSequence(request, "limit"));
      if (limit == 0) {
        return absl::InvalidArgumentError("limit must be positive");
      }
      ABSL_ASSIGN_OR_RETURN(auto fragments,
                            store->Next(deadline, limit).Await(deadline));
      nlohmann::json result = nlohmann::json::array();
      for (const auto& fragment : fragments) {
        if (!fragment.has_value()) {
          result.push_back(nullptr);
        } else {
          ABSL_ASSIGN_OR_RETURN(auto encoded,
                                a11::data::NodeFragmentToJsonValue(*fragment));
          result.push_back(std::move(encoded));
        }
      }
      return result;
    }
    if (operation == "store_put" || operation == "store_put_many") {
      const auto raw = request.find("fragments");
      if (raw == request.end() || !raw->is_array()) {
        return absl::InvalidArgumentError("fragments must be an array");
      }
      std::vector<a11::data::NodeFragment> fragments;
      fragments.reserve(raw->size());
      for (const auto& value : *raw) {
        ABSL_ASSIGN_OR_RETURN(auto fragment,
                              a11::data::NodeFragmentFromJsonValue(value));
        if (fragment.id != id) {
          return absl::InvalidArgumentError("fragment belongs to another node");
        }
        fragments.push_back(std::move(fragment));
      }
      ABSL_ASSIGN_OR_RETURN(auto sequences,
                            store->PutMany(std::move(fragments)).Await());
      return nlohmann::json(sequences);
    }
    if (operation == "store_clear_data") {
      ABSL_ASSIGN_OR_RETURN(auto sequence,
                            RequiredSequence(request, "sequence"));
      ABSL_ASSIGN_OR_RETURN(auto fragment, store->ClearData(sequence).Await());
      return a11::data::NodeFragmentToJsonValue(fragment);
    }
    if (operation == "store_arrival_sequence") {
      ABSL_ASSIGN_OR_RETURN(auto order, RequiredSequence(request, "order"));
      ABSL_ASSIGN_OR_RETURN(auto sequence,
                            store->GetSeqForArrivalOrder(order).Await());
      return nlohmann::json(sequence);
    }
    if (operation == "store_final_sequence") {
      ABSL_ASSIGN_OR_RETURN(auto sequence, store->GetFinalSeq().Await());
      return sequence.has_value() ? nlohmann::json(*sequence)
                                  : nlohmann::json(nullptr);
    }
    if (operation == "store_size") {
      ABSL_ASSIGN_OR_RETURN(auto size, store->Size().Await());
      return nlohmann::json(size);
    }
    if (operation == "store_close") {
      ABSL_ASSIGN_OR_RETURN(auto code, RequiredSequence(request, "code"));
      if (code > 16) {
        return absl::InvalidArgumentError("status code is invalid");
      }
      ABSL_ASSIGN_OR_RETURN(auto message, RequiredString(request, "message"));
      const auto status =
          absl::Status(static_cast<absl::StatusCode>(code), message);
      bool return_existing = false;
      const auto option = request.find("returnExisting");
      if (option != request.end()) {
        if (!option->is_boolean()) {
          return absl::InvalidArgumentError("returnExisting must be boolean");
        }
        return_existing = option->get<bool>();
      }
      auto outcome =
          store->CloseWritesWithStatus(status, return_existing).Await();
      if (!outcome.ok()) {
        return outcome.status();
      }
      return nlohmann::json{{"code", static_cast<int>(outcome->code())},
                            {"message", outcome->message()}};
    }
    return absl::UnimplementedError("unknown native store operation");
  }
  if (operation == "node_get") {
    return nlohmann::json{{"id", id}};
  }
  if (operation == "node_ref") {
    std::uint32_t offset = 0;
    if (auto found = request.find("offset"); found != request.end()) {
      if (!found->is_number_unsigned() ||
          found->get<std::uint64_t>() > UINT32_MAX) {
        return absl::InvalidArgumentError("offset must be uint32");
      }
      offset = found->get<std::uint32_t>();
    }
    std::optional<std::uint64_t> length;
    if (auto found = request.find("length");
        found != request.end() && !found->is_null()) {
      if (!found->is_number_unsigned() ||
          found->get<std::uint64_t>() >
              static_cast<std::uint64_t>(UINT32_MAX) + 1) {
        return absl::InvalidArgumentError("length exceeds uint32 range");
      }
      length = found->get<std::uint64_t>();
    }
    auto policy = a11::nodes::ReferenceStoragePolicy::kAuto;
    if (auto found = request.find("storage"); found != request.end()) {
      if (!found->is_string()) {
        return absl::InvalidArgumentError("storage must be a string");
      }
      const std::string value = found->get<std::string>();
      if (value == "include") {
        policy = a11::nodes::ReferenceStoragePolicy::kInclude;
      } else if (value == "omit") {
        policy = a11::nodes::ReferenceStoragePolicy::kOmit;
      } else if (value != "auto") {
        return absl::InvalidArgumentError("unknown storage policy");
      }
    }
    ABSL_ASSIGN_OR_RETURN(auto reference,
                          node->Ref(offset, length, node_map, policy));
    return a11::data::NodeRefToJsonValue(reference);
  }
  if (operation == "node_put") {
    const auto chunk_value = request.find("chunk");
    if (chunk_value == request.end()) {
      return absl::InvalidArgumentError("node_put requires chunk");
    }
    ABSL_ASSIGN_OR_RETURN(auto chunk,
                          a11::data::ChunkFromJsonValue(*chunk_value));
    std::optional<std::uint32_t> sequence;
    if (auto found = request.find("sequence");
        found != request.end() && !found->is_null()) {
      if (!found->is_number_unsigned() ||
          found->get<std::uint64_t>() > UINT32_MAX) {
        return absl::InvalidArgumentError("sequence must be uint32");
      }
      sequence = found->get<std::uint32_t>();
    }
    bool final = false;
    if (auto found = request.find("final"); found != request.end()) {
      if (!found->is_boolean()) {
        return absl::InvalidArgumentError("final must be boolean");
      }
      final = found->get<bool>();
    }
    ABSL_ASSIGN_OR_RETURN(
        const auto written,
        node->PutChunk(std::move(chunk), sequence, final).Await());
    return nlohmann::json{{"sequence", written}};
  }
  if (operation == "node_put_fragment") {
    const auto value = request.find("fragment");
    if (value == request.end()) {
      return absl::InvalidArgumentError("node_put_fragment requires fragment");
    }
    ABSL_ASSIGN_OR_RETURN(auto fragment,
                          a11::data::NodeFragmentFromJsonValue(*value));
    ABSL_ASSIGN_OR_RETURN(auto node_id, node->GetId());
    if (fragment.id != node_id) {
      return absl::InvalidArgumentError(
          "fragment id differs from destination node");
    }
    ABSL_ASSIGN_OR_RETURN(auto written,
                          node->PutFragment(std::move(fragment)).Await());
    return nlohmann::json{{"sequence", written}};
  }
  if (operation == "node_finalize") {
    a11::nodes::FinalizeOptions options;
    options.wait = true;
    if (auto found = request.find("sequence");
        found != request.end() && !found->is_null()) {
      if (!found->is_number_unsigned() ||
          found->get<std::uint64_t>() > UINT32_MAX) {
        return absl::InvalidArgumentError("sequence must be uint32");
      }
      options.seq = found->get<std::uint32_t>();
    }
    for (const auto& [name, target] : {std::pair{"close", &options.close},
                                       std::pair{"wait", &options.wait}}) {
      if (auto found = request.find(name); found != request.end()) {
        if (!found->is_boolean()) {
          return absl::InvalidArgumentError(std::string(name) +
                                            " must be boolean");
        }
        *target = found->get<bool>();
      }
    }
    const auto chunk = request.find("chunk");
    const auto reference = request.find("reference");
    if (chunk != request.end() && reference != request.end()) {
      return absl::InvalidArgumentError(
          "finalize accepts either chunk or reference");
    }
    if (chunk != request.end()) {
      ABSL_ASSIGN_OR_RETURN(auto value, a11::data::ChunkFromJsonValue(*chunk));
      ABSL_RETURN_IF_ERROR(
          node->Finalize(std::move(value), options).Await().status());
    } else if (reference != request.end()) {
      ABSL_ASSIGN_OR_RETURN(auto value,
                            a11::data::NodeRefFromJsonValue(*reference));
      ABSL_RETURN_IF_ERROR(
          node->Finalize(std::move(value), options).Await().status());
    } else {
      ABSL_RETURN_IF_ERROR(node->Finalize(options).Await().status());
    }
    return nlohmann::json::object();
  }
  if (operation == "node_next") {
    absl::Time deadline = absl::InfiniteFuture();
    if (auto found = request.find("timeoutMs"); found != request.end()) {
      if (!found->is_number_unsigned() ||
          found->get<std::uint64_t>() > 86'400'000) {
        return absl::InvalidArgumentError("timeoutMs must be nonnegative");
      }
      deadline = absl::Now() + absl::Milliseconds(found->get<std::uint64_t>());
    }
    ABSL_ASSIGN_OR_RETURN(auto chunk, node->NextChunk().Await(deadline));
    if (!chunk.has_value()) {
      return nlohmann::json(nullptr);
    }
    return a11::data::ChunkToJsonValue(*chunk);
  }
  if (operation == "node_next_fragment") {
    absl::Duration timeout = absl::InfiniteDuration();
    if (auto found = request.find("timeoutMs"); found != request.end()) {
      if (!found->is_number_unsigned() ||
          found->get<std::uint64_t>() > 86'400'000) {
        return absl::InvalidArgumentError("timeoutMs must be nonnegative");
      }
      timeout = absl::Milliseconds(found->get<std::uint64_t>());
    }
    ABSL_ASSIGN_OR_RETURN(auto fragment, node->NextFragment(timeout).Await());
    if (!fragment.has_value()) {
      return nlohmann::json(nullptr);
    }
    return a11::data::NodeFragmentToJsonValue(*fragment);
  }
  if (operation == "node_close") {
    ABSL_RETURN_IF_ERROR(node->Close().Await().status());
    return nlohmann::json::object();
  }
  if (operation == "node_abort") {
    const auto found = request.find("status");
    if (found == request.end()) {
      return absl::InvalidArgumentError("node_abort requires status");
    }
    ABSL_ASSIGN_OR_RETURN(auto status, a11::StatusFromJson(*found));
    if (status.ok()) {
      return absl::InvalidArgumentError("abort status must be non-OK");
    }
    ABSL_RETURN_IF_ERROR(node->AbortWithStatus(status).Await().status());
    return nlohmann::json::object();
  }
  if (operation == "node_reader_options") {
    return WriteReaderOptions(node->GetReaderOptions());
  }
  if (operation == "node_set_reader_options" ||
      operation == "node_reset_reader") {
    const auto found = request.find("options");
    if (found == request.end() || found->is_null()) {
      if (operation == "node_set_reader_options") {
        return absl::InvalidArgumentError("reader options are required");
      }
      ABSL_RETURN_IF_ERROR(node->ResetReader());
    } else {
      ABSL_ASSIGN_OR_RETURN(auto options,
                            ReadReaderOptions(*found,
                                              node->GetReaderOptions()));
      ABSL_RETURN_IF_ERROR(operation == "node_reset_reader"
                               ? node->ResetReader(std::move(options))
                               : node->SetReaderOptions(std::move(options)));
    }
    return nlohmann::json::object();
  }
  if (operation == "node_writer_options") {
    return WriteWriterOptions(node->GetWriterOptions());
  }
  if (operation == "node_set_writer_options") {
    const auto found = request.find("options");
    if (found == request.end()) {
      return absl::InvalidArgumentError("writer options are required");
    }
    ABSL_ASSIGN_OR_RETURN(auto options,
                          ReadWriterOptions(*found,
                                            node->GetWriterOptions()));
    ABSL_RETURN_IF_ERROR(node->SetWriterOptions(std::move(options)));
    return nlohmann::json::object();
  }
  if (operation == "node_state") {
    const auto aborted = node->GetWriterAbortStatus();
    return nlohmann::json{
        {"readerStatus",
         a11::StatusToJsonOrEmptyDetails(node->GetReaderStatus())},
        {"writerStatus",
         a11::StatusToJsonOrEmptyDetails(node->GetWriterStatus())},
        {"writerAbortStatus", aborted.has_value()
                                  ? a11::StatusToJsonOrEmptyDetails(*aborted)
                                  : nlohmann::json(nullptr)}};
  }
  if (operation == "node_is_writable") {
    ABSL_ASSIGN_OR_RETURN(const bool writable, node->IsWritable().Await());
    return writable;
  }
  if (operation == "node_wait_for_buffer_to_drain") {
    ABSL_ASSIGN_OR_RETURN(auto deadline, ReadDeadline(request));
    ABSL_RETURN_IF_ERROR(node->WaitForBufferToDrain().Await(deadline).status());
    return nlohmann::json::object();
  }
  if (operation == "node_cancel_reader") {
    node->CancelReader();
    return nlohmann::json::object();
  }
  if (operation == "node_cancel_writer") {
    node->CancelWriter();
    return nlohmann::json::object();
  }
  if (operation == "node_cancel") {
    node->Cancel();
    return nlohmann::json::object();
  }
  return absl::UnimplementedError("unknown native object operation");
}

}  // namespace

extern "C" a11::wasm::NativeObjects* a11_wasm_native_objects_new() {
  return new a11::wasm::NativeObjects{
      .graph = std::make_shared<a11::wasm::NativeGraph>()};
}

extern "C" void a11_wasm_native_objects_free(a11::wasm::NativeObjects* state) {
  delete state;
}

extern "C" std::shared_ptr<a11::wasm::AsyncTask>* a11_wasm_native_command_start(
    a11::wasm::NativeObjects* state, const std::uint8_t* data, size_t size) {
  if (state == nullptr) {
    return nullptr;
  }
  auto retained = std::make_shared<a11::wasm::NativeObjects>(*state);
  auto task = a11::wasm::AsyncTask::Start(
      [retained,
       bytes = std::string(View(data, size))]() -> absl::StatusOr<std::string> {
        ABSL_ASSIGN_OR_RETURN(auto request,
                              a11::ParseJson(bytes, "native object command"));
        ABSL_ASSIGN_OR_RETURN(auto output, Command(retained, request));
        return a11::DumpJson(output, "native object result");
      });
  return new std::shared_ptr<a11::wasm::AsyncTask>(std::move(task));
}
