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

#include "a11/net/wire_stream.h"

#include <cstddef>
#include <string>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/strings/ascii.h>

#include "a11/data/types.h"

namespace a11::net {

absl::Status WireStreamOptions::Validate() const {
  if (max_buffered_incoming_messages < 1 ||
      max_buffered_incoming_messages > 1024) {
    return absl::InvalidArgumentError(
        "max_buffered_incoming_messages must be in [1, 1024]");
  }
  const size_t minimum = data::EmptyWireMessageSize();
  if (max_single_message_size < minimum ||
      max_single_message_size > kMaxSingleMessageSize) {
    return absl::InvalidArgumentError(
        "max_single_message_size is outside the supported range");
  }
  if (max_buffered_incoming_bytes < minimum) {
    return absl::InvalidArgumentError(
        "max_buffered_incoming_bytes is smaller than an empty message");
  }
  if (message_timeout < absl::ZeroDuration() &&
      message_timeout != absl::InfiniteDuration()) {
    return absl::InvalidArgumentError(
        "message_timeout must be non-negative or infinite");
  }
  return absl::OkStatus();
}

absl::StatusOr<data::ByteMap> NormalizeWireHeaders(data::ByteMap headers) {
  data::ByteMap normalized;
  for (auto& [key, value] : headers) {
    std::string folded = absl::AsciiStrToLower(key);
    ABSL_RETURN_IF_ERROR(data::ValidateName(folded));
    normalized.insert_or_assign(std::move(folded), std::move(value));
  }
  return normalized;
}

}  // namespace a11::net
