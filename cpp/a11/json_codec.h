// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The only place A11 hands text or bytes to nlohmann, or asks for them.
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
 * @brief Parses JSON text, or explains why it is not JSON.
 *
 * @param encoded JSON text to parse.
 * @param what Names the document in the error, e.g. "WireMessage JSON".
 */
absl::StatusOr<nlohmann::json> ParseJson(std::string_view encoded,
                                         std::string_view what);

/**
 * @brief Whether @p text is valid UTF-8.
 *
 * Public because the answer has to be available *before* nlohmann is asked. See
 * DumpJson for why asking nlohmann is not safe.
 */
bool IsValidUtf8(std::string_view text);

/**
 * @brief The first string inside @p value that is not valid UTF-8, or nullptr.
 *
 * Recursive: the strings that come from outside are not only at the top level.
 * A response header's value and a directory entry's name are both fields of a
 * record, and both are exactly where something outside this process can put
 * arbitrary bytes.
 */
const nlohmann::json* FindUnencodableString(const nlohmann::json& value);

/**
 * @brief Serializes @p value, rejecting strings that are not valid UTF-8.
 *
 * Strict on purpose: JSON is defined over text, and a chunk holding arbitrary
 * bytes has to be encoded (base64, as `data/json.cc` does) rather than smuggled
 * into a string field where a peer's parser would reject it. This is the one
 * caller of nlohmann that wants the error rather than a replacement character.
 *
 * ### Why it checks rather than only catching
 *
 * nlohmann turns its `throw` into `std::abort()` in every translation unit
 * compiled `-fno-exceptions`, and most of A11 is. This file is compiled with
 * exceptions on so that the `try` below means something -- but `dump()` is a
 * template, and *whichever instantiation the linker keeps* decides whether a
 * bad string raises or aborts. An aborting one from another TU is a legal
 * choice, and it was the one being made: a `read_file` over a binary file
 * killed the process with no output at all.
 *
 * So the strings are checked here, where the answer depends on the bytes rather
 * than on how something was compiled, and the `try` stays as a second line for
 * everything else `dump()` can object to. A unit test cannot cover the
 * difference: test binaries are built with exceptions on, so the version under
 * test is the one that throws while the shipped library is the one that aborts.
 */
absl::StatusOr<std::string> DumpJson(const nlohmann::json& value,
                                     std::string_view what);

/**
 * @brief Serializes @p value, replacing anything that is not valid UTF-8.
 *
 * For a log line or a span attribute, where a lost byte is better than a lost
 * message and there is no peer to reject it.
 */
std::string DumpJsonLossy(const nlohmann::json& value);

/** @brief Encodes @p value as MessagePack, or says why it cannot be. */
absl::StatusOr<std::string> PackMsgpack(const nlohmann::json& value,
                                        std::string_view what);

/** @brief Decodes MessagePack bytes, or explains why they are not. */
absl::StatusOr<nlohmann::json> UnpackMsgpack(std::string_view encoded,
                                             std::string_view what);

}  // namespace a11

#endif  // A11_JSON_CODEC_H_
