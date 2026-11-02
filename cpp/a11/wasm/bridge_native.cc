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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <emscripten.h>

#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/actions/describe.h"
#include "a11/actions/registry.h"
#include "a11/concurrency/executor.h"
#include "a11/data/json.h"
#include "a11/flow/runtime.h"
#include "a11/json_codec.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/status.h"
#include "sdk/flow/actions/time_actions.h"
#include "async_task.h"
#include "bridge_core.h"
#include "host_settings.h"
#include "native_objects.h"
#include "thread/wasm_system_actions.h"

namespace {

absl::Status HostInvokeCallback(const std::string& callback_id,
                                const std::string& payload,
                                bool session_callback) {
  constexpr int kPending = 0;
  constexpr int kCompleted = 1;
  constexpr size_t kMessageCapacity = 16 * 1024;
  constexpr int kInternalCode =
      static_cast<int>(absl::StatusCode::kInternal);
  constexpr int kResourceExhaustedCode =
      static_cast<int>(absl::StatusCode::kResourceExhausted);
  constexpr absl::Duration kCompletionPollInterval = absl::Milliseconds(1);
  std::atomic<int> done{kPending};
  std::atomic<int> code{kInternalCode};
  std::array<char, kMessageCapacity> message{};
  MAIN_THREAD_ASYNC_EM_ASM({
    const complete = (result) => {
      let code = Number.isInteger(result?.code) ? result.code : $6;
      let encoded;
      try {
        encoded = JSON.stringify(result);
      } catch (error) {
        code = $6;
        encoded = JSON.stringify({code, message:
          "Host callback status could not be serialized", details: []});
      }
      if (typeof encoded !== "string") {
        code = $6;
        encoded = JSON.stringify({code,
          message: "Host callback returned no status", details: []});
      }
      if (lengthBytesUTF8(encoded) >= $5) {
        code = $8;
        encoded = JSON.stringify({code: $8,
          message: "Host callback status exceeds the WASM bridge limit",
          details: []});
      }
      stringToUTF8(encoded, $4, $5);
      Atomics.store(HEAP32, $3 >> 2, code);
      Atomics.store(HEAP32, $2 >> 2, $7);
      Atomics.notify(HEAP32, $2 >> 2);
    };
    Promise.resolve().then(() => ($9
      ? globalThis.__a11WasmInvokeSession
      : globalThis.__a11WasmInvokeAction)(
      UTF8ToString($0), UTF8ToString($1))).then(complete,
      error => complete({code: $6, message: String(error?.message ?? error)}));
  }, callback_id.c_str(), payload.c_str(), &done, &code,
     message.data(), message.size(), kInternalCode, kCompleted,
     kResourceExhaustedCode, session_callback);
  while (done.load(std::memory_order_acquire) == kPending) {
    thread::SleepFor(kCompletionPollInterval);
  }
  const int final_code = code.load(std::memory_order_acquire);
  if (final_code < static_cast<int>(absl::StatusCode::kOk) ||
      final_code > static_cast<int>(absl::StatusCode::kUnauthenticated)) {
    return absl::InternalError("host callback returned an invalid status code");
  }
  ABSL_ASSIGN_OR_RETURN(auto encoded,
                        a11::ParseJson(message.data(), "host callback status"));
  ABSL_ASSIGN_OR_RETURN(auto status, a11::StatusFromJson(encoded));
  if (static_cast<int>(status.code()) != final_code) {
    return absl::DataLossError("host callback status code changed in transit");
  }
  return status;
}

absl::Status HostInvokeAction(const std::string& action_id,
                              const std::string& action_name) {
  return HostInvokeCallback(action_id, action_name, false);
}

class FlowMapGuard {
 public:
  FlowMapGuard(std::shared_ptr<a11::wasm::NativeGraph> graph,
               std::shared_ptr<a11::nodes::NodeMap> nodes)
      : graph_(std::move(graph)), nodes_(std::move(nodes)) {
    thread::MutexLock lock(&graph_->mu);
    graph_->flow_nodes.push_back(nodes_);
  }

  ~FlowMapGuard() {
    if (retained_) return;
    thread::MutexLock lock(&graph_->mu);
    auto& maps = graph_->flow_nodes;
    maps.erase(std::remove(maps.begin(), maps.end(), nodes_), maps.end());
  }

