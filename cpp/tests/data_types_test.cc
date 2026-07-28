// Copyright 2026 The A11 Authors.

#include <string>

#include <gtest/gtest.h>

#include "a11/data/types.h"

namespace a11::data {
namespace {

TEST(DataTypesTest, WireMessageRoundTrips) {
  WireMessage message{
      .node_fragments = {{.id = "node",
                          .data = Chunk{.metadata =
                                            ChunkMetadata{
                                                .mimetype = "text/plain",
                                                .attributes = {{"lang", "en"}}},
                                        .data = "hello"},
                          .seq = 0,
                          .continued = false}},
      .actions = {{.id = "action", .name = "echo"}},
      .headers = {{"trace", "abc"}},
  };
  const absl::StatusOr<Bytes> encoded = message.ToMsgpack();
  ASSERT_TRUE(encoded.ok()) << encoded.status();
  const absl::StatusOr<WireMessage> decoded =
      WireMessage::FromMsgpack(*encoded);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(*decoded, message);
}

TEST(DataTypesTest, DecoderRejectsTrailingData) {
  absl::StatusOr<Bytes> encoded = WireMessage{}.ToMsgpack();
  ASSERT_TRUE(encoded.ok());
  encoded->push_back(static_cast<char>(0xc0));
  EXPECT_EQ(WireMessage::FromMsgpack(*encoded).status().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(DataTypesTest, NodeReferenceChecksBounds) {
  EXPECT_TRUE(
      (NodeRef{.id = "node", .offset = 4, .length = 8}).Validate().ok());
  EXPECT_EQ((NodeRef{.id = "node", .offset = 4, .length = 1ULL << 32U})
                .Validate()
                .code(),
            absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace a11::data
