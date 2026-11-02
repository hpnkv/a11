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
Result* a11_wasm_wire_encode(const std::uint8_t* data, size_t size) {
  auto message = a11::data::WireMessageFromJson(View(data, size));
  if (!message.ok()) {
    return Failure(message.status());
  }
  auto bytes = message->ToMsgpack();
  if (!bytes.ok()) {
    return Failure(bytes.status());
  }
  return new Result{0, std::move(*bytes)};
}

Result* a11_wasm_wire_decode(const std::uint8_t* data, size_t size) {
  auto message = a11::data::WireMessage::FromMsgpack(View(data, size));
  if (!message.ok()) {
    return Failure(message.status());
  }
  auto encoded = a11::data::WireMessageToJson(*message);
  if (!encoded.ok()) {
    return Failure(encoded.status());
  }
  return new Result{0, std::move(*encoded)};
}

Result* a11_wasm_fragment_decode(const std::uint8_t* data, size_t size) {
  auto fragment = a11::data::NodeFragment::FromMsgpack(View(data, size));
  if (!fragment.ok()) {
    return Failure(fragment.status());
  }
  auto json = a11::data::NodeFragmentToJsonValue(*fragment);
  if (!json.ok()) {
    return Failure(json.status());
  }
  auto encoded = a11::DumpJson(*json, "fragment");
  if (!encoded.ok()) {
    return Failure(encoded.status());
  }
  return new Result{0, std::move(*encoded)};
}

}  // extern "C"
