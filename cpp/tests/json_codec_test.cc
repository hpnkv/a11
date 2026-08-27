// Copyright 2026 The A11 Authors.

// The UTF-8 guard in front of nlohmann.

#include "a11/json_codec.h"

#include <string>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace a11 {
namespace {

/// Not valid UTF-8: a lead byte promising a continuation that never comes.
const std::string kBadBytes = std::string("\xC3\x28", 2);

TEST(JsonCodecTest, AcceptsTextAndRejectsWhatIsNotUtf8) {
  EXPECT_TRUE(IsValidUtf8(""));
  EXPECT_TRUE(IsValidUtf8("plain"));
  EXPECT_TRUE(IsValidUtf8("na\xC3\xAFve \xF0\x9F\x99\x82"));
  EXPECT_FALSE(IsValidUtf8(kBadBytes));
  // A truncated sequence, an overlong encoding and a surrogate half: all three
  // are things a filesystem or a socket will hand over.
  EXPECT_FALSE(IsValidUtf8(std::string("\xE2\x82", 2)));
  EXPECT_FALSE(IsValidUtf8(std::string("\xC0\xAF", 2)));
  EXPECT_FALSE(IsValidUtf8(std::string("\xED\xA0\x80", 3)));
}

TEST(JsonCodecTest, FindsABadStringWhereverItIsNested) {
  EXPECT_EQ(FindUnencodableString(nlohmann::json{{"ok", "text"}}), nullptr);
  // A record's field and a list's element, because that is where a response
  // header's value and a directory entry's name actually live.
  nlohmann::json nested;
  nested["headers"]["etag"] = kBadBytes;
  EXPECT_NE(FindUnencodableString(nested), nullptr);
  nlohmann::json listed;
  listed["items"] = nlohmann::json::array({"fine", kBadBytes});
  EXPECT_NE(FindUnencodableString(listed), nullptr);
}

TEST(JsonCodecTest, DumpRefusesABadStringAndSaysWhatToDoInstead) {
  nlohmann::json value;
  value["etag"] = kBadBytes;
  const absl::StatusOr<std::string> refused =
      DumpJson(value, "a response header");
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.status().code(), absl::StatusCode::kInvalidArgument);
  // The message names the document and a way out, because "invalid argument"
  // alone leaves a caller with nothing to change.
  EXPECT_NE(refused.status().message().find("a response header"),
            std::string::npos);
  EXPECT_NE(refused.status().message().find("MessagePack"), std::string::npos);
}

TEST(JsonCodecTest, DumpStillWorksOnEverythingElse) {
  nlohmann::json value;
  value["n"] = 1;
  value["s"] = "na\xC3\xAFve";
  const absl::StatusOr<std::string> dumped = DumpJson(value, "a value");
  ASSERT_TRUE(dumped.ok()) << dumped.status();
  EXPECT_NE(dumped->find("na\xC3\xAFve"), std::string::npos);
  // The lossy form is what a log line uses, and it cannot fail.
  nlohmann::json bad;
  bad["etag"] = kBadBytes;
  EXPECT_FALSE(DumpJsonLossy(bad).empty());
}

}  // namespace
}  // namespace a11
