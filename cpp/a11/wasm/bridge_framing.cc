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
Result* a11_wasm_frame_split(const std::uint8_t* data, size_t size,
                             std::uint64_t transient_id, size_t packet_size) {
  auto packets = a11::net::SplitBytesIntoPackets(View(data, size), transient_id,
                                                 packet_size);
  if (!packets.ok()) {
    return Failure(packets.status());
  }
  std::string encoded;
  for (const auto& packet : *packets) {
    const auto length = static_cast<std::uint32_t>(packet.size());
    for (unsigned shift = 0; shift < 32; shift += 8) {
      encoded.push_back(static_cast<char>((length >> shift) & 0xff));
    }
    encoded.append(packet);
  }
  return new Result{0, std::move(encoded)};
}

a11::net::ByteReassembler* a11_wasm_reassembler_new(size_t packet_size,
                                                    size_t max_message_size,
                                                    size_t max_pending_messages,
                                                    size_t max_pending_bytes) {
  a11::net::ByteChunkingOptions options{
      packet_size, max_message_size, max_pending_messages, max_pending_bytes};
  if (!options.Validate().ok()) {
    return nullptr;
  }
  return new a11::net::ByteReassembler(options);
}

void a11_wasm_reassembler_free(a11::net::ByteReassembler* reassembler) {
  delete reassembler;
}

Result* a11_wasm_reassembler_feed(a11::net::ByteReassembler* reassembler,
                                  const std::uint8_t* data, size_t size) {
  auto result = reassembler->Feed(std::string(View(data, size)));
  if (!result.ok()) {
    return Failure(result.status());
  }
  if (!result->has_value()) {
    return new Result{0, {}, false};
  }
  return new Result{0, std::move(**result)};
}

}  // extern "C"
