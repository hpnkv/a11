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

#include "host_stream.h"

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <emscripten.h>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/time/time.h>

#include "a11/concurrency/executor.h"
#include "a11/data/json.h"
#include "a11/json_codec.h"
#include "a11/status.h"
#include "thread/boost_primitives.h"

namespace a11::wasm {
namespace {

absl::Status HostWireEvent(const std::string& id, const char* kind,
                           const std::string& payload) {
  constexpr int kPending = 0;
  constexpr int kCompleted = 1;
  constexpr size_t kStatusCapacity = 16 * 1024;
  constexpr int kInternalCode = static_cast<int>(absl::StatusCode::kInternal);
  std::atomic<int> done{kPending};
  std::array<char, kStatusCapacity> encoded{};
  MAIN_THREAD_ASYNC_EM_ASM({
    const complete = status => {
      let value;
      try { value = JSON.stringify(status); }
      catch { value = JSON.stringify({code: $5,
        message: 'Wire callback status could not be serialized'}); }
      if (typeof value !== 'string' || lengthBytesUTF8(value) >= $4) {
        value = JSON.stringify({code: $5,
          message: 'Wire callback status exceeds the bridge limit'});
      }
      stringToUTF8(value, $3, $4);
      Atomics.store(HEAP32, $6 >> 2, $7);
      Atomics.notify(HEAP32, $6 >> 2);
    };
    Promise.resolve().then(() => globalThis.__a11WasmWireEvent(
      UTF8ToString($0), UTF8ToString($1),
      JSON.parse(UTF8ToString($2)))).then(complete,
      error => complete({code: $5, message: String(error?.message ?? error)}));
  }, id.c_str(), kind, payload.c_str(), encoded.data(), encoded.size(),
     kInternalCode, &done, kCompleted);
  while (done.load(std::memory_order_acquire) == kPending) {
    thread::SleepFor(absl::Milliseconds(1));
  }
  ABSL_ASSIGN_OR_RETURN(auto value,
                        a11::ParseJson(encoded.data(), "host wire status"));
  ABSL_ASSIGN_OR_RETURN(auto status, a11::StatusFromJson(value));
  return status;
}

absl::StatusOr<std::string> EncodeMessage(const data::WireMessage& message) {
  ABSL_ASSIGN_OR_RETURN(auto value, data::WireMessageToJsonValue(message));
  return a11::DumpJson(value, "host wire message");
}

}  // namespace

absl::Status HostStreamEvent(const std::string& id, const char* kind,
                             const std::string& payload) {
  return HostWireEvent(id, kind, payload);
}

absl::StatusOr<std::shared_ptr<HostStreamPair>> HostStreamPair::Create(
    std::string id) {
  ABSL_ASSIGN_OR_RETURN(auto endpoints,
                        net::InProcessWireStream::CreatePair(
                            std::nullopt, std::nullopt, std::nullopt, id));
  auto pair = std::make_shared<HostStreamPair>(
      HostStreamPair{.id = std::move(id),
                     .session_side = std::move(endpoints.first),
                     .host_side = std::move(endpoints.second)});
  const std::weak_ptr<HostStreamPair> weak = pair;
  auto on_message = [weak](std::optional<data::WireMessage> message) {
    return a11::SubmitTask(
        [weak, message = std::move(message)]() mutable -> absl::Status {
          const auto pair = weak.lock();
          if (pair == nullptr) {
            return absl::CancelledError("host wire stream was released");
          }
          if (message.has_value()) {
            ABSL_ASSIGN_OR_RETURN(auto encoded, EncodeMessage(*message));
            return HostWireEvent(pair->id, "message", encoded);
          }
          data::WireMessage marker;
          marker.headers = pair->host_side->GetTrailers().value_or(
              data::ByteMap{});
          ABSL_ASSIGN_OR_RETURN(auto encoded, EncodeMessage(marker));
          return HostWireEvent(pair->id, "half_close", encoded);
        });
  };
  auto on_done = [weak]() {
    return a11::SubmitTask([weak]() -> absl::Status {
      const auto pair = weak.lock();
      if (pair == nullptr) return absl::OkStatus();
      ABSL_ASSIGN_OR_RETURN(
          auto encoded,
          a11::DumpJson(a11::StatusToJsonOrEmptyDetails(
                            pair->host_side->GetStatus()),
                        "host wire completion"));
      return HostWireEvent(pair->id, "done", encoded);
    });
  };
  ABSL_RETURN_IF_ERROR(pair->host_side->Start(
      std::move(on_message), std::move(on_done)).Await().status());
  return pair;
}

}  // namespace a11::wasm
