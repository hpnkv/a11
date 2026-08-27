// Copyright 2026 The A11 Authors.

#include "a11/uuid.h"

#include <string>
#include <string_view>
#include <unordered_set>

#include <gtest/gtest.h>

#include "a11/data/types.h"

namespace {

bool IsLowercaseHex(std::string_view text) {
  for (const char one : text) {
    const bool digit = one >= '0' && one <= '9';
    const bool letter = one >= 'a' && one <= 'f';
    if (!digit && !letter) {
      return false;
    }
  }
  return !text.empty();
}

TEST(NewShortIdTest, IsTwelveHexDigitsByDefault) {
  const std::string id = a11::NewShortId();
  EXPECT_EQ(id.size(), 12U);
  EXPECT_TRUE(IsLowercaseHex(id)) << id;
}

TEST(NewShortIdTest, IsAValidName) {
  // The reason every character is a hex digit: an action id has to pass
  // ValidateName, and a node id is "<action id>#<port>", which every parser
  // splits at the single '#'.
  for (int digits = 8; digits <= 16; ++digits) {
    const std::string id = a11::NewShortId(digits);
    ASSERT_EQ(id.size(), static_cast<std::size_t>(digits)) << digits;
    EXPECT_TRUE(a11::data::ValidateName(id).ok()) << id;
    EXPECT_EQ(id.find('#'), std::string::npos) << id;
    EXPECT_EQ(id.find('-'), std::string::npos) << id;
  }
}

TEST(NewShortIdTest, ClampsTheRequestedWidth) {
  EXPECT_EQ(a11::NewShortId(1).size(), 8U);
  EXPECT_EQ(a11::NewShortId(64).size(), 16U);
}

TEST(NewShortIdTest, DoesNotRepeatItselfInBulk) {
  // A repeat among one million 48-bit ids indicates a faulty generator.
  constexpr int kCount = 1000000;
  std::unordered_set<std::string> seen;
  seen.reserve(kCount);
  for (int index = 0; index < kCount; ++index) {
    seen.insert(a11::NewShortId());
  }
  EXPECT_EQ(seen.size(), static_cast<std::size_t>(kCount));
}

TEST(NewUuidTest, StaysAFullUuid) {
  // Shortening action ids left this alone: sessions, streams and the `new_uuid`
  // action all promise a UUID.
  const std::string id = a11::NewUuid();
  EXPECT_EQ(id.size(), 36U);
  EXPECT_EQ(id[8], '-');
  EXPECT_EQ(id[13], '-');
  EXPECT_EQ(id[14], '4');
  EXPECT_EQ(id[18], '-');
  EXPECT_EQ(id[23], '-');
  EXPECT_TRUE(a11::data::ValidateName(id).ok()) << id;
}

}  // namespace
