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

#include <string>

#include <absl/status/status.h>
#include <gtest/gtest.h>

#include "sdk/llm_context/transform.h"

namespace a11::sdk::llm_context {
namespace {
TEST(LlmContextTransform, JsonPreservesDuplicateKeysAndNumericTokens) {
  auto result =
      CompactJson(R"( { "a": 1.2300, "a": 1234567890123456789012345 } )");
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(*result, R"({"a":1.2300,"a":1234567890123456789012345})");
}

TEST(LlmContextTransform, RejectsDepthAndInvalidUtf8) {
  EXPECT_EQ(CompactJson(std::string(129, '[') + "0" + std::string(129, ']'))
                .status()
                .code(),
            absl::StatusCode::kResourceExhausted);
  EXPECT_FALSE(CompactJson(std::string("\xff", 1)).ok());
}

TEST(LlmContextTransform, FormatsRoundtripExactly) {
  for (const auto* format : {"a11.lines/v1", "a11.json-keys/v1"}) {
    const std::string source = R"( [{ "key": 1.2300}, {"key": 2}] )";
    auto encoded = Encode(source, format);
    ASSERT_TRUE(encoded.ok()) << encoded.status();
    auto expanded = Expand(*encoded);
    ASSERT_TRUE(expanded.ok()) << expanded.status();
    EXPECT_EQ(*expanded, source);
  }
}

TEST(LlmContextTransform, RejectsUnknownEmptyEnvelopeAndExpansionBomb) {
  EXPECT_FALSE(Expand(R"({"format":"unknown","parts":[]})").ok());
  EXPECT_EQ(Expand(R"({"format":"a11.lines/v1","parts":[[99999999,"x"]]})", 256)
                .status()
                .code(),
            absl::StatusCode::kResourceExhausted);
}
}  // namespace
}  // namespace a11::sdk::llm_context
