// Copyright 2026 The A11 Authors.

#include "a11/net/wire_stream.h"

#include <cstddef>
#include <string>
#include <utility>

#include <absl/status/status.h>
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
    absl::Status status = data::ValidateName(folded);
    if (!status.ok())
      return status;
    normalized.insert_or_assign(std::move(folded), std::move(value));
  }
  return normalized;
}

}  // namespace a11::net