  absl::Status Retain(std::string id) {
    thread::MutexLock lock(&graph_->mu);
    if (graph_->retained_flows.find(id) != graph_->retained_flows.end()) {
      return absl::AlreadyExistsError("Flow scope id already exists");
    }
    auto& maps = graph_->flow_nodes;
    maps.erase(std::remove(maps.begin(), maps.end(), nodes_), maps.end());
    graph_->retained_flows.emplace(std::move(id), nodes_);
    retained_ = true;
    return absl::OkStatus();
  }

 private:
  std::shared_ptr<a11::wasm::NativeGraph> graph_;
  std::shared_ptr<a11::nodes::NodeMap> nodes_;
  bool retained_ = false;
};

absl::Status RegisterHostActionImpl(
    a11::actions::ActionRegistry& registry,
    a11::actions::ActionSchema schema,
    const std::shared_ptr<a11::wasm::NativeGraph>& graph) {
  const std::string name = schema.name;
  const auto inputs = schema.inputs;
  const auto outputs = schema.outputs;
  auto handler = [graph, name, inputs,
                  outputs](std::shared_ptr<a11::actions::Action> called) {
    return a11::SubmitTask(
        [graph, name, inputs, outputs,
         called = std::move(called)]() -> absl::Status {
          for (const auto& [port, unused] : inputs) {
            ABSL_RETURN_IF_ERROR(called->GetInput(port, false).status());
          }
          for (const auto& [port, unused] : outputs) {
            ABSL_RETURN_IF_ERROR(called->GetOutput(port, false).status());
          }
          const std::string id = called->GetId();
          bool inserted = false;
          {
            thread::MutexLock lock(&graph->mu);
            const auto [found, created] = graph->actions.emplace(id, called);
            if (!created && found->second != called) {
              return absl::AlreadyExistsError(
                  "host action id is already active");
            }
            inserted = created;
          }
          ABSL_RETURN_IF_ERROR(called->SetOnCancelled(
              [id](std::shared_ptr<a11::actions::Action>) {
                MAIN_THREAD_EM_ASM({
                  globalThis.__a11WasmActionCancelled(UTF8ToString($0));
                }, id.c_str());
                return absl::OkStatus();
              }));
          const absl::Status status = HostInvokeAction(id, name);
          if (inserted) {
            thread::MutexLock lock(&graph->mu);
            graph->actions.erase(id);
          }
          return status;
        });
  };
  return registry.Register(name, std::move(schema), std::move(handler));
}

absl::StatusOr<size_t> Limit(const nlohmann::json& request,
                             std::string_view key, size_t fallback) {
  const auto found = request.find(std::string(key));
  if (found == request.end()) {
    return fallback;
  }
  if (!found->is_number_unsigned()) {
    return absl::InvalidArgumentError(std::string(key) + " must be positive");
  }
  const auto value = found->get<std::uint64_t>();
  if (value == 0 || value > 1'000'000'000) {
    return absl::InvalidArgumentError(std::string(key) + " is out of range");
  }
  return static_cast<size_t>(value);
}

absl::StatusOr<nlohmann::json> Run(
    const nlohmann::json& request,
    const std::shared_ptr<a11::wasm::NativeObjects>& state) {
  if (!request.is_object()) {
    return absl::InvalidArgumentError("Flow run request must be an object");
  }
  const auto source = request.find("source");
  const auto flow_name = request.find("flow");
  if (source == request.end() || !source->is_string() ||
      flow_name == request.end() || !flow_name->is_string()) {
    return absl::InvalidArgumentError("Flow run requires source and flow");
  }
  ABSL_ASSIGN_OR_RETURN(const size_t timeout_ms,
                        Limit(request, "timeoutMs", 30'000));
  ABSL_ASSIGN_OR_RETURN(const size_t max_chunks,
                        Limit(request, "maxOutputChunks", 1024));
  ABSL_ASSIGN_OR_RETURN(const size_t max_bytes,
                        Limit(request, "maxOutputBytes", 32 * 1024 * 1024));
  bool stream_outputs = false;
  std::string stream_id;
  bool retain_nodes = false;
  std::string run_id;
  if (auto enabled = request.find("streamOutputs");
      enabled != request.end()) {
    if (!enabled->is_boolean()) {
      return absl::InvalidArgumentError("streamOutputs must be boolean");
    }
    stream_outputs = enabled->get<bool>();
  }
  if (stream_outputs) {
    const auto id = request.find("streamId");
    if (id == request.end() || !id->is_string() ||
        id->get_ref<const std::string&>().empty()) {
      return absl::InvalidArgumentError(
          "streamId must be a nonempty string");
    }
    stream_id = id->get<std::string>();
  }
  if (auto enabled = request.find("retainNodes"); enabled != request.end()) {
    if (!enabled->is_boolean()) {
      return absl::InvalidArgumentError("retainNodes must be boolean");
    }
    retain_nodes = enabled->get<bool>();
  }
  if (retain_nodes) {
    const auto id = request.find("runId");
    if (id == request.end() || !id->is_string() ||
        id->get_ref<const std::string&>().empty()) {
      return absl::InvalidArgumentError("runId must be a nonempty string");
    }
    run_id = id->get<std::string>();
  }
  const auto inputs = request.find("inputs");
  if (inputs != request.end() && !inputs->is_object()) {
    return absl::InvalidArgumentError("Flow inputs must be an object");
  }

  ABSL_ASSIGN_OR_RETURN(
      auto program, a11::flow::CompiledProgram::Compile(
                        source->get<std::string>(), "wasm.flow"));
  const auto* flow = program->Flow(flow_name->get_ref<const std::string&>());
  if (flow == nullptr) {
    return absl::NotFoundError("Flow name is not present in source");
  }
  ABSL_ASSIGN_OR_RETURN(auto schema, a11::flow::FlowSchema(flow->plan));
  ABSL_ASSIGN_OR_RETURN(auto handler,
                        a11::flow::MakeHandler(program,
                                               flow_name->get_ref<const std::string&>()));
  ABSL_ASSIGN_OR_RETURN(auto map, a11::nodes::NodeMap::Create());
  FlowMapGuard flow_map(state->graph, map);
  auto registry = std::make_shared<a11::actions::ActionRegistry>();
  ABSL_RETURN_IF_ERROR(a11::sdk::flow::RegisterTimeActions(*registry));
  const auto host = request.find("host");
  ABSL_ASSIGN_OR_RETURN(
      auto settings,
      a11::wasm::ParseHostSettings(
          host == request.end() ? nlohmann::json::object() : *host));
  ABSL_RETURN_IF_ERROR(a11::wasm::RegisterSystemActions(
      *registry, settings.environment, settings.register_environment));
  for (const std::string_view name : {"make_http_request", "web-fetch"}) {
    const bool enabled = name == "make_http_request"
                             ? settings.register_make_http_request
                             : settings.register_web_fetch;
    if (enabled) {
      ABSL_ASSIGN_OR_RETURN(auto http_schema,
                            a11::wasm::SchemaFromCatalogue(name));
      ABSL_RETURN_IF_ERROR(RegisterHostActionImpl(
          *registry, std::move(http_schema), state->graph));
    }
  }
  if (auto host_actions = request.find("hostActions");
      host_actions != request.end()) {
    if (!host_actions->is_array()) {
      return absl::InvalidArgumentError("hostActions must be an array");
    }
    for (const auto& description : *host_actions) {
      ABSL_ASSIGN_OR_RETURN(auto host_schema,
                            a11::actions::SchemaFromJson(description));
      ABSL_RETURN_IF_ERROR(RegisterHostActionImpl(
          *registry, std::move(host_schema), state->graph));
    }
  }
  ABSL_ASSIGN_OR_RETURN(auto action,
                        a11::actions::Action::Create(
                            schema, "wasm-flow", handler, map, nullptr,
                            nullptr, registry));

  absl::flat_hash_map<std::string, std::shared_ptr<a11::nodes::AsyncNode>>
      outputs;
  for (const auto& [name, unused] : schema.outputs) {
    ABSL_ASSIGN_OR_RETURN(auto node, action->GetOutput(name, false));
    outputs.emplace(name, std::move(node));
  }
  ABSL_RETURN_IF_ERROR(action->Run().status());
  for (const auto& [name, unused] : schema.inputs) {
    ABSL_ASSIGN_OR_RETURN(auto node, action->GetInput(name, false));
    if (inputs != request.end()) {
      const auto values = inputs->find(name);
      if (values != inputs->end()) {
        if (!values->is_array()) {
          return absl::InvalidArgumentError("Flow input must be a chunk array");
        }
        for (const auto& value : *values) {
          ABSL_ASSIGN_OR_RETURN(auto chunk,
                                a11::data::ChunkFromJsonValue(value));
          ABSL_RETURN_IF_ERROR(
              node->PutChunk(std::move(chunk)).Await().status());
        }
      }
    }
    ABSL_RETURN_IF_ERROR(node->Finalize({.wait = true}).Await().status());
  }
  const absl::Duration timeout = absl::Milliseconds(timeout_ms);
  const absl::Time deadline = absl::Now() + timeout;
  nlohmann::json result = nlohmann::json::object();
  thread::Mutex output_mu;
  size_t total_chunks = 0;
  size_t total_bytes = 0;
  std::vector<a11::Task> drains;
  drains.reserve(outputs.size());
  for (auto& [name, node] : outputs) {
    result[name] = nlohmann::json::array();
    drains.push_back(a11::SubmitTask(
        [name, node, action, &result, &output_mu, &total_chunks,
         &total_bytes, max_chunks, max_bytes, deadline, stream_outputs,
         stream_id]() -> absl::Status {
          for (;;) {
            ABSL_ASSIGN_OR_RETURN(auto chunk, node->NextChunk().Await(deadline));
            if (!chunk.has_value()) return absl::OkStatus();
            ABSL_ASSIGN_OR_RETURN(auto json,
                                  a11::data::ChunkToJsonValue(*chunk));
            bool exceeded = false;
            {
              thread::MutexLock lock(&output_mu);
              exceeded = total_chunks >= max_chunks ||
                         chunk->data.size() > max_bytes - total_bytes;
              if (!exceeded) {
                ++total_chunks;
                total_bytes += chunk->data.size();
                result[name].push_back(json);
              }
            }
            if (exceeded) {
              (void)action->Cancel();
              return absl::ResourceExhaustedError("Flow output limit exceeded");
            }
            if (stream_outputs) {
              ABSL_ASSIGN_OR_RETURN(
                  auto encoded,
                  a11::DumpJson(json, "Flow output event"));
              MAIN_THREAD_EM_ASM({
                globalThis.__a11WasmFlowOutput(
                    UTF8ToString($0), UTF8ToString($1),
                    JSON.parse(UTF8ToString($2)));
              }, stream_id.c_str(), name.c_str(), encoded.c_str());
            }
          }
        }));
  }
  const absl::Status action_status =
      action->Wait(timeout).Await(deadline).status();
  if (!action_status.ok()) {
    for (const auto& drain : drains) {
      if (!drain.IsReady()) (void)drain.Cancel();
    }
  }
  absl::Status output_status;
  for (const auto& drain : drains) {
    const absl::Status status = drain.Await(deadline).status();
    if (output_status.ok() && !status.ok()) output_status = status;
  }
  if (output_status.code() == absl::StatusCode::kResourceExhausted) {
    return output_status;
  }
  ABSL_RETURN_IF_ERROR(action_status);
  ABSL_RETURN_IF_ERROR(output_status);
  if (retain_nodes) ABSL_RETURN_IF_ERROR(flow_map.Retain(run_id));
  return result;
}

}  // namespace

absl::Status a11::wasm::RegisterHostAction(
    actions::ActionRegistry& registry, actions::ActionSchema schema,
    const std::shared_ptr<NativeGraph>& graph) {
  return RegisterHostActionImpl(registry, std::move(schema), graph);
}

absl::Status a11::wasm::HostInvokeSessionCallback(
    const std::string& session_id, const std::string& payload) {
  return HostInvokeCallback(session_id, payload, true);
}

extern "C" std::shared_ptr<a11::wasm::AsyncTask>* a11_wasm_native_flow_start(
    a11::wasm::NativeObjects* state, const std::uint8_t* data, size_t size) {
  if (state == nullptr) return nullptr;
  auto retained = std::make_shared<a11::wasm::NativeObjects>(*state);
  auto task = a11::wasm::AsyncTask::Start(
      [request_bytes =
           std::string(View(data, size)), retained]()
          -> absl::StatusOr<std::string> {
        ABSL_ASSIGN_OR_RETURN(
            auto request, a11::ParseJson(request_bytes, "Flow run request"));
        ABSL_ASSIGN_OR_RETURN(auto output, Run(request, retained));
        return a11::DumpJson(output, "Flow run result");
      });
  return new std::shared_ptr<a11::wasm::AsyncTask>(std::move(task));
}

extern "C" Result* a11_wasm_task_poll(
    std::shared_ptr<a11::wasm::AsyncTask>* task) {
  if (task == nullptr) {
    return Failure(absl::InvalidArgumentError("Task handle is null"));
  }
  return (*task)->Poll();
}

extern "C" void a11_wasm_task_launch(
    std::shared_ptr<a11::wasm::AsyncTask>* task) {
  if (task != nullptr) (*task)->Launch();
}

extern "C" void a11_wasm_task_cancel(
    std::shared_ptr<a11::wasm::AsyncTask>* task) {
  if (task != nullptr) {
    (*task)->Cancel();
  }
}

extern "C" void a11_wasm_task_free(
    std::shared_ptr<a11::wasm::AsyncTask>* task) {
  delete task;
}
