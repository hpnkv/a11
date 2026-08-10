// Copyright 2026 The A11 Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

/**
 * @file
 * @brief A11's error type and its protocol/JSON bridges.
 *
 * A11 uses Abseil's @c absl::Status (and the canonical @c absl::StatusCode
 * enumeration) as its error carrier rather than defining a bespoke type. This
 * header adds the pieces A11 layers on top: mappings between status codes and
 * the HTTP and WebSocket close codes that form part of the public wire
 * contract, and helpers that carry A11's optional structured "details"
 * payload through JSON (and, via JSON, MessagePack) round-trips.
 */

#ifndef A11_STATUS_H_
#define A11_STATUS_H_

#include <cstdint>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <nlohmann/json_fwd.hpp>

namespace a11 {

/**
 * @brief Maps an HTTP status code to a canonical status code.
 * @param http_code HTTP status code.
 * @return The corresponding @c absl::StatusCode; unknown codes map to
 *         @c kUnknown rather than an invented status.
 */
absl::StatusCode StatusCodeFromHttp(int http_code);
/**
 * @brief Maps a canonical status code to its HTTP status code.
 * @param code Canonical status code.
 * @return The corresponding HTTP status code.
 */
int StatusCodeToHttp(absl::StatusCode code);
/**
 * @brief Maps a WebSocket close code to a canonical status code.
 * @param close_code WebSocket close code.
 * @return The corresponding @c absl::StatusCode; unknown codes map to
 *         @c kUnknown.
 */
absl::StatusCode StatusCodeFromWebSocket(std::uint16_t close_code);
/**
 * @brief Maps a canonical status code to its WebSocket close code.
 * @param code Canonical status code.
 * @return The corresponding WebSocket close code.
 */
std::uint16_t StatusCodeToWebSocket(absl::StatusCode code);

/**
 * @brief Builds a status carrying an A11 structured-details payload.
 *
 * The @p details JSON is attached to the returned @c absl::Status so it
 * survives the JSON and MessagePack bridges used across the wire.
 *
 * @param code Canonical status code (use @c kOk for success).
 * @param message Human-readable message.
 * @param details Arbitrary structured detail payload.
 * @return A status combining @p code, @p message and @p details.
 */
absl::Status MakeStatus(absl::StatusCode code, std::string message,
                        nlohmann::json details);
/**
 * @brief Extracts the structured-details payload from a status.
 * @param status Status to inspect.
 * @return The attached details JSON, or a null/empty value when none is set.
 */
nlohmann::json StatusDetails(const absl::Status& status);

/**
 * @brief Serializes a status (code, message, details) to JSON.
 * @param status Status to serialize.
 * @return The JSON representation, or an error status on failure.
 */
absl::StatusOr<nlohmann::json> StatusToJson(const absl::Status& status);
/**
 * @brief Serializes a status to JSON, never failing.
 *
 * For diagnostics that embed a status in a larger document and have nothing
 * useful to do with an encoding failure. Falls back to the same fields with
 * empty details, so the layout is the one StatusToJson produces either way.
 * @param status Status to serialize.
 * @return The JSON representation.
 */
nlohmann::json StatusToJsonOrEmptyDetails(const absl::Status& status);
/**
 * @brief Reconstructs a status from its JSON representation.
 * @param value JSON produced by StatusToJson.
 * @return The reconstructed status, or an error status when @p value is
 *         not a valid Status document.
 */
absl::StatusOr<absl::Status> StatusFromJson(const nlohmann::json& value);

/** @brief Type URL identifying A11's status-details payload. */
constexpr std::string_view kStatusDetailsPayloadUrl =
    "type.a11.dev/status-details+json";

}  // namespace a11

#endif  // A11_STATUS_H_
