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

/** @file
 * @brief Bounded deterministic encodings for model-facing content.
 */
#ifndef A11_SDK_LLM_CONTEXT_TRANSFORM_H_
#define A11_SDK_LLM_CONTEXT_TRANSFORM_H_

#include <cstddef>
#include <string>
#include <string_view>

#include <absl/status/statusor.h>

namespace a11::sdk::llm_context {

/** Return JSON with representation whitespace removed, preserving all tokens.
 * @param text Complete UTF-8 JSON, including duplicate keys or large numbers.
 * @param max_bytes Maximum admitted input size.
 * @param max_depth Maximum nested object/array depth.
 * @return Compacted text; InvalidArgument for malformed JSON/UTF-8 and
 *         ResourceExhausted for size/depth limits.
 */
absl::StatusOr<std::string> CompactJson(std::string_view text,
                                        size_t max_bytes = 8 * 1024 * 1024,
                                        size_t max_depth = 128);

/** Encode repeated lines or JSON key lexemes in a reversible JSON envelope.
 * @param text Original UTF-8 content; JSON-key encoding requires valid JSON.
 * @param format a11.lines/v1 or a11.json-keys/v1.
 * @param max_bytes Maximum admitted input size. The output may be larger;
 *        callers selecting candidates count the full envelope overhead.
 * @return A byte-reversible encoding or a validation/limit status.
 */
absl::StatusOr<std::string> Encode(std::string_view text,
                                   std::string_view format,
                                   size_t max_bytes = 8 * 1024 * 1024);

/** Expand a context encoding, enforcing the expanded byte limit.
 * @param encoded Versioned encoding envelope produced by Encode.
 * @param max_bytes Maximum encoded input and expanded output size.
 * @return Exact original bytes or a validation/limit status. Expansion checks
 *         run-length multiplication before allocating the output.
 */
absl::StatusOr<std::string> Expand(std::string_view encoded,
                                   size_t max_bytes = 8 * 1024 * 1024);

/** Estimate tokens from UTF-8 bytes; this is not a provider tokenizer. */
size_t EstimateTokens(std::string_view text);

}  // namespace a11::sdk::llm_context
#endif  // A11_SDK_LLM_CONTEXT_TRANSFORM_H_
