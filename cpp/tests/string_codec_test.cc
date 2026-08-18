// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief
 *   How a std::string travels: as bytes by default, as text when asked.
 *
 * C++ has no type that means "text" rather than "bytes" -- a `std::string` is a
 * sequence of bytes, and whether those bytes are UTF-8 is a fact about the value
 * and not about its type. So `application/octet-stream` is the default here and
 * `text/plain` is available by naming it, which is the only place the
 * distinction can live. Python, Kotlin and TypeScript all have the distinction
 * in their type systems and take `text/plain` for their string type instead.
 *
 * Both media types describe their content entirely, so neither chunk carries a
 * `;type=` parameter, and neither payload is framed: the bytes on the wire are
 * the value. That is the whole reason for these codecs -- the JSON
 * representation of a string is a quoted, escaped copy, and of bytes a base64
 * copy a third larger again.
 */

#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/strings/escaping.h>
#include <absl/status/statusor.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "a11/data/serialization.h"
#include "a11/data/types.h"

namespace a11::data {
namespace {

SerializationRegistry& Registry() { return GlobalSerializationRegistry(); }

TEST(StringCodecTest, AStringIsBytesByDefaultAndCarriesNoTag) {
  const std::string value = "hello";

  absl::StatusOr<Chunk> chunk = Registry().ToChunk<std::string>(value);

  ASSERT_TRUE(chunk.ok()) << chunk.status();
  ASSERT_TRUE(chunk->metadata.has_value());
  // Bare: no ";type=", because the media type already says what this is.
  EXPECT_EQ(chunk->metadata->mimetype, kBytesMimetype);
  // And the payload is the value, not a quoted or base64 copy of it.
  EXPECT_EQ(chunk->data, value);
}

TEST(StringCodecTest, TextPlainIsAvailableByAskingForIt) {
  const std::string value = "hello";

  absl::StatusOr<Chunk> chunk =
      Registry().ToChunk<std::string>(value, std::string(kTextMimetype));

  ASSERT_TRUE(chunk.ok()) << chunk.status();
  ASSERT_TRUE(chunk->metadata.has_value());
  EXPECT_EQ(chunk->metadata->mimetype, kTextMimetype);
  EXPECT_EQ(chunk->data, value);
}

TEST(StringCodecTest, BothRoundTrip) {
  for (const std::string_view mimetype : {kBytesMimetype, kTextMimetype}) {
    const std::string value = "round trip";
    absl::StatusOr<Chunk> chunk =
        Registry().ToChunk<std::string>(value, std::string(mimetype));
    ASSERT_TRUE(chunk.ok()) << mimetype << ": " << chunk.status();

    absl::StatusOr<std::string> decoded =
        Registry().FromChunk<std::string>(*chunk);

    ASSERT_TRUE(decoded.ok()) << mimetype << ": " << decoded.status();
    EXPECT_EQ(*decoded, value) << mimetype;
  }
}

TEST(StringCodecTest, ArbitraryBytesSurviveTheDefaultRepresentation) {
  // The case the default exists for: bytes that are not text at all, including
  // an embedded NUL, which a length-carrying payload must not truncate.
  const std::string value("\x00\x01\xff\xfe binary\x00", 12);

  absl::StatusOr<Chunk> chunk = Registry().ToChunk<std::string>(value);
  ASSERT_TRUE(chunk.ok()) << chunk.status();
  absl::StatusOr<std::string> decoded =
      Registry().FromChunk<std::string>(*chunk);

  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(*decoded, value);
  EXPECT_EQ(decoded->size(), value.size());
}

TEST(StringCodecTest, TextPlainRefusesBytesThatArePeersCannotDecode) {
  // Rejected where the value came from, rather than in a peer's decoder one hop
  // away. Every other A11 language's string type refuses these bytes.
  const std::string cases[] = {
      std::string("\xff", 1),              // never a lead byte
      std::string("\xc0\x80", 2),          // overlong encoding of NUL
      std::string("\xed\xa0\x80", 3),      // a UTF-16 surrogate half
      std::string("\xf4\x90\x80\x80", 4),  // above U+10FFFF
      std::string("\xe2\x82", 2),          // truncated three-byte sequence
      std::string("\x80", 1),              // a continuation byte with no lead
  };
  for (const std::string& value : cases) {
    absl::StatusOr<Chunk> chunk =
        Registry().ToChunk<std::string>(value, std::string(kTextMimetype));
    EXPECT_FALSE(chunk.ok())
        << "accepted invalid UTF-8: " << absl::CHexEscape(value);
    // The same bytes are perfectly fine as bytes.
    EXPECT_TRUE(Registry().ToChunk<std::string>(value).ok())
        << absl::CHexEscape(value);
  }
}

TEST(StringCodecTest, ValidMultiByteTextIsAccepted) {
  const std::string cases[] = {
      "",                            // empty is valid
      "ascii",                       //
      "\xc2\xa3",                    // U+00A3, two bytes
      "\xe2\x82\xac",                // U+20AC, three bytes
      "\xf0\x9f\x92\xa1",            // U+1F4A1, four bytes
      "mixed \xc2\xa3 and \xf0\x9f\x92\xa1",
  };
  for (const std::string& value : cases) {
    absl::StatusOr<Chunk> chunk =
        Registry().ToChunk<std::string>(value, std::string(kTextMimetype));
    ASSERT_TRUE(chunk.ok()) << absl::CHexEscape(value) << ": "
                            << chunk.status();
    absl::StatusOr<std::string> decoded =
        Registry().FromChunk<std::string>(*chunk);
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    EXPECT_EQ(*decoded, value);
  }
}

TEST(StringCodecTest, TheJsonRepresentationIsStillThereWhenAskedFor) {
  // Changing the default must not remove a representation. A peer that speaks
  // only JSON is still served, and still understood.
  absl::StatusOr<Chunk> chunk = Registry().ToChunk<nlohmann::json>(
      nlohmann::json("hello"), std::string(kJsonMimetype));

  ASSERT_TRUE(chunk.ok()) << chunk.status();
  ASSERT_TRUE(chunk->metadata.has_value());
  EXPECT_EQ(chunk->metadata->mimetype, kJsonMimetype);
  EXPECT_EQ(chunk->data, "\"hello\"");
}

}  // namespace
}  // namespace a11::data
