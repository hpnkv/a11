// Copyright 2026 The A11 Authors.

/** @file
 * @brief Compares UnpackMsgpack with nlohmann::json::from_msgpack across
 * MessagePack size classes and representations.
 */

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/statusor.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a11/json_codec.h"

namespace a11 {
namespace {

using Json = nlohmann::json;

std::string ToMsgpack(const Json& value) {
  const std::vector<std::uint8_t> encoded = Json::to_msgpack(value);
  return std::string(reinterpret_cast<const char*>(encoded.data()),
                     encoded.size());
}

Json FromNlohmann(std::string_view bytes) {
  const auto* first = reinterpret_cast<const std::uint8_t*>(bytes.data());
  return Json::from_msgpack(first, first + bytes.size(), /*strict=*/true,
                            /*allow_exceptions=*/false);
}

// Compare value and storage type because JSON equality merges some numeric
// types and same-byte strings and binaries.
void ExpectSameAsNlohmann(std::string_view bytes, std::string_view label) {
  const Json expected = FromNlohmann(bytes);
  ASSERT_FALSE(expected.is_discarded()) << label << ": nlohmann rejected these";
  const absl::StatusOr<Json> actual = UnpackMsgpack(bytes, "a value");
  ASSERT_TRUE(actual.ok()) << label << ": " << actual.status().message();
  EXPECT_EQ(*actual, expected) << label;
  EXPECT_EQ(actual->type_name(), expected.type_name()) << label;
  EXPECT_EQ(actual->is_binary(), expected.is_binary()) << label;
  EXPECT_EQ(actual->is_number_unsigned(), expected.is_number_unsigned())
      << label;
  EXPECT_EQ(actual->is_number_integer(), expected.is_number_integer()) << label;
  EXPECT_EQ(actual->is_number_float(), expected.is_number_float()) << label;
  if (expected.is_binary()) {
    EXPECT_EQ(actual->get_binary().has_subtype(),
              expected.get_binary().has_subtype())
        << label;
    if (expected.get_binary().has_subtype()) {
      EXPECT_EQ(actual->get_binary().subtype(), expected.get_binary().subtype())
          << label;
    }
  }
}

// The same, for a value nlohmann's encoder can produce the bytes for.
void ExpectEncodedMatch(const Json& value, std::string_view label) {
  ExpectSameAsNlohmann(ToMsgpack(value), label);
}

std::string Repeat(size_t length, char character = 'x') {
  return std::string(length, character);
}

TEST(MsgpackDecodeTest, StringsMatchAcrossEverySizeClass) {
  // fixstr up to 31, str8 to 255, str16 to 65535, str32 beyond.
  for (const size_t length : {size_t{0}, size_t{1}, size_t{31}, size_t{32},
                              size_t{255}, size_t{256}, size_t{65535},
                              size_t{65536}, size_t{65536 + 1}}) {
    ExpectEncodedMatch(Json(Repeat(length)),
                         "string of " + std::to_string(length));
  }
}

TEST(MsgpackDecodeTest, BinaryMatchesAcrossEverySizeClass) {
  for (const size_t length : {size_t{0}, size_t{1}, size_t{255}, size_t{256},
                              size_t{65535}, size_t{65536}}) {
    const std::string payload = Repeat(length, '\xfe');
    ExpectEncodedMatch(
        Json::binary(std::vector<std::uint8_t>(payload.begin(), payload.end())),
        "binary of " + std::to_string(length));
  }
}

TEST(MsgpackDecodeTest, A64KiBStringAndBinaryDecodeToTheSameValue) {
  // The payload size `data/chunk_codec` measures.
  ExpectEncodedMatch(Json{{"id", "bench"}, {"body", Repeat(65536 - 24)}},
                       "the benchmark's 64 KiB object");
  const std::string payload = Repeat(65536, '\x01');
  ExpectEncodedMatch(
      Json::binary(std::vector<std::uint8_t>(payload.begin(), payload.end())),
      "64 KiB of binary");
}

TEST(MsgpackDecodeTest, UnsignedIntegersMatchAcrossEverySizeClass) {
  const std::vector<std::uint64_t> values = {
      0,
      1,
      127,
      128,
      255,
      256,
      65535,
      65536,
      4294967295,
      4294967296,
      1700000000000000ULL,
      std::numeric_limits<std::uint64_t>::max()};
  for (const std::uint64_t value : values) {
    ExpectEncodedMatch(Json(value), "unsigned " + std::to_string(value));
  }
}

TEST(MsgpackDecodeTest, SignedIntegersMatchAcrossEverySizeClass) {
  const std::vector<std::int64_t> values = {
      -1,
      -32,
      -33,
      -128,
      -129,
      -32768,
      -32769,
      -2147483648LL,
      -2147483649LL,
      std::numeric_limits<std::int64_t>::min()};
  for (const std::int64_t value : values) {
    ExpectEncodedMatch(Json(value), "signed " + std::to_string(value));
  }
}

TEST(MsgpackDecodeTest, FloatsMatchBothWidths) {
  for (const double value : {0.0, -0.0, 1.5, -1.5, 3.141592653589793,
                             1e-300, 1e300}) {
    ExpectEncodedMatch(Json(value), "double " + std::to_string(value));
  }
  // nlohmann encodes doubles as float64. This manual value covers float32.
  const std::string float32 =
      std::string("\xca", 1) + std::string("\x3f\xc0\x00\x00", 4);
  ExpectSameAsNlohmann(float32, "float32 1.5");
}

TEST(MsgpackDecodeTest, NilAndBooleansMatch) {
  ExpectEncodedMatch(Json(nullptr), "nil");
  ExpectEncodedMatch(Json(true), "true");
  ExpectEncodedMatch(Json(false), "false");
}

TEST(MsgpackDecodeTest, ArraysAndMapsMatchAcrossEverySizeClass) {
  for (const size_t count : {size_t{0}, size_t{1}, size_t{15}, size_t{16},
                             size_t{65535}, size_t{65536}}) {
    Json array = Json::array();
    for (size_t index = 0; index < count; ++index) {
      array.push_back(index % 128);
    }
    ExpectEncodedMatch(array, "array of " + std::to_string(count));
  }
  for (const size_t count : {size_t{0}, size_t{1}, size_t{15}, size_t{16},
                             size_t{1000}}) {
    Json object = Json::object();
    for (size_t index = 0; index < count; ++index) {
      object["k" + std::to_string(index)] = index;
    }
    ExpectEncodedMatch(object, "map of " + std::to_string(count));
  }
}

TEST(MsgpackDecodeTest, NestedContainersMatch) {
  ExpectEncodedMatch(
      Json{{"a", Json::array({1, -1, 1.5, nullptr, true, "s"})},
           {"b", Json{{"c", Json::array({Json{{"d", Json::array()}}})}}},
           {"e", Json::object()}},
      "a nested document");
}

TEST(MsgpackDecodeTest, ExtensionsCarryTheirSubtype) {
  // fixext1, 2, 4, 8, 16 then ext8, ext16, ext32.
  struct Case {
    std::uint8_t marker;
    size_t payload_length;
  };
  const std::vector<Case> fixed = {
      {0xd4, 1}, {0xd5, 2}, {0xd6, 4}, {0xd7, 8}, {0xd8, 16}};
  for (const Case& one : fixed) {
    const std::string bytes = std::string(1, static_cast<char>(one.marker)) +
                              std::string(1, '\x2a') +  // subtype 42
                              Repeat(one.payload_length, '\x07');
    ExpectSameAsNlohmann(bytes, "fixext of " +
                                    std::to_string(one.payload_length));
  }

  const std::string ext8 = std::string("\xc7", 1) + std::string(1, '\x03') +
                           std::string(1, '\xff') + Repeat(3, '\x07');
  ExpectSameAsNlohmann(ext8, "ext8 subtype 255");

  std::string ext16 = std::string("\xc8", 1);
  ext16 += std::string("\x01\x00", 2);  // 256 bytes
  ext16 += std::string(1, '\x01');
  ext16 += Repeat(256, '\x09');
  ExpectSameAsNlohmann(ext16, "ext16");

  std::string ext32 = std::string("\xc9", 1);
  ext32 += std::string("\x00\x01\x00\x00", 4);  // 65536 bytes
  ext32 += std::string(1, '\x02');
  ext32 += Repeat(65536, '\x0a');
  ExpectSameAsNlohmann(ext32, "ext32");
}

TEST(MsgpackDecodeTest, ForcedSizeClassesDecodeTheSameWay) {
  // A value encoded in a wider class than it needs is legal MessagePack, and a
  // peer in another language may pick one. str16 for one character, array32 for
  // one element, map16 for one entry, uint64 for zero.
  ExpectSameAsNlohmann(std::string("\xda", 1) + std::string("\x00\x01", 2) +
                           "z",
                       "str16 holding one byte");
  ExpectSameAsNlohmann(std::string("\xdd", 1) +
                           std::string("\x00\x00\x00\x01", 4) +
                           std::string(1, '\x05'),
                       "array32 holding one element");
  ExpectSameAsNlohmann(std::string("\xde", 1) + std::string("\x00\x01", 2) +
                           std::string("\xa1", 1) + "k" +
                           std::string(1, '\x05'),
                       "map16 holding one entry");
  ExpectSameAsNlohmann(std::string("\xcf", 1) +
                           std::string("\x00\x00\x00\x00\x00\x00\x00\x00", 8),
                       "uint64 holding zero");
  ExpectSameAsNlohmann(std::string("\xd3", 1) +
                           std::string("\x00\x00\x00\x00\x00\x00\x00\x01", 8),
                       "int64 holding one");
}

TEST(MsgpackDecodeTest, ARepeatedKeyKeepsTheLastValue) {
  const std::string bytes = std::string("\x82", 1) +      // fixmap of 2
                            std::string("\xa1", 1) + "k" + std::string(1, '\x01') +
                            std::string("\xa1", 1) + "k" + std::string(1, '\x02');
  ExpectSameAsNlohmann(bytes, "a map with a repeated key");
}

TEST(MsgpackDecodeTest, TruncatedInputIsRejected) {
  const std::string complete = ToMsgpack(Json{{"body", Repeat(300)}});
  for (const size_t length : {size_t{0}, size_t{1}, size_t{2}, size_t{5},
                              complete.size() - 1}) {
    const absl::StatusOr<Json> decoded =
        UnpackMsgpack(std::string_view(complete).substr(0, length), "a value");
    EXPECT_FALSE(decoded.ok()) << "truncated to " << length;
  }
}

TEST(MsgpackDecodeTest, TrailingBytesAreRejected) {
  const std::string one = ToMsgpack(Json(1));
  EXPECT_TRUE(UnpackMsgpack(one, "a value").ok());
  EXPECT_FALSE(UnpackMsgpack(one + one, "a value").ok());
  EXPECT_FALSE(UnpackMsgpack(one + std::string(1, '\x00'), "a value").ok());
}

TEST(MsgpackDecodeTest, UnsupportedAndMistypedMarkersAreRejected) {
  // 0xc1 is never a valid marker.
  EXPECT_FALSE(UnpackMsgpack(std::string("\xc1", 1), "a value").ok());
  // A map key has to be a MessagePack string, which nlohmann also requires.
  const std::string integer_key = std::string("\x81", 1) +
                                  std::string(1, '\x01') +
                                  std::string(1, '\x02');
  EXPECT_TRUE(FromNlohmann(integer_key).is_discarded());
  EXPECT_FALSE(UnpackMsgpack(integer_key, "a value").ok());
}

TEST(MsgpackDecodeTest, NestingPastTheCapIsRejected) {
  // 300 nested one-element arrays, past the 256 the scanner allows.
  const std::string deep = Repeat(300, '\x91') + std::string(1, '\xc0');
  EXPECT_FALSE(UnpackMsgpack(deep, "a value").ok());
  // 200 of them decode.
  const std::string shallow = Repeat(200, '\x91') + std::string(1, '\xc0');
  ExpectSameAsNlohmann(shallow, "200 nested arrays");
}

TEST(MsgpackDecodeTest, TheEncoderRoundTripsThroughTheDecoder) {
  const Json value{{"text", "hello"},
                   {"count", 7},
                   {"negative", -7},
                   {"ratio", 0.25},
                   {"flag", true},
                   {"nothing", nullptr},
                   {"list", Json::array({1, 2, 3})},
                   {"nested", Json{{"deep", Repeat(70000)}}}};
  const absl::StatusOr<std::string> encoded = PackMsgpack(value, "a value");
  ASSERT_TRUE(encoded.ok()) << encoded.status().message();
  EXPECT_EQ(*encoded, ToMsgpack(value));
  const absl::StatusOr<Json> decoded = UnpackMsgpack(*encoded, "a value");
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();
  EXPECT_EQ(*decoded, value);
}

}  // namespace
}  // namespace a11
