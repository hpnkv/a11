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

#include "host_binary_channel.h"

#include <array>
#include <string>
#include <utility>

#include <emscripten.h>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/strings/escaping.h>

#include "a11/json_codec.h"
#include "a11/status.h"
#include "completion_signal.h"

namespace a11::wasm {

absl::StatusOr<nlohmann::json> HostBinaryChannel::Call(
    std::string_view kind, const nlohmann::json& payload) const {
  constexpr size_t kCapacity = 16 * 1024;
  constexpr int kInternalCode = static_cast<int>(absl::StatusCode::kInternal);
  ABSL_ASSIGN_OR_RETURN(auto encoded_payload,
                        a11::DumpJson(payload, "binary channel event"));
  const std::string kind_text(kind);
  CompletionSignal completion;
  std::array<char, kCapacity> encoded{};
  MAIN_THREAD_ASYNC_EM_ASM({
    const complete = value => {
      let text;
      try { text = JSON.stringify(value); }
      catch { text = JSON.stringify({status: {code: $5,
        message: 'Binary channel response could not be serialized'}}); }
      if (typeof text !== 'string' || lengthBytesUTF8(text) >= $4) {
        text = JSON.stringify({status: {code: $5,
          message: 'Binary channel response exceeds bridge limit'}});
      }
      stringToUTF8(text, $3, $4);
      globalThis.__a11WasmCompletion($6);
    };
    Promise.resolve().then(() => globalThis.__a11WasmBinaryEvent(
      UTF8ToString($0), UTF8ToString($1),
      JSON.parse(UTF8ToString($2)))).then(complete,
      error => complete({status: {code: $5,
        message: String(error?.message ?? error)}}));
  }, id_.c_str(), kind_text.c_str(), encoded_payload.c_str(),
     encoded.data(), encoded.size(), kInternalCode, &completion);
  completion.Wait();
  ABSL_ASSIGN_OR_RETURN(auto answer,
                        a11::ParseJson(encoded.data(), "binary channel reply"));
  if (!answer.is_object()) {
    return absl::InvalidArgumentError("Binary channel reply must be an object");
  }
  const auto status_value = answer.find("status");
  if (status_value == answer.end()) {
    return absl::InvalidArgumentError("Binary channel reply has no status");
  }
  ABSL_ASSIGN_OR_RETURN(auto status, a11::StatusFromJson(*status_value));
  ABSL_RETURN_IF_ERROR(status);
  const auto value = answer.find("value");
  return value == answer.end() ? nlohmann::json() : *value;
}

absl::Status HostBinaryChannel::SetCallbacks(
    net::internal::BinaryChannelCallbacks callbacks) {
  std::optional<absl::Status> pending;
  {
    thread::MutexLock lock(&mu_);
    callbacks_ = callbacks;
    pending = std::move(pending_error_);
    pending_error_.reset();
  }
  if (pending.has_value() && callbacks.on_error) {
    callbacks.on_error(std::move(*pending));
  }
  return absl::OkStatus();
}

absl::Status HostBinaryChannel::ResetCallbacks() {
  {
    thread::MutexLock lock(&mu_);
    callbacks_ = {};
  }
  return Call("reset", nullptr).status();
}

absl::Status HostBinaryChannel::Open() {
  return Call("open", nullptr).status();
}

absl::Status HostBinaryChannel::Send(std::string bytes) {
  return Call("send", absl::Base64Escape(bytes)).status();
}

absl::StatusOr<size_t> HostBinaryChannel::BufferedAmount() const {
  ABSL_ASSIGN_OR_RETURN(auto value, Call("buffered_amount", nullptr));
  if (!value.is_number_unsigned()) {
    return absl::InvalidArgumentError(
        "Binary channel buffered amount must be unsigned");
  }
  return value.get<size_t>();
}

absl::StatusOr<bool> HostBinaryChannel::IsOpen() const {
  ABSL_ASSIGN_OR_RETURN(auto value, Call("is_open", nullptr));
  if (!value.is_boolean()) {
    return absl::InvalidArgumentError("Binary channel open state must be boolean");
  }
  return value.get<bool>();
}

absl::Status HostBinaryChannel::Close() {
  return Call("close", nullptr).status();
}

absl::Status HostBinaryChannel::Abort(absl::Status status) {
  return Call("abort", a11::StatusToJsonOrEmptyDetails(status)).status();
}

absl::Status HostBinaryChannel::Emit(std::string_view kind,
                                      const nlohmann::json& payload) {
  net::internal::BinaryChannelCallbacks callbacks;
  {
    thread::MutexLock lock(&mu_);
    callbacks = callbacks_;
  }
  if (kind == "open") {
    if (callbacks.on_open) callbacks.on_open();
  } else if (kind == "message") {
    if (!payload.is_string()) {
      return absl::InvalidArgumentError("Binary packet must be base64 text");
    }
    std::string packet;
    if (!absl::Base64Unescape(payload.get<std::string>(), &packet)) {
      return absl::InvalidArgumentError("Binary packet base64 is invalid");
    }
    if (callbacks.on_message) callbacks.on_message(std::move(packet));
  } else if (kind == "error") {
    ABSL_ASSIGN_OR_RETURN(auto status, a11::StatusFromJson(payload));
    if (callbacks.on_error) {
      callbacks.on_error(std::move(status));
    } else {
      thread::MutexLock lock(&mu_);
      pending_error_ = std::move(status);
    }
  } else if (kind == "closed") {
    if (callbacks.on_closed) callbacks.on_closed();
  } else if (kind == "buffered_amount_low") {
    if (callbacks.on_buffered_amount_low) {
      callbacks.on_buffered_amount_low();
    }
  } else {
    return absl::UnimplementedError("Unknown binary channel event");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::shared_ptr<HostChannelWireStream>>
HostChannelWireStream::Create(
    const std::shared_ptr<HostBinaryChannel>& channel, std::string id,
    net::ChannelEndpointRole role, net::WireStreamOptions options,
    net::ChannelFramingOptions framing) {
  ABSL_ASSIGN_OR_RETURN(
      auto state,
      MakeState(channel, std::move(id), role,
                [channel]() { return channel->Open(); }, options, framing));
  return std::shared_ptr<HostChannelWireStream>(
      new HostChannelWireStream(std::move(state)));
}

}  // namespace a11::wasm
