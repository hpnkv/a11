// Copyright 2026 The A11 Authors.

#ifndef A11_DATA_JSON_H_
#define A11_DATA_JSON_H_

#include <string>
#include <string_view>

#include <absl/status/statusor.h>
#include <nlohmann/json_fwd.hpp>

#include "a11/data/types.h"

namespace a11::data {

// JSON encoding used by A11's HTTP/SSE transports.
absl::StatusOr<nlohmann::json> WireMessageToJsonValue(
    const WireMessage& message);
absl::StatusOr<std::string> WireMessageToJson(const WireMessage& message);
absl::StatusOr<WireMessage> WireMessageFromJsonValue(
    const nlohmann::json& value);
absl::StatusOr<WireMessage> WireMessageFromJson(std::string_view encoded);

}  // namespace a11::data

#endif  // A11_DATA_JSON_H_
