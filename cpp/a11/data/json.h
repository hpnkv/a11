/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
