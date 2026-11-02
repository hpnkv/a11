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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/data/json.h"
#include "a11/data/types.h"
#include "a11/flow/service.h"
#include "a11/json_codec.h"
#include "a11/net/byte_chunking.h"
#include "bridge_core.h"
#include "runtime.h"

extern "C" {
a11::wasm::Runtime* a11_wasm_runtime_new(const char* storage_id, size_t size) {
  return new a11::wasm::Runtime(std::string(View(storage_id, size)));
}

void a11_wasm_runtime_free(a11::wasm::Runtime* runtime) {
  delete runtime;
}

Result* a11_wasm_runtime_register(a11::wasm::Runtime* runtime, const char* name,
                                  size_t size) {
  return FromStatus(runtime->RegisterAction(std::string(View(name, size))));
}

Result* a11_wasm_runtime_invoke(a11::wasm::Runtime* runtime,
                                const std::uint8_t* data, size_t size) {
  auto json = a11::ParseJson(View(data, size), "action");
  if (!json.ok()) {
    return Failure(json.status());
  }
  auto action = a11::data::ActionMessageFromJsonValue(*json);
  if (!action.ok()) {
    return Failure(action.status());
  }
  return FromStatus(runtime->Invoke(std::move(*action)));
}

Result* a11_wasm_runtime_next_action(a11::wasm::Runtime* runtime) {
  auto action = runtime->NextAction();
  if (!action.ok()) {
    return Failure(action.status());
  }
  if (!action->has_value()) {
    return new Result{0, "null"};
  }
  auto json = a11::data::ActionMessageToJsonValue(**action);
  if (!json.ok()) {
    return Failure(json.status());
  }
  auto encoded = a11::DumpJson(*json, "action");
  if (!encoded.ok()) {
    return Failure(encoded.status());
  }
  return new Result{0, std::move(*encoded)};
}

Result* a11_wasm_runtime_finish_action(a11::wasm::Runtime* runtime,
                                       const char* id, size_t id_size, int code,
                                       const char* message,
                                       size_t message_size) {
  const auto outcome = code == 0
                           ? absl::OkStatus()
                           : absl::Status(static_cast<absl::StatusCode>(code),
                                          View(message, message_size));
  return FromStatus(runtime->FinishAction(View(id, id_size), outcome));
}

Result* a11_wasm_runtime_node_append(a11::wasm::Runtime* runtime,
                                     const char* id, size_t id_size,
                                     const std::uint8_t* data, size_t size,
                                     const char* mimetype,
                                     size_t mimetype_size) {
  a11::data::Chunk chunk;
  chunk.data = std::string(View(data, size));
  if (mimetype_size != 0) {
    chunk.metadata = a11::data::ChunkMetadata{};
    chunk.metadata->mimetype = std::string(View(mimetype, mimetype_size));
  }
  a11::data::NodeFragment fragment;
  fragment.id = std::string(View(id, id_size));
  fragment.data = std::move(chunk);
  return FromStatus(runtime->Append(std::move(fragment)));
}

Result* a11_wasm_runtime_node_append_ref(a11::wasm::Runtime* runtime,
                                         const char* id, size_t id_size,
                                         const std::uint8_t* data,
                                         size_t size) {
  auto json = a11::ParseJson(View(data, size), "node reference");
  if (!json.ok()) {
    return Failure(json.status());
  }
  auto ref = a11::data::NodeRefFromJsonValue(*json);
  if (!ref.ok()) {
    return Failure(ref.status());
  }
  a11::data::NodeFragment fragment;
  fragment.id = std::string(View(id, id_size));
  fragment.data = std::move(*ref);
  return FromStatus(runtime->Append(std::move(fragment)));
}

Result* a11_wasm_runtime_node_put_fragment(a11::wasm::Runtime* runtime,
                                           const std::uint8_t* data,
                                           size_t size) {
  auto json = a11::ParseJson(View(data, size), "node fragment");
  if (!json.ok()) {
    return Failure(json.status());
  }
  auto fragment = a11::data::NodeFragmentFromJsonValue(*json);
  if (!fragment.ok()) {
    return Failure(fragment.status());
  }
  auto seq = runtime->Put(std::move(*fragment));
  if (!seq.ok()) {
    return Failure(seq.status());
  }
  return new Result{0, std::to_string(*seq)};
}

Result* a11_wasm_runtime_node_put_many(a11::wasm::Runtime* runtime,
                                       const std::uint8_t* data, size_t size) {
  auto json = a11::ParseJson(View(data, size), "node fragments");
  if (!json.ok()) {
    return Failure(json.status());
  }
  if (!json->is_array()) {
    return Failure(absl::InvalidArgumentError("fragments must be an array"));
  }
  std::vector<a11::data::NodeFragment> fragments;
  fragments.reserve(json->size());
  for (const auto& item : *json) {
    auto fragment = a11::data::NodeFragmentFromJsonValue(item);
    if (!fragment.ok()) {
      return Failure(fragment.status());
    }
    fragments.push_back(std::move(*fragment));
  }
  auto seqs = runtime->PutMany(std::move(fragments));
  if (!seqs.ok()) {
    return Failure(seqs.status());
  }
  auto encoded = a11::DumpJson(*seqs, "assigned sequences");
  if (!encoded.ok()) {
    return Failure(encoded.status());
  }
  return new Result{0, std::move(*encoded)};
}

Result* a11_wasm_runtime_node_close(a11::wasm::Runtime* runtime, const char* id,
                                    size_t size, int code, const char* message,
                                    size_t message_size) {
  const auto outcome = code == 0
                           ? absl::OkStatus()
                           : absl::Status(static_cast<absl::StatusCode>(code),
                                          View(message, message_size));
  return FromStatus(runtime->Close(View(id, size), outcome));
}

Result* a11_wasm_runtime_node_read(a11::wasm::Runtime* runtime, const char* id,
                                   size_t size, size_t index) {
  auto fragment = runtime->Read(View(id, size), index);
  if (!fragment.ok()) {
    return Failure(fragment.status());
  }
  auto encoded = fragment->ToMsgpack();
  if (!encoded.ok()) {
    return Failure(encoded.status());
  }
  return new Result{0, std::move(*encoded)};
}

Result* a11_wasm_runtime_node_read_arrival(a11::wasm::Runtime* runtime,
                                           const char* id, size_t size,
                                           size_t arrival) {
  auto fragment = runtime->ReadArrival(View(id, size), arrival);
  if (!fragment.ok()) {
    return Failure(fragment.status());
  }
  auto encoded = fragment->ToMsgpack();
  if (!encoded.ok()) {
    return Failure(encoded.status());
  }
  return new Result{0, std::move(*encoded)};
}

Result* a11_wasm_runtime_node_clear_data(a11::wasm::Runtime* runtime,
                                         const char* id, size_t size,
                                         std::uint32_t seq) {
  auto fragment = runtime->ClearData(View(id, size), seq);
  if (!fragment.ok()) {
    return Failure(fragment.status());
  }
  auto encoded = fragment->ToMsgpack();
  if (!encoded.ok()) {
    return Failure(encoded.status());
  }
  return new Result{0, std::move(*encoded)};
}

Result* a11_wasm_runtime_node_arrival_seq(a11::wasm::Runtime* runtime,
                                          const char* id, size_t size,
                                          size_t arrival) {
  auto seq = runtime->ArrivalSeq(View(id, size), arrival);
  if (!seq.ok()) {
    return Failure(seq.status());
  }
  return new Result{0, std::to_string(*seq)};
}

Result* a11_wasm_runtime_node_final_seq(a11::wasm::Runtime* runtime,
                                        const char* id, size_t size) {
  auto seq = runtime->FinalSeq(View(id, size));
  if (!seq.ok()) {
    return Failure(seq.status());
  }
  return new Result{0, seq->has_value() ? std::to_string(**seq) : "null"};
}

Result* a11_wasm_runtime_node_read_data(a11::wasm::Runtime* runtime,
                                        const char* id, size_t size,
                                        size_t index) {
  auto fragment = runtime->Read(View(id, size), index);
  if (!fragment.ok()) {
    return Failure(fragment.status());
  }
  auto chunk = fragment->GetChunk();
  if (!chunk.ok()) {
    return Failure(chunk.status());
  }
  auto status = (*chunk)->Materialize();
  if (!status.ok()) {
    return Failure(status);
  }
  return new Result{0, (*chunk)->data};
}

Result* a11_wasm_runtime_node_count(a11::wasm::Runtime* runtime, const char* id,
                                    size_t size) {
  auto count = runtime->Count(View(id, size));
  if (!count.ok()) {
    return Failure(count.status());
  }
  return new Result{0, std::to_string(*count)};
}

Result* a11_wasm_runtime_node_closed(a11::wasm::Runtime* runtime,
                                     const char* id, size_t size) {
  auto closed = runtime->Closed(View(id, size));
  if (!closed.ok()) {
    return Failure(closed.status());
  }
  return new Result{0, *closed ? "true" : "false"};
}

Result* a11_wasm_runtime_resolve(a11::wasm::Runtime* runtime,
                                 const std::uint8_t* data, size_t size,
                                 size_t max_depth, size_t max_chunks) {
  auto json = a11::ParseJson(View(data, size), "node reference");
  if (!json.ok()) {
    return Failure(json.status());
  }
  auto ref = a11::data::NodeRefFromJsonValue(*json);
  if (!ref.ok()) {
    return Failure(ref.status());
  }
  auto chunks = runtime->Resolve(*ref, max_depth, max_chunks);
  if (!chunks.ok()) {
    return Failure(chunks.status());
  }
  a11::data::WireMessage wire;
  for (auto& chunk : *chunks) {
    a11::data::NodeFragment fragment;
    fragment.id = ref->id;
    fragment.data = std::move(chunk);
    fragment.seq = static_cast<std::uint32_t>(wire.node_fragments.size());
    fragment.continued = true;
    wire.node_fragments.push_back(std::move(fragment));
  }
  if (!wire.node_fragments.empty()) {
    wire.node_fragments.back().continued = false;
  }
  auto encoded = wire.ToMsgpack();
  if (!encoded.ok()) {
    return Failure(encoded.status());
  }
  return new Result{0, std::move(*encoded)};
}

Result* a11_wasm_runtime_export_snapshot(a11::wasm::Runtime* runtime) {
  auto snapshot = runtime->ExportSnapshot();
  if (!snapshot.ok()) {
    return Failure(snapshot.status());
  }
  return new Result{0, std::move(*snapshot)};
}

Result* a11_wasm_runtime_import_snapshot(a11::wasm::Runtime* runtime,
                                         const std::uint8_t* data,
                                         size_t size) {
  return FromStatus(runtime->ImportSnapshot(View(data, size)));
}

}  // extern "C"
