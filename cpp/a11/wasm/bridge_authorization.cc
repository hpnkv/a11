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

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <nlohmann/json.hpp>

#include "a11/actions/authorization.h"
#include "a11/json_codec.h"
#include "bridge_core.h"

namespace {
absl::StatusOr<a11::actions::AuthorizationEnvelope> ReadEnvelope(
    std::string_view bytes) {
  auto value = a11::ParseJson(bytes, "authorization envelope");
  if (!value.ok()) return value.status();
  if (!value->is_object()) {
    return absl::InvalidArgumentError(
        "Authorization envelope must be an object");
  }
  const auto version = value->find("version");
  const auto chain = value->find("chain");
  if (version == value->end() || !version->is_number_integer() ||
      chain == value->end() || !chain->is_array()) {
    return absl::InvalidArgumentError("Invalid authorization envelope");
  }
  a11::actions::AuthorizationEnvelope envelope;
  envelope.version = version->get<int>();
  for (const auto& statement : *chain) {
    if (!statement.is_string()) {
      return absl::InvalidArgumentError(
          "Authorization chain must contain strings");
    }
    envelope.chain.push_back(statement.get<std::string>());
  }
  return envelope;
}

Result* WriteEnvelope(
    const absl::StatusOr<a11::actions::AuthorizationEnvelope>& envelope) {
  if (!envelope.ok()) return Failure(envelope.status());
  auto json = a11::DumpJson(nlohmann::json{
      {"version", envelope->version}, {"chain", envelope->chain}},
      "authorization envelope");
  return json.ok() ? new Result{0, std::move(*json)}
                   : Failure(json.status());
}
}  // namespace

extern "C" {
Result* a11_wasm_authorization_encode(const std::uint8_t* data,
                                       size_t size) {
  auto envelope = ReadEnvelope(View(data, size));
  if (!envelope.ok()) return Failure(envelope.status());
  auto encoded = a11::actions::EncodeAuthorization(*envelope);
  return encoded.ok() ? new Result{0, std::move(*encoded)}
                      : Failure(encoded.status());
}

Result* a11_wasm_authorization_decode(const std::uint8_t* data,
                                       size_t size) {
  return WriteEnvelope(a11::actions::DecodeAuthorization(View(data, size)));
}

Result* a11_wasm_authorization_to_text(const std::uint8_t* data,
                                        size_t size) {
  auto envelope = ReadEnvelope(View(data, size));
  if (!envelope.ok()) return Failure(envelope.status());
  auto encoded = a11::actions::AuthorizationToText(*envelope);
  return encoded.ok() ? new Result{0, std::move(*encoded)}
                      : Failure(encoded.status());
}

Result* a11_wasm_authorization_from_text(const char* data, size_t size) {
  return WriteEnvelope(a11::actions::AuthorizationFromText(View(data, size)));
}
}  // extern "C"
