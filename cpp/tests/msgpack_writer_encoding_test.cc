// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief
 *   The typed MsgpackWriter field writers encode exactly what JSON did.
 *
 * `MsgpackWriter` grew direct writers -- `PackUint`, `PackString`, `PackBinary`
 * and friends -- so the wire path could stop building a `nlohmann::json` value
 * and an intermediate `std::string` for every field it emits. That is only a
 * safe change if the bytes are identical, because the format is pinned across
 * four language implementations and A11 speaks it to peers it did not build.
 *
 * So each test here encodes the same value both ways and compares the bytes.
 * The interesting cases are the boundaries between MessagePack size classes --
 * fixint/uint8/uint16/uint32/uint64, fixstr/str8/str16, bin8/bin16, and the
 * negative-integer forms -- because picking a different class for the same value
 * is legal MessagePack, decodes correctly, and is still a wire change.
 */

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a11/data/msgpack.h"

namespace a11::data {
namespace {

// What the old path produced: a JSON value through MsgpackWriter::Pack.
std::string ViaJson(const nlohmann::json& value) {
  MsgpackWriter writer;
  EXPECT_TRUE(writer.Pack(value).ok());
  return writer.TakeBytes();
}

TEST(MsgpackWriterEncodingTest, UnsignedIntegersMatchAcrossEverySizeClass) {
  const std::vector<std::uint64_t> values = {
      0,       1,          127,        128,        255,
      256,     65535,      65536,      4294967295, 4294967296,
      1'700'000'000'000'000ULL,  // a microsecond timestamp, the real case
      std::numeric_limits<std::uint64_t>::max()};
  for (const std::uint64_t value : values) {
    MsgpackWriter writer;
    writer.PackUint(value);
    EXPECT_EQ(writer.TakeBytes(), ViaJson(nlohmann::json(value)))
        << "value " << value;
  }
}

TEST(MsgpackWriterEncodingTest, SignedIntegersMatchAcrossEverySizeClass) {
  const std::vector<std::int64_t> values = {
      0,    1,      127,    128,     -1,      -32,
      -33,  -128,   -129,   -32768,  -32769,  -2147483648LL,
      -2147483649LL, std::numeric_limits<std::int64_t>::min(),
      std::numeric_limits<std::int64_t>::max()};
  for (const std::int64_t value : values) {
    MsgpackWriter writer;
    writer.PackInt(value);
    EXPECT_EQ(writer.TakeBytes(), ViaJson(nlohmann::json(value)))
        << "value " << value;
  }
}

TEST(MsgpackWriterEncodingTest, StringsMatchAcrossEverySizeClass) {
  for (const size_t length : {size_t{0}, size_t{1}, size_t{31}, size_t{32},
                             size_t{255}, size_t{256}, size_t{65535},
                             size_t{65536}}) {
    const std::string value(length, 'x');
    MsgpackWriter writer;
    writer.PackString(value);
    EXPECT_EQ(writer.TakeBytes(), ViaJson(nlohmann::json(value)))
        << "length " << length;
  }
}

TEST(MsgpackWriterEncodingTest, BinaryMatchesAcrossEverySizeClass) {
  for (const size_t length : {size_t{0}, size_t{1}, size_t{255}, size_t{256},
                             size_t{65535}, size_t{65536}}) {
    const std::string value(length, '\x7f');
    MsgpackWriter writer;
    writer.PackBinary(value);
    EXPECT_EQ(writer.TakeBytes(), ViaJson(Binary(value))) << "length " << length;
  }
}

TEST(MsgpackWriterEncodingTest, NilAndBooleansMatch) {
  MsgpackWriter nil;
  nil.PackNil();
  EXPECT_EQ(nil.TakeBytes(), ViaJson(nlohmann::json(nullptr)));

  MsgpackWriter yes;
  yes.PackBool(true);
  EXPECT_EQ(yes.TakeBytes(), ViaJson(nlohmann::json(true)));

  MsgpackWriter no;
  no.PackBool(false);
  EXPECT_EQ(no.TakeBytes(), ViaJson(nlohmann::json(false)));
}

TEST(MsgpackWriterEncodingTest, ArrayOfBinaryRecordsMatches) {
  // The shape WireMessage uses for its fragment and action lists: an array
  // header followed by one binary field per element.
  for (const size_t count : {size_t{0}, size_t{1}, size_t{15}, size_t{16},
                             size_t{17}}) {
    nlohmann::json array = nlohmann::json::array();
    MsgpackWriter writer;
    writer.PackArrayHeader(count);
    for (size_t index = 0; index < count; ++index) {
      const std::string element = "record-" + std::to_string(index);
      writer.PackBinary(element);
      array.push_back(Binary(element));
    }
    EXPECT_EQ(writer.TakeBytes(), ViaJson(array)) << "count " << count;
  }
}

TEST(MsgpackWriterEncodingTest, MapOfBinaryValuesMatches) {
  // The shape a ByteMap takes. nlohmann's object is ordered, so the direct
  // writer has to emit sorted keys to match it -- which is also what makes the
  // encoding deterministic, since a ByteMap is a hash map.
  for (const size_t count : {size_t{0}, size_t{1}, size_t{15}, size_t{16}}) {
    nlohmann::json object = nlohmann::json::object();
    MsgpackWriter writer;
    writer.PackMapHeader(count);
    for (size_t index = 0; index < count; ++index) {
      // Zero-padded so lexical and numeric order agree for this fixture.
      const std::string key = "k" + std::string(2 - std::to_string(index).size(),
                                                '0') +
                              std::to_string(index);
      const std::string value = "v" + std::to_string(index);
      writer.PackString(key);
      writer.PackBinary(value);
      object[key] = Binary(value);
    }
    EXPECT_EQ(writer.TakeBytes(), ViaJson(object)) << "count " << count;
  }
}

TEST(MsgpackWriterEncodingTest, PackRecordMatchesEncodingThenEmbedding) {
  // PackRecord's whole purpose is to skip the intermediate buffer, so what it
  // must prove is that skipping it changes nothing.
  MsgpackWriter direct;
  ASSERT_TRUE(direct
                  .PackRecord([](MsgpackWriter* child) {
                    child->PackString("node-1");
                    child->PackUint(7);
                    return absl::OkStatus();
                  })
                  .ok());

  MsgpackWriter inner;
  inner.PackString("node-1");
  inner.PackUint(7);
  MsgpackWriter outer;
  outer.PackBinary(inner.bytes());

  EXPECT_EQ(direct.TakeBytes(), outer.TakeBytes());
}

TEST(MsgpackWriterEncodingTest, PackRecordPropagatesFailure) {
  MsgpackWriter writer;
  const absl::Status status = writer.PackRecord([](MsgpackWriter*) {
    return absl::InvalidArgumentError("child failed");
  });
  EXPECT_TRUE(absl::IsInvalidArgument(status)) << status;
  // Nothing half-written: a failed child must not leave a binary header behind.
  EXPECT_TRUE(writer.bytes().empty());
}

TEST(MsgpackWriterEncodingTest, BorrowedBufferAppendsRatherThanReplaces) {
  std::string target = "existing";
  MsgpackWriter writer(&target);
  writer.PackUint(1);
  EXPECT_EQ(target, std::string("existing\x01", 9));
}

}  // namespace
}  // namespace a11::data
