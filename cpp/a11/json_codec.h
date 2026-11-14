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

/**
 * @file
 * @brief Status-returning JSON and MessagePack codecs.
 *
 * These functions convert untrusted JSON and MessagePack input into status
 * errors. `json_codec.cc` enables exceptions for the nlohmann operations that
 * have no non-throwing overload; other A11 translation units remain
 * `-fno-exceptions`.
 */

#ifndef A11_JSON_CODEC_H_
#define A11_JSON_CODEC_H_

#include <string>
#include <string_view>

#include <absl/status/statusor.h>
#include <nlohmann/json.hpp>

namespace a11 {

/**
 * @brief Parses JSON text, returning a status for invalid input.
 *
 * @param encoded JSON text to parse.
 * @param what Names the document in the error, e.g. "WireMessage JSON".
 */
absl::StatusOr<nlohmann::json> ParseJson(std::string_view encoded,
                                         std::string_view what);

/** Parse host fields retaining their declared insertion order. */
absl::StatusOr<nlohmann::ordered_json> ParseOrderedJson(
    std::string_view encoded, std::string_view what);

/**
 * @brief Whether @p text is valid strict UTF-8.
 *
 * Rejects overlong encodings, surrogate code points and values above U+10FFFF.
 */
bool IsValidUtf8(std::string_view text);

/**
 * @brief The first string inside @p value that is not valid UTF-8, or nullptr.
 *
 * Traverses nested fields iteratively to bound stack use on fixed fiber stacks.
 */
const nlohmann::json* FindUnencodableString(const nlohmann::json& value);

/**
 * @brief Serializes @p value, rejecting strings that are not valid UTF-8.
 *
 * Checks strings before calling nlohmann's templated dump(). A linked
 * instantiation from a `-fno-exceptions` translation unit may abort on invalid
 * UTF-8. Other serialization errors are caught in json_codec.cc.
 */
absl::StatusOr<std::string> DumpJson(const nlohmann::json& value,
                                     std::string_view what);

/** Serialize host fields retaining their declared insertion order. */
absl::StatusOr<std::string> DumpOrderedJson(const nlohmann::ordered_json& value,
                                            std::string_view what);

/**
 * @brief Serializes @p value, replacing anything that is not valid UTF-8.
 *
 * Use for logs and span attributes that can accept replacement characters.
 */
std::string DumpJsonLossy(const nlohmann::json& value);

/** @brief Encodes @p value as MessagePack, or says why it cannot be. */
absl::StatusOr<std::string> PackMsgpack(const nlohmann::json& value,
                                        std::string_view what);

/** Encode host fields retaining their declared insertion order. */
absl::StatusOr<std::string> PackOrderedMsgpack(
    const nlohmann::ordered_json& value, std::string_view what);

/** @brief Decodes MessagePack bytes, or explains why they are not. */
absl::StatusOr<nlohmann::json> UnpackMsgpack(std::string_view encoded,
                                             std::string_view what);

}  // namespace a11

#endif  // A11_JSON_CODEC_H_
