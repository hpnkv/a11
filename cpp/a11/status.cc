// Copyright 2026 The A11 Authors.

#include "a11/status.h"

#include <exception>
#include <string>
#include <utility>

#include <absl/strings/cord.h>
#include <absl/strings/str_cat.h>
#include <nlohmann/json.hpp>

namespace a11 {
namespace {

bool IsCanonicalCode(std::int64_t code) {
  return code >= static_cast<std::int64_t>(absl::StatusCode::kOk) &&
         code <= static_cast<std::int64_t>(absl::StatusCode::kUnauthenticated);
}

}  // namespace

absl::StatusCode StatusCodeFromHttp(int http_code) {
  if (http_code >= 200 && http_code < 300) {
    return absl::StatusCode::kOk;
  }
  switch (http_code) {
    case 400:
      return absl::StatusCode::kInvalidArgument;
    case 401:
      return absl::StatusCode::kUnauthenticated;
    case 403:
      return absl::StatusCode::kPermissionDenied;
    case 404:
      return absl::StatusCode::kNotFound;
    case 409:
      return absl::StatusCode::kAborted;
    case 429:
      return absl::StatusCode::kResourceExhausted;
    case 501:
      return absl::StatusCode::kUnimplemented;
    case 503:
      return absl::StatusCode::kUnavailable;
    default:
      break;
  }
  if (http_code >= 400 && http_code < 500) {
    return absl::StatusCode::kFailedPrecondition;
  }
  if (http_code >= 500 && http_code < 600) {
    return absl::StatusCode::kInternal;
  }
  return absl::StatusCode::kUnknown;
}

int StatusCodeToHttp(absl::StatusCode code) {
  switch (code) {
    case absl::StatusCode::kOk:
      return 200;
    case absl::StatusCode::kCancelled:
      return 499;
    case absl::StatusCode::kInvalidArgument:
    case absl::StatusCode::kFailedPrecondition:
    case absl::StatusCode::kOutOfRange:
      return 400;
    case absl::StatusCode::kDeadlineExceeded:
      return 504;
    case absl::StatusCode::kNotFound:
      return 404;
    case absl::StatusCode::kAlreadyExists:
    case absl::StatusCode::kAborted:
      return 409;
    case absl::StatusCode::kPermissionDenied:
      return 403;
    case absl::StatusCode::kResourceExhausted:
      return 429;
    case absl::StatusCode::kUnimplemented:
      return 501;
    case absl::StatusCode::kUnavailable:
      return 503;
    case absl::StatusCode::kUnauthenticated:
      return 401;
    case absl::StatusCode::kUnknown:
    case absl::StatusCode::kInternal:
    case absl::StatusCode::kDataLoss:
      return 500;
    default:
      return 500;
  }
}

absl::StatusCode StatusCodeFromWebSocket(std::uint16_t close_code) {
  switch (close_code) {
    case 1000:
      return absl::StatusCode::kOk;
    case 1001:
      return absl::StatusCode::kAborted;
    case 1002:
    case 1003:
    case 1007:
      return absl::StatusCode::kInvalidArgument;
    case 1008:
      return absl::StatusCode::kPermissionDenied;
    case 1009:
      return absl::StatusCode::kResourceExhausted;
    case 1011:
      return absl::StatusCode::kInternal;
    case 1012:
    case 1013:
      return absl::StatusCode::kUnavailable;
    case 4000:
      return absl::StatusCode::kCancelled;
    case 4002:
      return absl::StatusCode::kInvalidArgument;
    case 4003:
      return absl::StatusCode::kDeadlineExceeded;
    case 4004:
      return absl::StatusCode::kNotFound;
    case 4005:
      return absl::StatusCode::kAlreadyExists;
    case 4006:
      return absl::StatusCode::kPermissionDenied;
    case 4007:
      return absl::StatusCode::kUnauthenticated;
    case 4008:
      return absl::StatusCode::kResourceExhausted;
    case 4009:
      return absl::StatusCode::kFailedPrecondition;
    case 4010:
      return absl::StatusCode::kAborted;
    case 4011:
      return absl::StatusCode::kOutOfRange;
    case 4012:
      return absl::StatusCode::kUnimplemented;
    case 4013:
      return absl::StatusCode::kInternal;
    case 4014:
      return absl::StatusCode::kUnavailable;
    case 4015:
      return absl::StatusCode::kDataLoss;
    case 4001:
    default:
      return absl::StatusCode::kUnknown;
  }
}

std::uint16_t StatusCodeToWebSocket(absl::StatusCode code) {
  if (code == absl::StatusCode::kOk) {
    return 1000;
  }
  const int raw = static_cast<int>(code);
  if (raw >= 1 && raw <= 15) {
    return static_cast<std::uint16_t>(3999 + raw);
  }
  if (code == absl::StatusCode::kUnauthenticated) {
    return 4007;
  }
  return 4001;
}

absl::Status MakeStatus(absl::StatusCode code, std::string message,
                        nlohmann::json details) {
  absl::Status status(code, std::move(message));
  if (details.is_array() && !details.empty()) {
    try {
      status.SetPayload(kStatusDetailsPayloadUrl, absl::Cord(details.dump()));
    } catch (const std::exception& error) {
      return absl::InternalError(
          absl::StrCat("Failed to encode status details: ", error.what()));
    }
  }
  return status;
}

nlohmann::json StatusDetails(const absl::Status& status) {
  const std::optional<absl::Cord> payload =
      status.GetPayload(kStatusDetailsPayloadUrl);
  if (!payload.has_value()) {
    return nlohmann::json::array();
  }
  try {
    absl::Cord mutable_payload = *payload;
    nlohmann::json details =
        nlohmann::json::parse(std::string(mutable_payload.Flatten()));
    return details.is_array() ? std::move(details) : nlohmann::json::array();
  } catch (const std::exception&) {
    return nlohmann::json::array();
  }
}

absl::StatusOr<nlohmann::json> StatusToJson(const absl::Status& status) {
  try {
    return nlohmann::json{{"code", static_cast<int>(status.code())},
                          {"message", std::string(status.message())},
                          {"details", StatusDetails(status)}};
  } catch (const std::exception& error) {
    return absl::InternalError(
        absl::StrCat("Failed to create status JSON: ", error.what()));
  }
}

nlohmann::json StatusToJsonOrEmptyDetails(const absl::Status& status) {
  absl::StatusOr<nlohmann::json> encoded = StatusToJson(status);
  if (encoded.ok()) {
    return std::move(*encoded);
  }
  // Only the details payload can fail to encode, so drop just that.
  return nlohmann::json{{"code", static_cast<int>(status.code())},
                        {"message", std::string(status.message())},
                        {"details", nlohmann::json::array()}};
}

absl::StatusOr<absl::Status> StatusFromJson(const nlohmann::json& value) {
  try {
    if (!value.is_object() || value.find("code") == value.end() ||
        !value["code"].is_number_integer() ||
        value.find("message") == value.end() || !value["message"].is_string()) {
      absl::StatusOr<absl::Status> result;
      result.AssignStatus(
          absl::InvalidArgumentError("JSON does not contain a valid Status"));
      return result;
    }
    const std::int64_t raw_code = value["code"].get<std::int64_t>();
    if (!IsCanonicalCode(raw_code)) {
      absl::StatusOr<absl::Status> result;
      result.AssignStatus(
          absl::InvalidArgumentError("Status code is not canonical"));
      return result;
    }
    nlohmann::json details = nlohmann::json::array();
    if (value.find("details") != value.end()) {
      details = value["details"];
      if (!details.is_array()) {
        absl::StatusOr<absl::Status> result;
        result.AssignStatus(
            absl::InvalidArgumentError("Status details must be an array"));
        return result;
      }
    }
    return absl::StatusOr<absl::Status>(
        std::in_place,
        MakeStatus(static_cast<absl::StatusCode>(raw_code),
                   value["message"].get<std::string>(), std::move(details)));
  } catch (const std::exception& error) {
    absl::StatusOr<absl::Status> result;
    result.AssignStatus(absl::InvalidArgumentError(
        absl::StrCat("Failed to decode Status JSON: ", error.what())));
    return result;
  }
}

}  // namespace a11
