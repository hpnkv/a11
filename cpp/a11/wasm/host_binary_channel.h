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

#ifndef A11_WASM_HOST_BINARY_CHANNEL_H_
#define A11_WASM_HOST_BINARY_CHANNEL_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <nlohmann/json.hpp>

#include "a11/net/internal/binary_channel.h"
#include "a11/net/channel_wire_stream.h"
#include "thread/boost_primitives.h"

namespace a11::wasm {

class HostBinaryChannel final : public net::internal::BinaryChannel {
 public:
  explicit HostBinaryChannel(std::string id) : id_(std::move(id)) {}

  absl::Status SetCallbacks(net::internal::BinaryChannelCallbacks callbacks)
      override;
  absl::Status ResetCallbacks() override;
  absl::Status Open() override;
  absl::Status Send(std::string bytes) override;
  absl::StatusOr<size_t> BufferedAmount() const override;
  absl::StatusOr<bool> IsOpen() const override;
  absl::Status Close() override;
  absl::Status Abort(absl::Status status) override;
  void* absl_nullable GetImpl() const override { return nullptr; }

  absl::Status Emit(std::string_view kind, const nlohmann::json& payload);

 private:
  absl::StatusOr<nlohmann::json> Call(std::string_view kind,
                                      const nlohmann::json& payload) const;

  std::string id_;
  mutable thread::Mutex mu_;
  net::internal::BinaryChannelCallbacks callbacks_ ABSL_GUARDED_BY(mu_);
  std::optional<absl::Status> pending_error_ ABSL_GUARDED_BY(mu_);
};

class HostChannelWireStream final : public net::ChannelWireStream {
 public:
  static absl::StatusOr<std::shared_ptr<HostChannelWireStream>> Create(
      const std::shared_ptr<HostBinaryChannel>& channel, std::string id,
      net::ChannelEndpointRole role, net::WireStreamOptions options,
      net::ChannelFramingOptions framing);

 private:
  explicit HostChannelWireStream(std::shared_ptr<State> state)
      : net::ChannelWireStream(std::move(state)) {}
};

}  // namespace a11::wasm

#endif  // A11_WASM_HOST_BINARY_CHANNEL_H_
