// Copyright 2026 The A11 Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

#ifndef A11_STATUS_H_
#define A11_STATUS_H_

#include <cstdint>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <nlohmann/json_fwd.hpp>

namespace a11 {

// HTTP and WebSocket mappings are part of the public wire contract. Unknown
// protocol codes deliberately map to kUnknown rather than inventing a status.
absl::StatusCode StatusCodeFromHttp(int http_code);
int StatusCodeToHttp(absl::StatusCode code);
absl::StatusCode StatusCodeFromWebSocket(std::uint16_t close_code);
std::uint16_t StatusCodeToWebSocket(absl::StatusCode code);

// Abseil Status remains the error carrier. A11's optional structured details
// are retained as a JSON Status payload and survive JSON/MessagePack bridges.
absl::Status MakeStatus(absl::StatusCode code, std::string message,
                        nlohmann::json details);
nlohmann::json StatusDetails(const absl::Status& status);

absl::StatusOr<nlohmann::json> StatusToJson(const absl::Status& status);
absl::StatusOr<absl::Status> StatusFromJson(const nlohmann::json& value);

constexpr std::string_view kStatusDetailsPayloadUrl =
    "type.a11.dev/status-details+json";

}  // namespace a11

#endif  // A11_STATUS_H_
