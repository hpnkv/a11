// Copyright 2026 The A11 Authors

#include "a11/actions/authorization.h"
#include "a11/actions/authorization_codec_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/escaping.h>
#include <absl/strings/str_cat.h>
#include <nlohmann/json.hpp>

#include "a11/data/msgpack.h"

namespace a11::actions {
namespace {
absl::Status Validate(const AuthorizationEnvelope& envelope) {
  if (envelope.version != kAuthorizationVersion) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Authorization version ", envelope.version, " is not supported"));
  }
  if (envelope.chain.empty() || envelope.chain.size() > kMaxAuthorizationHops) {
    return absl::InvalidArgumentError(
        "An authorization chain must contain between one and eight statements");
  }
  for (const std::string& statement : envelope.chain) {
    if (statement.empty() ||
        std::count(statement.begin(), statement.end(), '.') != 2 ||
        std::any_of(statement.begin(), statement.end(),
                    [](unsigned char c) { return c > 0x7f; })) {
      return absl::InvalidArgumentError(
          "Every authorization statement must be an ASCII compact JWS");
    }
  }
  return absl::OkStatus();
}

}  // namespace

namespace internal {
std::string UnpaddedWebSafeBase64(std::string_view value) {
  std::string encoded = absl::WebSafeBase64Escape(value);
  while (encoded.ends_with('=')) {
    encoded.pop_back();
  }
  return encoded;
}

}  // namespace internal

absl::StatusOr<std::string> EncodeAuthorization(
    const AuthorizationEnvelope& envelope) {
  ABSL_RETURN_IF_ERROR(Validate(envelope));
  nlohmann::json value = nlohmann::json::array();
  value.push_back(envelope.version);
  value.push_back(envelope.chain);
  data::MsgpackWriter writer;
  ABSL_RETURN_IF_ERROR(writer.Pack(value));
  std::string encoded = writer.TakeBytes();
  if (encoded.size() > kMaxAuthorizationBytes) {
    return absl::InvalidArgumentError(
        "An authorization value may contain at most 16384 bytes");
  }
  return encoded;
}

absl::StatusOr<AuthorizationEnvelope> DecodeAuthorization(
    std::string_view value) {
  if (value.size() > kMaxAuthorizationBytes) {
    return absl::InvalidArgumentError(
        "An authorization value may contain at most 16384 bytes");
  }
  data::MsgpackReader reader(value);
  ABSL_ASSIGN_OR_RETURN(nlohmann::json decoded, reader.Read());
  ABSL_RETURN_IF_ERROR(reader.EnsureFullyConsumed());
  if (!decoded.is_array() || decoded.size() != 2 ||
      !decoded[0].is_number_integer() || !decoded[1].is_array()) {
    return absl::InvalidArgumentError(
        "The authorization envelope must be [version, chain]");
  }
  AuthorizationEnvelope envelope;
  const std::int64_t version = decoded[0].get<std::int64_t>();
  if (version != kAuthorizationVersion) {
    return absl::InvalidArgumentError(
        "The authorization version is not supported");
  }
  envelope.version = kAuthorizationVersion;
  for (const nlohmann::json& statement : decoded[1]) {
    if (!statement.is_string()) {
      return absl::InvalidArgumentError(
          "The authorization chain must contain JWS strings");
    }
    envelope.chain.push_back(statement.get<std::string>());
  }
  ABSL_ASSIGN_OR_RETURN(const std::string canonical,
                        EncodeAuthorization(envelope));
  if (canonical != value) {
    return absl::InvalidArgumentError(
        "The authorization envelope is not canonically encoded");
  }
  return envelope;
}

absl::StatusOr<std::string> AuthorizationToText(
    const AuthorizationEnvelope& envelope) {
  ABSL_ASSIGN_OR_RETURN(const std::string encoded,
                        EncodeAuthorization(envelope));
  return absl::StrCat("a11-auth/1.", internal::UnpaddedWebSafeBase64(encoded));
}

absl::StatusOr<AuthorizationEnvelope> AuthorizationFromText(
    std::string_view value) {
  constexpr std::string_view prefix = "a11-auth/1.";
  if (!value.starts_with(prefix)) {
    return absl::InvalidArgumentError(
        "The authorization value must start with 'a11-auth/1.'");
  }
  const std::string_view body = value.substr(prefix.size());
  if (std::any_of(body.begin(), body.end(), [](unsigned char c) {
        return !(std::isalnum(c) != 0 || c == '-' || c == '_');
      })) {
    return absl::InvalidArgumentError(
        "The textual authorization value is not base64url");
  }
  std::string decoded;
  if (!absl::WebSafeBase64Unescape(body, &decoded)) {
    return absl::InvalidArgumentError(
        "The textual authorization value is not base64url");
  }
  return DecodeAuthorization(decoded);
}

}  // namespace a11::actions
