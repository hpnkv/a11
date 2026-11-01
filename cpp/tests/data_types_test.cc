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

TEST(DataTypesTest, StorageQualifiedNodeReferencesRoundTrip) {
  StorageIdentity identity{
      .id = "shared",
      .services = {StorageService{
          .role = "stream",
          .endpoint = "https://storage.example",
          .credentials = StorageCredentials{
              .scheme = "bearer", .fields = {{"token", "test-token"}}}}}};
  NodeRef original{
      .id = "node", .offset = 17, .length = 3, .storage = identity};
  auto encoded = original.ToMsgpack();
  ASSERT_TRUE(encoded.ok()) << encoded.status();
  auto decoded = NodeRef::FromMsgpack(*encoded);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(*decoded, original);
  EXPECT_EQ(decoded->offset, 17);
  EXPECT_EQ(decoded->length, 3);
  EXPECT_EQ(original.DebugString().find("test-token"), std::string::npos);
  original.storage.reset();
  auto local_bytes = original.ToMsgpack();
  ASSERT_TRUE(local_bytes.ok());
  EXPECT_EQ(*NodeRef::FromMsgpack(*local_bytes), original);
}

TEST(DataTypesTest, NodeReferenceCredentialsRemainTypedAndOptional) {
  const NodeRef local{.id = "local"};
  auto local_bytes = local.ToMsgpack();
  ASSERT_TRUE(local_bytes.ok()) << local_bytes.status();
  auto decoded_local = NodeRef::FromMsgpack(*local_bytes);
  ASSERT_TRUE(decoded_local.ok()) << decoded_local.status();
  EXPECT_FALSE(decoded_local->storage.has_value());
  EXPECT_EQ(local.WithCredentials(StorageCredentials{.scheme = "bearer"})
                .status()
                .code(),
            absl::StatusCode::kFailedPrecondition);

  const NodeRef qualified{
      .id = "remote",
      .storage = StorageIdentity{
          .id = "shared",
          .services = {StorageService{.role = "payload",
                                      .endpoint = "https://storage.example"}}}};
  auto with_namespace = qualified.WithCredentials(
      StorageCredentials{.scheme = "bearer", .fields = {{"token", "secret"}}});
  ASSERT_TRUE(with_namespace.ok()) << with_namespace.status();
  EXPECT_FALSE(qualified.storage->credentials.has_value());
  EXPECT_EQ(with_namespace->storage->credentials->fields.at("token"), "secret");
  auto with_service = with_namespace->WithCredentials(
      StorageCredentials{.scheme = "bearer", .fields = {{"token", "other"}}},
      "payload");
  ASSERT_TRUE(with_service.ok()) << with_service.status();
  auto encoded = with_service->ToMsgpack();
  ASSERT_TRUE(encoded.ok()) << encoded.status();
  EXPECT_EQ(*NodeRef::FromMsgpack(*encoded), *with_service);
  EXPECT_EQ(with_service->DebugString().find("secret"), std::string::npos);
  EXPECT_EQ(with_service->DebugString().find("other"), std::string::npos);
}

}  // namespace
}  // namespace a11::data
