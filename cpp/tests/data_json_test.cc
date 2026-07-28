// Copyright 2026 The A11 Authors.

#include <cstdint>
#include <string>

#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/data/json.h"
#include "a11/data/types.h"

namespace a11::data {
namespace {

TEST(DataJsonTest, RoundTripsPythonSseRepresentation) {
  WireMessage message;
  message.headers.emplace("wire", std::string("\0\xff", 2));
  message.node_fragments.push_back(NodeFragment{
      .id = "node",
      .data =
          Chunk{.metadata =
                    ChunkMetadata{
                        .mimetype = "application/octet-stream",
                        .timestamp = absl::FromUnixMicros(1234567),
                        .attributes = {{"attribute", std::string("\1\2", 2)}}},
                .ref = "",
                .data = std::string("payload\0", 8)},
      .seq = 4,
      .continued = true});
  message.node_fragments.push_back(
      NodeFragment{.id = "reference",
                   .data = NodeRef{.id = "node", .offset = 2, .length = 3},
                   .seq = 0,
                   .continued = false});
  message.actions.push_back(
      ActionMessage{.id = "action",
                    .name = "echo",
                    .inputs = {Port{.name = "input", .id = "node"}},
                    .outputs = {Port{.name = "output", .id = "result"}},
                    .headers = {{"binary", std::string("\0", 1)}}});

  auto encoded = WireMessageToJson(message);
  ASSERT_TRUE(encoded.ok()) << encoded.status();
  EXPECT_NE(encoded->find("cGF5bG9hZAA="), std::string::npos);
  EXPECT_NE(encoded->find("1970-01-01T00:00:01.234567+00:00"),
            std::string::npos);

  auto decoded = WireMessageFromJson(*encoded);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(*decoded, message);
}

TEST(DataJsonTest, AcceptsPythonOmittedDefaults) {
  auto decoded = WireMessageFromJson(
      R"({"node_fragments":[{"data":{}},{"data":{"id":"target"}}]})");
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  ASSERT_EQ(decoded->node_fragments.size(), 2);
  EXPECT_TRUE(std::holds_alternative<Chunk>(decoded->node_fragments[0].data));
  EXPECT_TRUE(std::holds_alternative<NodeRef>(decoded->node_fragments[1].data));
}

}  // namespace
}  // namespace a11::data
